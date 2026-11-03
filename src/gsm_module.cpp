#include "gsm_module.h"
#include "cmd_parser.h"

GSMModule gsmMod;

void GSMModule::begin() {
    pinMode(PIN_VGSM_EN, OUTPUT);
    pinMode(PIN_GSM_PWR_KEY, OUTPUT);
    pinMode(PIN_GSM_DTR, OUTPUT);

    digitalWrite(PIN_VGSM_EN, LOW);
    digitalWrite(PIN_GSM_PWR_KEY, LOW);
    digitalWrite(PIN_GSM_DTR, LOW);

    // Schematic: IO16 = GSM_TX net, IO17 = GSM_RX net
    Serial2.begin(115200, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);

    state = GSM_POWER_EN;
    stateTimer = millis();
}

void GSMModule::sendAT(const String& cmd) {
    Serial2.println(cmd);
}

void GSMModule::update() {
    unsigned long now = millis();

    // Non-blocking State Machine for MC65 Power-Up (Schematic Page 2)
    switch (state) {
        case GSM_POWER_EN:
            // Turn ON VGSM power via Q2 (S8050) & Q1 (SI2333)
            digitalWrite(PIN_VGSM_EN, HIGH);
            if (now - stateTimer >= 500) {
                digitalWrite(PIN_GSM_PWR_KEY, HIGH); // Pull MC65 PWRKEY low via Q3
                stateTimer = now;
                state = GSM_PWRKEY_PULSE;
            }
            break;

        case GSM_PWRKEY_PULSE:
            if (now - stateTimer >= 1200) {
                digitalWrite(PIN_GSM_PWR_KEY, LOW);
                stateTimer = now;
                state = GSM_WAIT_READY;
            }
            break;

        case GSM_WAIT_READY:
            if (now - stateTimer >= 3000) {
                sendAT("AT");
                sendAT("ATE0");
                sendAT("AT+CMGF=1");          // Text mode SMS
                sendAT("AT+CNMI=2,2,0,0,0");  // Direct URC for incoming SMS/Server msg
                stateTimer = now;
                state = GSM_ONLINE;
            }
            break;

        case GSM_ONLINE:
            break;

        default:
            break;
    }

    // Read incoming data from MC65 GSM UART
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        if (c == '\n' || c == '\r') {
            if (rxBuffer.length() > 0) {
                processIncomingLine(rxBuffer);
                rxBuffer = "";
            }
        } else {
            rxBuffer += c;
            // Agar command packet '#` par end ho gaya to turant process karein
            if (c == '#' && rxBuffer.indexOf("*$") >= 0) {
                processIncomingLine(rxBuffer);
                rxBuffer = "";
            }
        }
    }
}

void GSMModule::processIncomingLine(const String& line) {
    int startIdx = line.indexOf("*$");
    int endIdx = line.indexOf('#', startIdx);
    if (startIdx >= 0 && endIdx > startIdx) {
        String cleanCmd = line.substring(startIdx, endIdx + 1);
        cmdParser.processCommand(cleanCmd, SRC_GSM_SERVER);
    }
}

void GSMModule::sendResponseToServer(const String& resp) {
    // GSM TCP/MQTT/SMS stream par response bhejein
    Serial2.println(resp);
}