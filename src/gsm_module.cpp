#include "gsm_module.h"
#include "cmd_parser.h"
#include <string.h>
#include <stdlib.h>

GSMModule gsmMod;

namespace {
// MC65/MC60 power-up timing (schematic: VGSM_EN -> Q2/Q1, PWR_KEY -> Q3).
// Quectel MC60 HW design: PWRKEY low >1 s = power on; default autobauding -> no RDY, AT ready ~4-5 s.
const uint32_t OFF_FIRST_MS     = 1500;  // boot par modem ko pehle poori tarah off rakho
const uint32_t VBAT_SETTLE_MS   = 500;
const uint32_t PWRKEY_MS        = 1200;
const uint32_t BOOT_MIN_WAIT_MS = 2000;
const uint32_t BOOT_TIMEOUT_MS  = 25000;
const uint32_t PROBE_EVERY_MS   = 700;
const uint32_t HEALTH_EVERY_MS  = 30000;
const uint32_t UNREG_POLL_MS    = 5000;
const uint8_t  SETTINGS_EVERY   = 6;      // har 6th health poll par AT+CNMI? (autobaud reboot me RDY nahi aata)
const uint32_t INIT_RETRY_MS    = 1000;
const uint32_t QUIET_MS         = 1500;   // timeout ke baad naya command nahi (late OK/ERROR drain)
const uint32_t SMS_PROMPT_MS    = 5000;
const uint32_t SMS_SEND_MS      = 125000; // CMGS result network par depend: 60 s kam tha (duplicate SMS)
const uint32_t SMS_RETRY_MS     = 5000;
const uint8_t  SMS_MAX_TRIES    = 3;

struct InitStep { const char* cmd; uint32_t timeoutMs; uint8_t maxTries; bool needSim; bool optional; };
const InitStep INIT_STEPS[] = {
    {"ATE0",              1000,  3, false, false},
    {"AT+CPIN?",          3000, 20, true,  false},   // exact "+CPIN: READY" hone tak retry
    {"AT+CSCS=\"IRA\"",   2000,  3, false, true },   // ASCII charset: '$' (*$..#) aur '@' (&@..) sahi map hon
    {"AT+CMGF=1",         2000,  3, false, false},   // Text mode SMS
    {"AT+CNMI=2,2,0,0,0", 2000,  3, false, false},   // Incoming SMS direct +CMT URC
    {"AT+CMGD=1,4",       5000,  1, false, true },   // SIM me pade purane SMS delete (storage full na ho)
};
const uint8_t INIT_STEP_COUNT = sizeof(INIT_STEPS) / sizeof(INIT_STEPS[0]);

inline bool elapsed(uint32_t now, uint32_t start, uint32_t dur) { return (uint32_t)(now - start) >= dur; }

// Strict normalisation. Sirf digits (optional leading '+'). Alphanumeric sender IDs, short codes aur
// truncated numbers reject. National = 10 digit (India NSN), "0"+10 digit trunk format bhi.
bool normalizeNumber(const char* in, char* out, size_t outSize, bool& intl) {
    const char* p = in;
    while (*p == ' ') p++;
    bool plus = false;
    if (*p == '+') { plus = true; p++; }
    char d[24];
    size_t n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        if (n >= sizeof(d) - 1) return false;
        d[n++] = *p;
    }
    d[n] = 0;
    const char* s = d;
    if (!plus && n >= 2 && s[0] == '0' && s[1] == '0') { s += 2; n -= 2; plus = true; } // 00 = international
    else if (!plus && n == 11 && s[0] == '0') { s += 1; n -= 1; }                   // national trunk 0
    else if (!plus && n > 10) plus = true;                                           // CC without '+'
    if (n < 10 || n > 15) return false;
    if (!plus && n != 10) return false;
    if (n + 1 > outSize) return false;
    memcpy(out, s, n + 1);
    intl = plus;
    return true;
}

// Dono international hon to poora number match; warna last 10 digits (national significant number).
// Pehle "last min(10,len) digits" tha -> "3210" jaisa short sender bhi authorised ho jata tha.
bool numbersMatch(const char* a, const char* b) {
    char na[24], nb[24];
    bool ia = false, ib = false;
    if (!normalizeNumber(a, na, sizeof(na), ia) || !normalizeNumber(b, nb, sizeof(nb), ib)) return false;
    if (ia && ib) return strcmp(na, nb) == 0;
    size_t la = strlen(na), lb = strlen(nb);
    return strcmp(na + la - 10, nb + lb - 10) == 0;
}
} // namespace

