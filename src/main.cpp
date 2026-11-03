#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <esp_task_wdt.h>

#include "config.h"
#include "schedule_module.h"
#include "refill_module.h"
#include "gsm_module.h"
#include "cmd_parser.h"

// Nordic UART BLE UUIDs for Mobile App Communication
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ---------------------------------------------------------------------------
// THREADING MODEL
// BLE callbacks alag FreeRTOS task (Bluedroid BTC) me chalte hain. Wo sirf bytes StreamBuffer me
// daalte hain aur volatile flags set karte hain; saara processing loop() (single task) me hota hai.
// StreamBuffer: single writer (BTC task) + single reader (loop task) -> lock-free safe.
// ---------------------------------------------------------------------------
static BLEServer*          pServer = nullptr;
static BLECharacteristic*  pTxCharacteristic = nullptr;
static StreamBufferHandle_t bleRxStream = nullptr;

static volatile bool     deviceConnected = false;
static volatile bool     bleDisconnectEvent = false;
static volatile uint32_t bleDisconnectMs = 0;
static volatile uint16_t bleMtu = 23;
static volatile bool     bleRxDropPending = false;  // ek BLE write drop hua: agle write se pehle 0x00 marker
static volatile uint32_t bleRxDropped = 0;
static bool              bleAdvPending = false;

// ----- command assembler (UART aur BLE dono ke liye; bounded buffer) -----
#define CMD_BUF_MAX 96
struct CmdAssembler {
    char buf[CMD_BUF_MAX];
    uint8_t len;
    bool discarding;   // oversized frame: uske '#' tak sab discard
};
static CmdAssembler uartCmd = {{0}, 0, false};
static CmdAssembler bleCmd  = {{0}, 0, false};

static void resetAssembler(CmdAssembler& a) { a.len = 0; a.discarding = false; a.buf[0] = 0; }

// true tab jab '#' par command complete ho. CR/LF/NUL = frame boundary (NUL = BLE drop marker).
// Oversized frame ka koi hissa kabhi process nahi hota: pehle buffer wrap karke uska tail
// (jaise "...*$111,1,#") naya valid command ban jata tha.
static bool feedCmdChar(CmdAssembler& a, char c) {
    if (c == '\n' || c == '\r' || c == '\0') { resetAssembler(a); return false; }
    if (a.discarding) {
        if (c == '#') resetAssembler(a);
        return false;
    }
    if (a.len >= CMD_BUF_MAX - 1) {
        resetAssembler(a);
        a.discarding = (c != '#');
        return false;
    }
    a.buf[a.len++] = c;
    a.buf[a.len] = 0;
    return c == '#';
}

// ----- BLE TX queue: replies MTU-3 ke chunks me bhejte hain (app "&@ ... -!" tak reassemble karta hai) -----
#define BLE_TX_SLOTS 4
#define BLE_TX_GAP_MS 12
#define BLE_TX_RETRY_GAP_MS 25
#define BLE_TX_MAX_RETRIES 25
static String   bleTxMsg[BLE_TX_SLOTS];
static uint8_t  bleTxHead = 0, bleTxCount = 0;
static size_t   bleTxOffset = 0;
static uint32_t bleLastTxMs = 0;
static uint32_t bleTxGapMs = BLE_TX_GAP_MS;
static uint8_t  bleTxRetries = 0;
static int      bleTxLastStatus = -1;   // notify() ke andar synchronously onStatus se set (loop task)

void sendBleNotification(const String& data) {
    if (!deviceConnected || data.length() == 0) return;
    if (bleTxCount >= BLE_TX_SLOTS) { Serial.println("[BLE] TX queue full, reply dropped"); return; }
    bleTxMsg[(bleTxHead + bleTxCount) % BLE_TX_SLOTS] = data;
    bleTxCount++;
}

