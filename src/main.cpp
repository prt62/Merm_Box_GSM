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
// BLE callbacks alag FreeRTOS task me chalte hain. Pehle wo seedha cmdParser/scheduleMod ko
// touch karte the jab loop() bhi wahi data use kar raha tha (data race).
// Ab: BLE callback sirf bytes StreamBuffer me daalta hai; saara processing loop() me hota hai.
// ---------------------------------------------------------------------------
static BLEServer*          pServer = nullptr;
static BLECharacteristic*  pTxCharacteristic = nullptr;
static StreamBufferHandle_t bleRxStream = nullptr;

static volatile bool     deviceConnected = false;
static volatile bool     bleDisconnectEvent = false;
static volatile uint32_t bleDisconnectMs = 0;
static volatile uint16_t bleMtu = 23;
static bool              bleAdvPending = false;

// ----- command assembler (UART aur BLE dono ke liye; bounded buffer) -----
#define CMD_BUF_MAX 96
struct CmdAssembler {
    char buf[CMD_BUF_MAX];
    uint8_t len;
};
static CmdAssembler uartCmd = {{0}, 0};
static CmdAssembler bleCmd  = {{0}, 0};

// true tab jab '#' par command complete ho. Overflow par buffer drop (unbounded String growth nahi).
static bool feedCmdChar(CmdAssembler& a, char c) {
    if (c == '\n' || c == '\r') { a.len = 0; return false; }
    if (a.len >= CMD_BUF_MAX - 1) a.len = 0;
    a.buf[a.len++] = c;
    a.buf[a.len] = 0;
    return c == '#';
}

// ----- BLE TX queue: replies MTU-3 ke chunks me bhejte hain (pehle 20 byte se lambi reply truncate hoti thi) -----
#define BLE_TX_SLOTS 4
static String   bleTxMsg[BLE_TX_SLOTS];
static uint8_t  bleTxHead = 0, bleTxCount = 0;
static size_t   bleTxOffset = 0;
static uint32_t bleLastTxMs = 0;

void sendBleNotification(const String& data) {
    if (!deviceConnected || data.length() == 0) return;
    if (bleTxCount >= BLE_TX_SLOTS) return; // slow client -> drop (blocking nahi)
    bleTxMsg[(bleTxHead + bleTxCount) % BLE_TX_SLOTS] = data;
    bleTxCount++;
}

static void pumpBleTx() {
    if (!deviceConnected) { // disconnect: pending replies discard
        for (int i = 0; i < BLE_TX_SLOTS; i++) bleTxMsg[i] = "";
        bleTxHead = bleTxCount = 0;
        bleTxOffset = 0;
        return;
    }
    if (bleTxCount == 0 || pTxCharacteristic == nullptr) return;
    if ((millis() - bleLastTxMs) < 12) return; // notify congestion se bachne ke liye pacing

    uint16_t mtu = bleMtu;
    size_t chunk = (mtu > 23) ? (size_t)(mtu - 3) : 20;
    if (chunk > 180) chunk = 180;

    String& m = bleTxMsg[bleTxHead];
    size_t n = m.length() - bleTxOffset;
    if (n > chunk) n = chunk;

    pTxCharacteristic->setValue((uint8_t*)m.c_str() + bleTxOffset, n);
    pTxCharacteristic->notify();
    bleTxOffset += n;
    bleLastTxMs = millis();

    if (bleTxOffset >= m.length()) {
        m = "";
        bleTxHead = (bleTxHead + 1) % BLE_TX_SLOTS;
        bleTxCount--;
        bleTxOffset = 0;
    }
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* srv) override {
        deviceConnected = true;
        bleMtu = 23;
    }
    void onDisconnect(BLEServer* srv) override {
        deviceConnected = false;
        bleDisconnectMs = millis();
        bleDisconnectEvent = true; // advertising restart + buffers cleanup loop() me
    }
public:
    // Newer cores me MTU negotiate hone par call hota hai (override keyword nahi: purane core me bhi compile ho)
    void onMtuChanged(BLEServer* srv, esp_ble_gatts_cb_param_t* param) {
        bleMtu = param->mtu.mtu;
    }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        // getData()/getLength(): std::string vs Arduino String (core 2.x vs 3.x) dono par chalta hai
        size_t len = pCharacteristic->getLength();
        uint8_t* data = pCharacteristic->getData();
        if (len > 0 && data != nullptr && bleRxStream != nullptr) {
            xStreamBufferSend(bleRxStream, data, len, 0); // BLE task -> loop task, lock-free
        }
    }
};

void setupBLE() {
    bleRxStream = xStreamBufferCreate(1024, 1);

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
        bleCmd.len = 0;
        if (bleRxStream) xStreamBufferReset(bleRxStream);
        bleAdvPending = true;
    }
    if (bleAdvPending && !deviceConnected && (millis() - bleDisconnectMs) >= 300) {
        bleAdvPending = false;
        pServer->getAdvertising()->start(); // disconnect ke baad dobara visible
    }

    uint8_t tmp[32];
    size_t n;
    while (bleRxStream && (n = xStreamBufferReceive(bleRxStream, tmp, sizeof(tmp), 0)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (feedCmdChar(bleCmd, (char)tmp[i])) {
                cmdParser.processCommand(String(bleCmd.buf), SRC_BLE_APP);
                bleCmd.len = 0;
            }
        }
    }
    pumpBleTx();
}

static void pollUart() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (feedCmdChar(uartCmd, c)) {
            cmdParser.processCommand(String(uartCmd.buf), SRC_UART);
            uartCmd.len = 0;
        }
    }
}

static void setupWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg;
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic = true;
    esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL); // loop task hang ho jaye to auto-reset
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

    // 2. Update Refill Reminder & LED2
    refillMod.update(scheduleMod.getDateTime());

    // 3. Update GSM Module & SMS command listener
    gsmMod.update();

    // 4. Commands: UART (PROG_CONN) aur BLE app - dono yahin (single task) process hote hain
    pollUart();
    pollBle();

    delay(1); // IDLE task ko CPU do, power bachao
}