// ---------------------------------------------------------------- lifecycle
void GSMModule::begin() {
    pinMode(PIN_VGSM_EN, OUTPUT);
    pinMode(PIN_GSM_PWR_KEY, OUTPUT);
    pinMode(PIN_GSM_DTR, OUTPUT);

    digitalWrite(PIN_VGSM_EN, LOW);
    digitalWrite(PIN_GSM_PWR_KEY, LOW);
    digitalWrite(PIN_GSM_DTR, LOW);

    // Schematic: IO16 = GSM_TX net, IO17 = GSM_RX net (pins unchanged)
    // RX buffer bada: NVS flash write ke dauran (~100ms) bytes overflow na hon
    Serial2.setRxBufferSize(1024);
    Serial2.begin(115200, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);

#if GSM_ALLOW_ANY_SENDER
    Serial.println("[GSM] WARNING: GSM_ALLOW_ANY_SENDER=1 -> SMS commands accepted from ANY number");
#else
    if (GSM_ALLOWED_SENDER[0] == 0) Serial.println("[GSM] SMS whitelist empty -> SMS commands disabled");
#endif

    failCount = 0;
    smsHead = smsCount = 0;
    smsDropped = 0;
    enterOff(millis(), OFF_FIRST_MS);
}

// Modem ko power off karke reset state; durationMs ke baad dobara power-up.
// Outgoing SMS queue jaan-boojh kar preserve hoti hai (restart ke baad bheji jayegi).
void GSMModule::enterOff(uint32_t now, uint32_t durationMs) {
    digitalWrite(PIN_GSM_PWR_KEY, LOW);
    digitalWrite(PIN_VGSM_EN, LOW);
    cmdActive = false;
    cmdResult = RES_NONE;
    quiet = false;
    smsState = SMS_IDLE;
    rxLen = 0;
    rxOverflow = false;
    promptSeen = false;
    expectBody = false;
    currentSender[0] = 0;
    simReady = false;
    registered = false;
    healthFails = 0;
    offDurationMs = durationMs;
    stateTimer = now;
    state = GSM_OFF;
}

// Modem hang / SIM / init fail -> power-cycle with backoff (5s, 8s, 11s ... max 32s)
void GSMModule::failModem(uint32_t now) {
    if (failCount < 10) failCount++;
    enterOff(now, 2000UL + (uint32_t)failCount * 3000UL);
}

// Modem chal raha hai par settings kho gayi (reboot/brown-out): bina power-cycle ke init dobara
void GSMModule::restartInit(uint32_t now, uint32_t delayMs) {
    state = GSM_INIT;
    initStep = 0;
    initTries = 0;
    initWaitStartMs = now;
    initWaitMs = delayMs;
    cmdActive = false;
    smsState = SMS_IDLE;
    registered = false;
    expectBody = false;
}

void GSMModule::goOnline(uint32_t now) {
    state = GSM_ONLINE;
    failCount = 0;
    healthFails = 0;
    healthCount = 0;
    lastHealthMs = now;
    healthIntervalMs = 1000; // registration jaldi check karo
}

void GSMModule::update() {
    const uint32_t now = millis();

    while (Serial2.available()) feedRx((char)Serial2.read());

    if (cmdActive && cmdResult == RES_NONE && elapsed(now, cmdStartMs, cmdTimeoutMs)) {
        cmdResult = RES_TIMEOUT;
    }

    switch (state) {
        case GSM_OFF:
            if (elapsed(now, stateTimer, offDurationMs)) {
                digitalWrite(PIN_VGSM_EN, HIGH); // VGSM power ON (Q2 -> Q1)
                stateTimer = now;
                state = GSM_POWER_EN;
            }
            break;

        case GSM_POWER_EN:
            if (elapsed(now, stateTimer, VBAT_SETTLE_MS)) {
                digitalWrite(PIN_GSM_PWR_KEY, HIGH); // MC65 PWRKEY low (Q3)
                stateTimer = now;
                state = GSM_PWRKEY_PULSE;
            }
            break;

        case GSM_PWRKEY_PULSE:
            if (elapsed(now, stateTimer, PWRKEY_MS)) {
                digitalWrite(PIN_GSM_PWR_KEY, LOW);
                stateTimer = now;
                bootStartMs = now;
                lastProbeMs = now;
                rxLen = 0;
                state = GSM_BOOT_PROBE;
            }
            break;

        case GSM_BOOT_PROBE: runBootProbe(now); break;
        case GSM_INIT:       runInit(now);       break;
        case GSM_ONLINE:     runOnline(now);     break;
    }
}