static void bleTxPop() {
    bleTxMsg[bleTxHead] = "";
    bleTxHead = (uint8_t)((bleTxHead + 1) % BLE_TX_SLOTS);
    bleTxCount--;
    bleTxOffset = 0;
    bleTxRetries = 0;
    bleTxGapMs = BLE_TX_GAP_MS;
}

static void pumpBleTx() {
    if (!deviceConnected) { // disconnect: pending replies discard
        for (int i = 0; i < BLE_TX_SLOTS; i++) bleTxMsg[i] = "";
        bleTxHead = bleTxCount = 0;
        bleTxOffset = 0;
        bleTxRetries = 0;
        bleTxGapMs = BLE_TX_GAP_MS;
        return;
    }
    if (bleTxCount == 0 || pTxCharacteristic == nullptr) return;
    if ((millis() - bleLastTxMs) < bleTxGapMs) return; // notify congestion se bachne ke liye pacing

    uint16_t mtu = bleMtu;
    size_t chunk = (mtu > 23) ? (size_t)(mtu - 3) : 20;
    if (chunk > 180) chunk = 180;

    const String& m = bleTxMsg[bleTxHead];
    size_t n = m.length() - bleTxOffset;
    if (n > chunk) n = chunk;

    bleTxLastStatus = -1;
    pTxCharacteristic->setValue((uint8_t*)m.c_str() + bleTxOffset, n);
    pTxCharacteristic->notify();
    bleLastTxMs = millis();

    if (bleTxLastStatus == BLECharacteristicCallbacks::ERROR_GATT) {
        // Bluedroid L2CAP congested: chunk gum hota tha aur app ko adhoora reply milta tha -> retry
        if (++bleTxRetries <= BLE_TX_MAX_RETRIES) { bleTxGapMs = BLE_TX_RETRY_GAP_MS; return; }
        Serial.println("[BLE] notify failed repeatedly, reply dropped");
        bleTxPop();
        return;
    }
    if (bleTxLastStatus == BLECharacteristicCallbacks::ERROR_NO_CLIENT ||
        bleTxLastStatus == BLECharacteristicCallbacks::ERROR_NOTIFY_DISABLED) {
        bleTxPop();                     // app ne notifications enable nahi kiye: retry bekaar
        return;
    }
    bleTxRetries = 0;
    bleTxGapMs = BLE_TX_GAP_MS;
    bleTxOffset += n;
    if (bleTxOffset >= m.length()) bleTxPop();
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* srv) override {
        (void)srv;
        deviceConnected = true;
        bleMtu = 23;
    }
    void onDisconnect(BLEServer* srv) override {
        (void)srv;
        deviceConnected = false;
        bleDisconnectMs = millis();
        bleDisconnectEvent = true; // advertising restart + buffers cleanup loop() me
    }
public:
    // 2.0.17 aur 3.3.12 (Bluedroid) dono me isi signature ke saath virtual hai (verified).
    // override keyword nahi: purane 2.0.x core me ye method nahi tha, wahan bhi compile ho.
    void onMtuChanged(BLEServer* srv, esp_ble_gatts_cb_param_t* param) {
        (void)srv;
        bleMtu = param->mtu.mtu;
    }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        // getData()/getLength(): std::string vs Arduino String (core 2.x vs 3.x) dono par chalta hai
        size_t len = pCharacteristic->getLength();
        uint8_t* data = pCharacteristic->getData();
        if (len == 0 || data == nullptr || bleRxStream == nullptr) return;
        // Poora write ya kuch nahi. Pehle partial write do adhoore commands ko jod kar ek galat
        // magar "valid" command bana sakta tha. Drop ke baad 0x00 marker assembler ko reset karta hai.
        const bool marker = bleRxDropPending;
        if (xStreamBufferSpacesAvailable(bleRxStream) < len + (marker ? 1 : 0)) {
            bleRxDropPending = true;
            bleRxDropped = bleRxDropped + 1;
            return;
        }
        if (marker) {
            const uint8_t zero = 0;
            xStreamBufferSend(bleRxStream, &zero, 1, 0);
            bleRxDropPending = false;
        }
        xStreamBufferSend(bleRxStream, data, len, 0); // single writer: space check ke baad poora jayega
    }
};

