#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "config.h"
#include "schedule_module.h"
#include "refill_module.h"
#include "gsm_module.h"
#include "cmd_parser.h"

// Nordic UART BLE UUIDs for Mobile App Communication
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic* pTxCharacteristic = nullptr;
bool deviceConnected = false;
String uartRxBuffer = "";

void sendBleNotification(const String& data) {
    if (deviceConnected && pTxCharacteristic != nullptr) {
        pTxCharacteristic->setValue(data.c_str());
        pTxCharacteristic->notify();
    }
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
    }
    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        pServer->getAdvertising()->start();
    }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string rxValue = pCharacteristic->getValue();
        if (!rxValue.empty()) {
            String cmd = String(rxValue.c_str());
            cmdParser.processCommand(cmd, SRC_BLE_APP);
        }
    }
};

void setupBLE() {
    BLEDevice::init("AJEEVI_MERM_BOX");
    BLEServer* pServer = BLEDevice::createServer();
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

void setup() {
    Serial.begin(115200);

    // Modules Initialization
    scheduleMod.begin();
    refillMod.begin();
    gsmMod.begin();

    // BLE Mobile App Initialization
    setupBLE();
    cmdParser.setBleCallback(sendBleNotification);

    Serial.println("AJEEVI MERM BOX INITIALIZED");
}

void loop() {
    // 1. Update Schedule, Magnetic Sensor, LED1 & Buzzer
    scheduleMod.update();

    // 2. Update Refill Reminder & LED2
    refillMod.update(scheduleMod.getDateTime());

    // 3. Update GSM Module & Server Listener
    gsmMod.update();

    // 4. Listen for Configuration Commands on UART Serial (PROG_CONN)
    while (Serial.available()) {
        char c = (char)Serial.read();
        uartRxBuffer += c;
        if (c == '#') {
            cmdParser.processCommand(uartRxBuffer, SRC_UART);
            uartRxBuffer = "";
        } else if (c == '\n' || c == '\r') {
            uartRxBuffer = "";
        }
    }
}