// ---------------------------------------------------------------- state handlers
// "AT" bhejte raho jab tak modem OK na de (autobaud sync ke liye pehla AT aksar gum hota hai)
void GSMModule::runBootProbe(uint32_t now) {
    if (cmdActive) {
        if (cmdResult != RES_NONE) {
            bool ok = (cmdResult == RES_OK);
            cmdActive = false;
            if (ok) {
                restartInit(now, 0);
                return;
            }
        }
    } else if (elapsed(now, bootStartMs, BOOT_MIN_WAIT_MS) && elapsed(now, lastProbeMs, PROBE_EVERY_MS)) {
        lastProbeMs = now;
        sendCmd("AT", 500);
    }

    if (elapsed(now, bootStartMs, BOOT_TIMEOUT_MS)) failModem(now); // modem respond nahi kar raha
}

void GSMModule::runInit(uint32_t now) {
    if (initStep >= INIT_STEP_COUNT) { goOnline(now); return; }
    const InitStep& st = INIT_STEPS[initStep];

    if (!cmdActive) {
        if (elapsed(now, initWaitStartMs, initWaitMs) && quietOver(now)) {
            if (st.needSim) simReady = false;
            sendCmd(st.cmd, st.timeoutMs, CMD_PLAIN, st.needSim ? NEED_CPIN : NEED_NONE);
        }
        return;
    }
    if (cmdResult == RES_NONE) return;

    const CmdResult r = cmdResult;
    cmdActive = false;
    if (r == RES_TIMEOUT) startQuiet(now);
    bool ok = (r == RES_OK) && (!st.needSim || simReady);
    if (ok || (st.optional && ++initTries >= st.maxTries)) {
        initStep++;
        initTries = 0;
        initWaitStartMs = now;
        initWaitMs = 0;
        return;
    }
    if (!st.optional && ++initTries >= st.maxTries) { failModem(now); return; }
    initWaitStartMs = now;
    initWaitMs = INIT_RETRY_MS;
}

void GSMModule::runOnline(uint32_t now) {
    if (cmdActive) {
        if (cmdResult == RES_NONE) {
            // SMS: '>' prompt aane par text bhejo
            if (cmdKind == CMD_SMS && smsState == SMS_WAIT_PROMPT) {
                if (promptSeen) {
                    const SmsOut& m = smsQ[smsHead];
                    Serial2.print(m.text);
                    Serial2.write((uint8_t)0x1A); // Ctrl-Z = send
                    smsState = SMS_WAIT_RESULT;
                } else if (elapsed(now, smsStartMs, SMS_PROMPT_MS)) {
                    Serial2.write((uint8_t)0x1B); // ESC = abort
                    cmdActive = false;
                    startQuiet(now);
                    finishSms(now, false);
                }
            }
            return;
        }
        const CmdResult r = cmdResult;
        cmdActive = false;
        if (r == RES_TIMEOUT) startQuiet(now);
        if (cmdKind == CMD_SMS) {
            finishSms(now, r == RES_OK);          // RES_OK sirf "+CMGS: <mr>" ke baad (network ne accept kiya)
        } else if (r != RES_OK) {
            if (++healthFails >= 3) failModem(now); // modem hang -> power-cycle
        } else {
            healthFails = 0;
            if (cmdKind == CMD_SETTINGS && !cnmiOk) {
                Serial.println("[GSM] modem settings lost (silent reboot?) -> re-init");
                restartInit(now, 500);
            }
        }
        return;
    }
    if (!quietOver(now)) return;

    if (smsCount > 0 && registered && elapsed(now, smsRetryStartMs, smsRetryDelayMs)) {
        smsRetryDelayMs = 0;
        startSms(now);
        return;
    }

    if (elapsed(now, lastHealthMs, healthIntervalMs)) {
        lastHealthMs = now;
        healthIntervalMs = registered ? HEALTH_EVERY_MS : UNREG_POLL_MS;
        if (++healthCount >= SETTINGS_EVERY) {
            healthCount = 0;
            cnmiOk = false;
            sendCmd("AT+CNMI?", 3000, CMD_SETTINGS, NEED_CNMI);
        } else {
            sendCmd("AT+CREG?", 3000, CMD_HEALTH, NEED_CREG); // health check + network registration
        }
    }
}

// ---------------------------------------------------------------- AT helpers
void GSMModule::sendCmd(const char* cmd, uint32_t timeoutMs, CmdKind kind, CmdNeed need) {
    Serial2.print(cmd);
    Serial2.print('\r');
    cmdActive = true;
    cmdResult = RES_NONE;
    cmdKind = kind;
    cmdNeed = need;
    cmdInfoSeen = false;
    cmdStartMs = millis();
    cmdTimeoutMs = timeoutMs;
}