class MyTxCallbacks : public BLECharacteristicCallbacks {
    void onStatus(BLECharacteristic* pCharacteristic, Status s, uint32_t code) override {
        (void)pCharacteristic; (void)code;
        bleTxLastStatus = (int)s;
    }
};

void setupBLE() {
    bleRxStream = xStreamBufferCreate(1024, 1);
    if (bleRxStream == nullptr) Serial.println("[BLE] RX stream alloc failed: BLE commands disabled");

    BLEDevice::init("AJEEVI_MERM_BOX");
    BLEDevice::setMTU(185);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());
    pTxCharacteristic->setCallbacks(new MyTxCallbacks());

    BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyRxCallbacks());

    pService->start();
    pServer->getAdvertising()->start();
}

static void pollBle() {
    if (bleDisconnectEvent) {
        bleDisconnectEvent = false;
        resetAssembler(bleCmd);
        if (bleRxStream) xStreamBufferReset(bleRxStream);
        bleAdvPending = true;
    }
    if (bleAdvPending && !deviceConnected && (millis() - bleDisconnectMs) >= 300) {
        bleAdvPending = false;
        pServer->getAdvertising()->start(); // disconnect ke baad dobara visible
    }

    // Backpressure: reply ke liye TX slot ho tabhi agla command uthao (pehle 5th reply chupchap drop)
    uint8_t c;
    while (bleRxStream && bleTxCount < BLE_TX_SLOTS && xStreamBufferReceive(bleRxStream, &c, 1, 0) == 1) {
        if (feedCmdChar(bleCmd, (char)c)) {
            cmdParser.processCommand(String(bleCmd.buf), SRC_BLE_APP);
            resetAssembler(bleCmd);
        }
    }
    pumpBleTx();
}

static void pollUart() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (feedCmdChar(uartCmd, c)) {
            cmdParser.processCommand(String(uartCmd.buf), SRC_UART);
            resetAssembler(uartCmd);
        }
    }
}

// Return codes check: pehle reconfigure/add fail hone par bhi chupchap bina watchdog chalta tha.
static void setupWatchdog() {
    esp_err_t err;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = (1 << 0);   // CPU0 idle task watch (core 2.x default jaisa)
    cfg.trigger_panic = true;
    err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg); // core ne TWDT init nahi kiya
#else
    err = esp_task_wdt_init(WDT_TIMEOUT_S, true);  // IDF 4.4: already initialised -> reconfigure
#endif
    if (err != ESP_OK) Serial.printf("[WDT] configure failed (%d)\n", (int)err);
    err = esp_task_wdt_add(NULL); // loop task hang ho jaye to auto-reset
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) Serial.printf("[WDT] subscribe failed (%d)\n", (int)err);
}

void setup() {
    Serial.begin(115200);

    // Modules Initialization
    scheduleMod.begin();
    refillMod.begin();
    gsmMod.begin();

    // BLE Mobile App Initialization
    setupBLE();
    cmdParser.setBleCallback(sendBleNotification);

    setupWatchdog();
    Serial.println("AJEEVI MERM BOX INITIALIZED");
}

void loop() {
    esp_task_wdt_reset();

    // 1. Update Schedule, Magnetic Sensor, LED1 & Buzzer
    scheduleMod.update();

    // 2. Update Refill Reminder & LED2 (time invalid ho to LED2 off)
    refillMod.update(scheduleMod.getDateTime(), scheduleMod.isTimeValid());

    // 3. Update GSM Module & SMS command listener
    gsmMod.update();

    // 4. Commands: UART (PROG_CONN) aur BLE app - dono yahin (single task) process hote hain
    pollUart();
    pollBle();

    delay(1); // IDLE task ko CPU do, power bachao
}
