#include "gsm_module.h"
#include "cmd_parser.h"
#include <string.h>
#include <stdlib.h>

GSMModule gsmMod;

namespace {
// MC65/MC60 power-up timing (schematic: VGSM_EN -> Q2/Q1, PWR_KEY -> Q3)
const uint32_t OFF_FIRST_MS     = 1500;  // boot par modem ko pehle poori tarah off rakho
const uint32_t VBAT_SETTLE_MS   = 500;
const uint32_t PWRKEY_MS        = 1200;
const uint32_t BOOT_MIN_WAIT_MS = 2000;
const uint32_t BOOT_TIMEOUT_MS  = 25000;
const uint32_t PROBE_EVERY_MS   = 700;
const uint32_t HEALTH_EVERY_MS  = 30000;
const uint32_t UNREG_POLL_MS    = 5000;
const uint32_t INIT_RETRY_MS    = 1000;
const uint32_t SMS_PROMPT_MS    = 5000;
const uint32_t SMS_SEND_MS      = 60000;
const uint32_t SMS_RETRY_MS     = 5000;
const uint8_t  SMS_MAX_TRIES    = 3;

struct InitStep { const char* cmd; uint32_t timeoutMs; uint8_t maxTries; bool needSim; };
const InitStep INIT_STEPS[] = {
    {"ATE0",              1000,  3, false},
    {"AT+CPIN?",          3000, 20, true },   // SIM READY hone tak retry
    {"AT+CMGF=1",         2000,  3, false},   // Text mode SMS
    {"AT+CNMI=2,2,0,0,0", 2000,  3, false},   // Incoming SMS direct +CMT URC
};
const uint8_t INIT_STEP_COUNT = sizeof(INIT_STEPS) / sizeof(INIT_STEPS[0]);

// last min(10, len) digits compare (+91 prefix ka farq ignore)
bool numbersMatch(const char* a, const char* b) {
    char da[24], db[24];
    int la = 0, lb = 0;
    for (const char* p = a; *p && la < 23; p++) if (*p >= '0' && *p <= '9') da[la++] = *p;
    for (const char* p = b; *p && lb < 23; p++) if (*p >= '0' && *p <= '9') db[lb++] = *p;
    if (la == 0 || lb == 0) return false;
    int n = la < lb ? la : lb;
    if (n > 10) n = 10;
    return memcmp(da + la - n, db + lb - n, n) == 0;
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

    failCount = 0;
    smsHead = smsCount = 0;
    enterOff(millis(), OFF_FIRST_MS);
}

// Modem ko power off karke reset state; durationMs ke baad dobara power-up
void GSMModule::enterOff(uint32_t now, uint32_t durationMs) {
    digitalWrite(PIN_GSM_PWR_KEY, LOW);
    digitalWrite(PIN_VGSM_EN, LOW);
    cmdActive = false;
    cmdResult = RES_NONE;
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

// Modem hang / SIM / init fail -> power-cycle with backoff (2s, 5s, 8s ... max 32s)
void GSMModule::failModem(uint32_t now) {
    if (failCount < 10) failCount++;
    enterOff(now, 2000UL + (uint32_t)failCount * 3000UL);
}

void GSMModule::goOnline(uint32_t now) {
    state = GSM_ONLINE;
    failCount = 0;
    healthFails = 0;
    nextHealthMs = now + 1000; // registration jaldi check karo
}

void GSMModule::update() {
    const uint32_t now = millis();

    while (Serial2.available()) feedRx((char)Serial2.read());

    if (cmdActive && cmdResult == RES_NONE && (now - cmdStartMs) >= cmdTimeoutMs) {
        cmdResult = RES_TIMEOUT;
    }

    switch (state) {
        case GSM_OFF:
            if ((now - stateTimer) >= offDurationMs) {
                digitalWrite(PIN_VGSM_EN, HIGH); // VGSM power ON (Q2 -> Q1)
                stateTimer = now;
                state = GSM_POWER_EN;
            }
            break;

        case GSM_POWER_EN:
            if ((now - stateTimer) >= VBAT_SETTLE_MS) {
                digitalWrite(PIN_GSM_PWR_KEY, HIGH); // MC65 PWRKEY low (Q3)
                stateTimer = now;
                state = GSM_PWRKEY_PULSE;
            }
            break;

        case GSM_PWRKEY_PULSE:
            if ((now - stateTimer) >= PWRKEY_MS) {
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
// "AT" bhejte raho jab tak modem OK na de (blind 3s wait + unchecked commands ki jagah)
void GSMModule::runBootProbe(uint32_t now) {
    if (cmdActive) {
        if (cmdResult != RES_NONE) {
            bool ok = (cmdResult == RES_OK);
            cmdActive = false;
            if (ok) {
                state = GSM_INIT;
                initStep = 0;
                initTries = 0;
                retryAtMs = now;
                return;
            }
        }
    } else if ((now - bootStartMs) >= BOOT_MIN_WAIT_MS && (now - lastProbeMs) >= PROBE_EVERY_MS) {
        lastProbeMs = now;
        sendCmd("AT", 500);
    }

    if ((now - bootStartMs) >= BOOT_TIMEOUT_MS) failModem(now); // modem respond nahi kar raha
}

void GSMModule::runInit(uint32_t now) {
    if (initStep >= INIT_STEP_COUNT) { goOnline(now); return; }
    const InitStep& st = INIT_STEPS[initStep];

    if (!cmdActive) {
        if ((int32_t)(now - retryAtMs) >= 0) {
            if (st.needSim) simReady = false;
            sendCmd(st.cmd, st.timeoutMs);
        }
        return;
    }
    if (cmdResult == RES_NONE) return;

    bool ok = (cmdResult == RES_OK) && (!st.needSim || simReady);
    cmdActive = false;
    if (ok) {
        initStep++;
        initTries = 0;
        retryAtMs = now;
        return;
    }
    if (++initTries >= st.maxTries) { failModem(now); return; }
    retryAtMs = now + INIT_RETRY_MS;
}

void GSMModule::runOnline(uint32_t now) {
    if (cmdActive) {
        if (cmdResult == RES_NONE) {
            // SMS: '>' prompt aane par text bhejo
            if (cmdKind == CMD_SMS && smsState == SMS_WAIT_PROMPT) {
                if (promptSeen) {
                    SmsOut& m = smsQ[smsHead];
                    Serial2.print(m.text);
                    Serial2.write((uint8_t)0x1A); // Ctrl-Z = send
                    smsState = SMS_WAIT_RESULT;
                } else if ((now - smsStartMs) >= SMS_PROMPT_MS) {
                    Serial2.write((uint8_t)0x1B); // ESC = abort
                    cmdActive = false;
                    finishSms(now, false);
                }
            }
            return;
        }
        CmdResult r = cmdResult;
        cmdActive = false;
        if (cmdKind == CMD_SMS) {
            finishSms(now, r == RES_OK);
        } else if (cmdKind == CMD_HEALTH) {
            if (r == RES_OK) healthFails = 0;
            else if (++healthFails >= 3) failModem(now); // modem hang -> power-cycle
        }
        return;
    }

    if (smsCount > 0 && registered && (int32_t)(now - smsRetryAtMs) >= 0) {
        startSms(now);
        return;
    }

    if ((int32_t)(now - nextHealthMs) >= 0) {
        nextHealthMs = now + (registered ? HEALTH_EVERY_MS : UNREG_POLL_MS);
        sendCmd("AT+CREG?", 3000, CMD_HEALTH); // health check + network registration
    }
}

// ---------------------------------------------------------------- AT helpers
void GSMModule::sendCmd(const char* cmd, uint32_t timeoutMs, CmdKind kind) {
    Serial2.print(cmd);
    Serial2.print('\r');
    cmdActive = true;
    cmdResult = RES_NONE;
    cmdKind = kind;
    cmdStartMs = millis();
    cmdTimeoutMs = timeoutMs;
}

void GSMModule::setResult(CmdResult r) {
    if (cmdActive && cmdResult == RES_NONE) cmdResult = r;
}

// ---------------------------------------------------------------- RX
void GSMModule::feedRx(char c) {
    // SMS text prompt "> " bina line-ending ke aata hai
    if (c == '>' && rxLen == 0) { promptSeen = true; return; }

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
    if (n >= sizeof(currentSender)) n = sizeof(currentSender) - 1;
    memcpy(currentSender, q1 + 1, n);
    currentSender[n] = 0;
}

bool GSMModule::senderAllowed(const char* number) {
    const char* allowed = GSM_ALLOWED_SENDER;
    if (allowed[0] == 0) return true; // whitelist off
    return numbersMatch(number, allowed);
}

void GSMModule::handleLine(const char* line) {
    // +CMT header ke baad agli line SMS body hai
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
        bodyAllowed = senderAllowed(currentSender);
        expectBody = true;
        return;
    }
    if (strcmp(line, "OK") == 0) { setResult(RES_OK); return; }
    if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0 ||
        strncmp(line, "+CMS ERROR", 10) == 0) { setResult(RES_ERROR); return; }

    if (strncmp(line, "+CPIN:", 6) == 0) { simReady = (strstr(line, "READY") != nullptr); return; }

    if (strncmp(line, "+CREG:", 6) == 0) {
        // Query: "+CREG: <n>,<stat>[,...]"  URC: "+CREG: <stat>"
        const char* p = line + 6;
        long a = strtol(p, (char**)&p, 10);
        long stat = a;
        if (*p == ',') stat = strtol(p + 1, nullptr, 10);
        registered = (stat == 1 || stat == 5);
        return;
    }

    // Modem khud reboot hua (brownout etc.) -> init dobara
    if (strcmp(line, "RDY") == 0 && (state == GSM_ONLINE || state == GSM_INIT)) {
        state = GSM_INIT;
        initStep = 0;
        initTries = 0;
        cmdActive = false;
        smsState = SMS_IDLE;
        registered = false;
        retryAtMs = millis() + 500;
        return;
    }

    // Non-SMS raw push (server stream): "*$...#"
    if (strstr(line, "*$") != nullptr) {
        currentSender[0] = 0;
        cmdParser.processCommand(String(line), SRC_GSM_SERVER);
    }
}

// ---------------------------------------------------------------- SMS TX
void GSMModule::sendResponseToServer(const String& resp) {
    // Pehle yahan Serial2.println(resp) tha -> modem ise AT command samajh kar ERROR deta tha,
    // reply kabhi nahi pahunchta tha. Ab proper AT+CMGS flow se SMS jata hai.
    if (currentSender[0] == 0) return; // sender unknown (raw line) -> reply ka rasta nahi
    if (smsCount >= SMS_QUEUE) return; // queue full -> drop (heap use nahi)

    SmsOut& m = smsQ[(smsHead + smsCount) % SMS_QUEUE];
    strncpy(m.number, currentSender, sizeof(m.number) - 1);
    m.number[sizeof(m.number) - 1] = 0;
    size_t n = resp.length();
    if (n > SMS_TEXT_MAX) n = SMS_TEXT_MAX;
    memcpy(m.text, resp.c_str(), n);
    m.text[n] = 0;
    m.tries = 0;
    if (smsCount == 0) smsRetryAtMs = millis(); // millis-wrap safe: kabhi stale 0 se compare nahi
    smsCount++;
}

void GSMModule::startSms(uint32_t now) {
    char c[48];
    snprintf(c, sizeof(c), "AT+CMGS=\"%s\"", smsQ[smsHead].number);
    promptSeen = false;
    sendCmd(c, SMS_SEND_MS, CMD_SMS);
    smsState = SMS_WAIT_PROMPT;
    smsStartMs = now;
}

void GSMModule::finishSms(uint32_t now, bool ok) {
    smsState = SMS_IDLE;
    SmsOut& m = smsQ[smsHead];
    if (!ok) {
        m.tries++;
        if (m.tries < SMS_MAX_TRIES) {
            smsRetryAtMs = now + SMS_RETRY_MS; // same message dobara try
            return;
        }
    }
    smsHead = (smsHead + 1) % SMS_QUEUE; // success ya retries khatam -> pop
    smsCount--;
    smsRetryAtMs = now;                  // agla queued SMS turant
}