// OK sirf tab result hai jab command ka information line (+CREG:/+CPIN:/+CNMI:/+CMGS:) aa chuka ho.
// Isse pichhle timed-out command ka late "OK" naye command (jaise CMGS prompt) ko complete nahi karta.
void GSMModule::setResult(CmdResult r) {
    if (!cmdActive || cmdResult != RES_NONE) return;
    if (r == RES_OK && cmdNeed != NEED_NONE && !cmdInfoSeen) return;
    cmdResult = r;
}

void GSMModule::startQuiet(uint32_t now) { quiet = true; quietStartMs = now; }

bool GSMModule::quietOver(uint32_t now) {
    if (quiet && elapsed(now, quietStartMs, QUIET_MS)) quiet = false;
    return !quiet;
}

// ---------------------------------------------------------------- RX
void GSMModule::feedRx(char c) {
    // SMS text prompt "> " bina line-ending ke aata hai
    if (c == '>' && rxLen == 0 && smsState == SMS_WAIT_PROMPT) { promptSeen = true; return; }

    if (c == '\r' || c == '\n') {
        if (rxLen > 0) {
            if (!rxOverflow) {
                rxBuf[rxLen] = 0;
                handleLine(rxBuf);
            } else {
                expectBody = false; // overflow hui line drop; SMS context bhi
                currentSender[0] = 0;
            }
        }
        rxLen = 0;
        rxOverflow = false;
        return;
    }
    if (c == ' ' && rxLen == 0) return;
    if (rxLen < RX_MAX - 1) rxBuf[rxLen++] = c;
    else rxOverflow = true; // pehle String unbounded badhta tha -> heap exhaust
}

void GSMModule::parseSender(const char* line) {
    currentSender[0] = 0;
    const char* q1 = strchr(line, '"');
    if (!q1) return;
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return;
    size_t n = (size_t)(q2 - q1 - 1);
    if (n >= sizeof(currentSender)) return;           // overlong sender: unknown -> never authorised
    memcpy(currentSender, q1 + 1, n);
    currentSender[n] = 0;
}

bool GSMModule::senderAllowed(const char* number) {
#if GSM_ALLOW_ANY_SENDER
    (void)number;
    return true;                                       // explicit lab/test opt-in only
#else
    // GSM_ALLOWED_SENDER: comma-separated list. Empty list = SMS commands disabled (fail-closed).
    const char* p = GSM_ALLOWED_SENDER;
    while (*p) {
        char item[24];
        size_t n = 0;
        // config convenience: "+91 98765-43210" -> "+919876543210" (sender side stays strict)
        for (; *p && *p != ','; p++) {
            if (*p == ' ' || *p == '-') continue;
            if (n < sizeof(item) - 1) item[n++] = *p;
        }
        item[n] = 0;
        if (*p == ',') p++;
        if (n > 0 && numbersMatch(number, item)) return true;
    }
    return false;
#endif
}

