#ifndef GSM_MODULE_H
#define GSM_MODULE_H

#include "config.h"

// MC65/MC60 SMS-based command channel (text mode). Poora non-blocking state machine.
//   power-on -> AT probe -> init (ATE0, CPIN, CSCS, CMGF, CNMI, CMGD) -> online
//   incoming SMS (+CMT) -> whitelist -> cmdParser -> reply SMS wapas sender ko
// Saare timers "start + duration" (unsigned elapsed) se: millis() wrap / 24.8-din overflow safe.
class GSMModule {
public:
    void begin();
    void update();

    // Current command ke sender ko SMS reply queue karta hai (non-blocking)
    void sendResponseToServer(const String& resp);

    bool isOnline() const { return state == GSM_ONLINE; }
    bool isRegistered() const { return registered; }
    uint32_t droppedReplies() const { return smsDropped; }

private:
    enum GsmState { GSM_OFF, GSM_POWER_EN, GSM_PWRKEY_PULSE, GSM_BOOT_PROBE, GSM_INIT, GSM_ONLINE };
    enum CmdResult { RES_NONE, RES_OK, RES_ERROR, RES_TIMEOUT };
    enum CmdKind { CMD_PLAIN, CMD_HEALTH, CMD_SETTINGS, CMD_SMS };
    enum CmdNeed { NEED_NONE, NEED_CPIN, NEED_CREG, NEED_CNMI, NEED_CMGS }; // info line required before OK
    enum SmsState { SMS_IDLE, SMS_WAIT_PROMPT, SMS_WAIT_RESULT };

    static const uint16_t RX_MAX = 200;
    static const uint8_t  SMS_QUEUE = 4;
    static const uint16_t SMS_TEXT_MAX = 160;

    struct SmsOut {
        char number[24];
        char text[SMS_TEXT_MAX + 1];
        uint8_t tries;
    };

    GsmState state = GSM_OFF;
    uint32_t stateTimer = 0;
    uint32_t offDurationMs = 0;
    uint32_t bootStartMs = 0, lastProbeMs = 0;
    uint8_t failCount = 0;

    // Line receiver
    char rxBuf[RX_MAX];
    uint16_t rxLen = 0;
    bool rxOverflow = false;
    bool promptSeen = false;

    // AT command in flight (ek time par ek)
    bool cmdActive = false;
    CmdResult cmdResult = RES_NONE;
    CmdKind cmdKind = CMD_PLAIN;
    CmdNeed cmdNeed = NEED_NONE;
    bool cmdInfoSeen = false;
    uint32_t cmdStartMs = 0, cmdTimeoutMs = 0;
    uint32_t quietStartMs = 0;            // timeout ke baad thodi der chup: late reply galat command ko na mile
    bool quiet = false;

    // Init sequence
    uint8_t initStep = 0, initTries = 0;
    uint32_t initWaitStartMs = 0, initWaitMs = 0;
    bool simReady = false;
    bool registered = false;
    bool cnmiOk = false;
    uint8_t healthFails = 0;
    uint8_t healthCount = 0;
    uint32_t lastHealthMs = 0, healthIntervalMs = 0;

    // Incoming SMS context
    bool expectBody = false;
    bool bodyAllowed = false;
    char currentSender[24] = {0};

    // Outgoing SMS ring queue
    SmsOut smsQ[SMS_QUEUE];
    uint8_t smsHead = 0, smsCount = 0;
    SmsState smsState = SMS_IDLE;
    uint32_t smsStartMs = 0, smsRetryStartMs = 0, smsRetryDelayMs = 0;
    uint32_t smsDropped = 0;

    void enterOff(uint32_t now, uint32_t durationMs);
    void failModem(uint32_t now);
    void restartInit(uint32_t now, uint32_t delayMs);
    void goOnline(uint32_t now);
    void runBootProbe(uint32_t now);
    void runInit(uint32_t now);
    void runOnline(uint32_t now);

    void sendCmd(const char* cmd, uint32_t timeoutMs, CmdKind kind = CMD_PLAIN, CmdNeed need = NEED_NONE);
    void setResult(CmdResult r);
    void startQuiet(uint32_t now);
    bool quietOver(uint32_t now);
    void feedRx(char c);
    void handleLine(const char* line);
    void parseSender(const char* line);
    bool senderAllowed(const char* number);

    void startSms(uint32_t now);
    void finishSms(uint32_t now, bool ok);
};

extern GSMModule gsmMod;

#endif // GSM_MODULE_H