void GSMModule::handleLine(const char* line) {
    // +CMT header ke baad agli line SMS body hai. Sirf yahi line command ban sakti hai, aur sirf
    // authorised sender ki. (Pehle multi-line SMS ki doosri line "raw push" path se bina whitelist
    // check ke execute ho jati thi.)
    if (expectBody) {
        expectBody = false;
        if (bodyAllowed && strstr(line, "*$") != nullptr) {
            cmdParser.processCommand(String(line), SRC_GSM_SERVER); // reply currentSender ko queue hoga
        }
        currentSender[0] = 0;
        return;
    }

    if (strncmp(line, "+CMT:", 5) == 0) {
        parseSender(line);
        bodyAllowed = currentSender[0] != 0 && senderAllowed(currentSender);
        if (!bodyAllowed) Serial.println("[GSM] SMS from non-whitelisted sender ignored");
        expectBody = true;
        return;
    }
    if (strcmp(line, "OK") == 0) { setResult(RES_OK); return; }
    if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0 ||
        strncmp(line, "+CMS ERROR", 10) == 0) { setResult(RES_ERROR); return; }

    if (strncmp(line, "+CPIN:", 6) == 0) {
        simReady = (strcmp(line, "+CPIN: READY") == 0);   // "+CPIN: NOT READY" bhi "READY" contain karta hai
        if (cmdActive && cmdNeed == NEED_CPIN) cmdInfoSeen = true;
        return;
    }

    if (strncmp(line, "+CREG:", 6) == 0) {
        // Query: "+CREG: <n>,<stat>[,...]"  URC: "+CREG: <stat>[,...]"
        char* end = nullptr;
        long a = strtol(line + 6, &end, 10);
        long stat = a;
        if (end && *end == ',' && cmdActive && cmdNeed == NEED_CREG) stat = strtol(end + 1, nullptr, 10);
        registered = (stat == 1 || stat == 5);
        if (cmdActive && cmdNeed == NEED_CREG) cmdInfoSeen = true;
        return;
    }

    if (strncmp(line, "+CNMI:", 6) == 0) {
        // "+CNMI: <mode>,<mt>,..." : direct routing ke liye mode=2, mt=2 chahiye
        char* end = nullptr;
        long mode = strtol(line + 6, &end, 10);
        long mt = (end && *end == ',') ? strtol(end + 1, nullptr, 10) : -1;
        cnmiOk = (mode == 2 && mt == 2);
        if (cmdActive && cmdNeed == NEED_CNMI) cmdInfoSeen = true;
        return;
    }

    if (strncmp(line, "+CMGS:", 6) == 0) {             // network ne SMS accept kiya (message reference)
        if (cmdActive && cmdNeed == NEED_CMGS) cmdInfoSeen = true;
        return;
    }

    // SMS SIM me store hua (+CMTI) = CNMI routing kho gayi (modem silently reboot hua)
    if (strncmp(line, "+CMTI:", 6) == 0 && state == GSM_ONLINE) {
        Serial.println("[GSM] +CMTI: SMS routing lost -> re-init");
        restartInit(millis(), 500);
        return;
    }

    if (strcmp(line, "UNDER_VOLTAGE POWER DOWN") == 0 || strcmp(line, "NORMAL POWER DOWN") == 0) {
        Serial.println("[GSM] modem powered down -> power-cycle");
        failModem(millis());
        return;
    }
    if (strcmp(line, "UNDER_VOLTAGE WARNING") == 0) { Serial.println("[GSM] modem under-voltage warning"); return; }

    // Modem khud reboot hua (fixed baud mode me RDY aata hai) -> init dobara
    if (strcmp(line, "RDY") == 0 && (state == GSM_ONLINE || state == GSM_INIT)) {
        restartInit(millis(), 500);
        return;
    }
    // Koi aur line command nahi ban sakti (server stream / TCP path is firmware me hai hi nahi).
}

// ---------------------------------------------------------------- SMS TX
void GSMModule::sendResponseToServer(const String& resp) {
    if (currentSender[0] == 0) return; // sender unknown -> reply ka rasta nahi
    if (smsCount >= SMS_QUEUE) {       // queue full -> drop, lekin ab detectable
        smsDropped++;
        Serial.println("[GSM] SMS queue full, reply dropped");
        return;
    }

    SmsOut& m = smsQ[(smsHead + smsCount) % SMS_QUEUE];
    strncpy(m.number, currentSender, sizeof(m.number) - 1);
    m.number[sizeof(m.number) - 1] = 0;
    size_t n = resp.length();
    if (n > SMS_TEXT_MAX) n = SMS_TEXT_MAX;
    memcpy(m.text, resp.c_str(), n);
    m.text[n] = 0;
    m.tries = 0;
    if (smsCount == 0) { smsRetryStartMs = millis(); smsRetryDelayMs = 0; }
    smsCount++;
}

void GSMModule::startSms(uint32_t now) {
    char c[48];
    snprintf(c, sizeof(c), "AT+CMGS=\"%s\"", smsQ[smsHead].number);
    promptSeen = false;
    smsState = SMS_WAIT_PROMPT;
    sendCmd(c, SMS_SEND_MS, CMD_SMS, NEED_CMGS);
    smsStartMs = now;
}

void GSMModule::finishSms(uint32_t now, bool ok) {
    smsState = SMS_IDLE;
    promptSeen = false;
    SmsOut& m = smsQ[smsHead];
    if (!ok) {
        m.tries++;
        if (m.tries < SMS_MAX_TRIES) {
            smsRetryStartMs = now;            // same message dobara try
            smsRetryDelayMs = SMS_RETRY_MS;
            return;
        }
        Serial.println("[GSM] SMS reply failed after retries, dropped");
    }
    smsHead = (uint8_t)((smsHead + 1) % SMS_QUEUE); // success ya retries khatam -> pop
    smsCount--;
    smsRetryStartMs = now;                    // agla queued SMS turant
    smsRetryDelayMs = 0;
}
