#include "cmd_parser.h"
#include "schedule_module.h"
#include "refill_module.h"
#include "gsm_module.h"

CommandParser cmdParser;

static const int MAX_TOKENS = 10;

void CommandParser::setBleCallback(BleSendCallback cb) {
    bleSend = cb;
}

// overflow=true jab MAX_TOKENS se zyada fields hon (pehle extra fields chupchap ignore hote the)
int CommandParser::splitCSV(const String& str, String* tokens, int maxTokens, bool& overflow) {
    int count = 0;
    int start = 0;
    overflow = false;
    while (true) {
        if (count >= maxTokens) { overflow = true; break; }
        int comma = str.indexOf(',', start);
        if (comma == -1) {
            tokens[count++] = str.substring(start);
            break;
        }
        tokens[count++] = str.substring(start, comma);
        start = comma + 1;
    }
    return count;
}

// Strict number parse: sirf digits (spaces trim). "abc" ya "12x" ab 0 nahi banta, reject hota hai.
static bool parseInt(const String& s, long& out) {
    int i = 0, j = (int)s.length();
    while (i < j && s.charAt(i) == ' ') i++;
    while (j > i && s.charAt(j - 1) == ' ') j--;
    if (j <= i || (j - i) > 9) return false;
    long v = 0;
    for (int k = i; k < j; k++) {
        char c = s.charAt(k);
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

static bool isBlank(const String& s) {
    for (unsigned i = 0; i < s.length(); i++) if (s.charAt(i) != ' ') return false;
    return true;
}

// tokens[first .. first+n-1] ko integers me parse karta hai
static bool parseArgs(const String* tokens, int tokenCount, int first, int n, long* out) {
    for (int k = 0; k < n; k++) {
        if (first + k >= tokenCount) return false;
        if (!parseInt(tokens[first + k], out[k])) return false;
    }
    return true;
}

// Har command ke exact parameters (documented format). -1 = unknown command.
static int expectedArgs(long cmd) {
    switch (cmd) {
        case 101: return 5;
        case 102: case 103: return 1;
        case 104: case 105: return 6;
        case 106: return 0;
        case 110: case 111: return 1;
        case 201: case 202: case 205: case 206: case 207: return 0;
        case 240: case 250: return 1;
        default: return -1;
    }
}

void CommandParser::sendReply(const String& reply, CommandSource src) {
    // Debug ke liye UART par hamesha print karein
    Serial.println(reply);

    if (src == SRC_BLE_APP && bleSend != nullptr) {
        bleSend(reply);
    } else if (src == SRC_GSM_SERVER) {
        gsmMod.sendResponseToServer(reply);
    }
}

static String ack(int code, bool ok) {
    return String("&@") + String(code) + "-" + (ok ? "1" : "2") + "-!";
}

void CommandParser::processCommand(const String& rawCmd, CommandSource src) {
    int startPos = rawCmd.indexOf("*$");
    if (startPos == -1) return;
    int endPos = rawCmd.indexOf('#', startPos);
    if (endPos == -1) return;

    // Extract payload between "*$" and "#"
    String payload = rawCmd.substring(startPos + 2, endPos);
    String tokens[MAX_TOKENS];
    bool overflow = false;
    int tokenCount = splitCSV(payload, tokens, MAX_TOKENS, overflow);
    if (tokenCount < 1) return;
    // Documented format me '#' se pehle comma hota hai ("*$101,1,9,0,10,0,#") -> ek khaali last token.
    // Wahi ek ignore hota hai; beech ka khaali field ya extra field reject.
    if (tokenCount > 1 && isBlank(tokens[tokenCount - 1])) tokenCount--;

    long cmd;
    if (!parseInt(tokens[0], cmd)) {
        sendReply("&@ERR-2-!", src);
        return;
    }
    const int want = expectedArgs(cmd);
    if (want < 0) {
        sendReply("&@ERR-2-!", src);
        return;
    }
    long v[6] = {0, 0, 0, 0, 0, 0};
    const int nargs = overflow ? MAX_TOKENS : tokenCount - 1;
    const bool argsOk = (nargs == want) && parseArgs(tokens, tokenCount, 1, want, v);

    // SMS se destructive commands (logs clear / factory reset) default me band.
    // SMS sender ID spoofable hai aur ye operations irreversible hain; BLE/UART (paas hona zaroori) se allowed.
    if (src == SRC_GSM_SERVER && (cmd == 110 || cmd == 111) && !GSM_SMS_ALLOW_DESTRUCTIVE) {
        Serial.println("[CMD] destructive command refused over SMS");
        sendReply(ack((int)cmd, false), src);
        return;
    }

    String response = "";

    switch (cmd) {
        // 101: TO SET SCHEDULE -> EX: *$101,1,9,0,10,0,#
        case 101: {
            bool ok = argsOk && scheduleMod.setSchedule((int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4]);
            response = ack(101, ok);
            break;
        }

        // 102: TO DELETE EXISTING SCHEDULE -> EX: *$102,1,#
        case 102: {
            bool ok = argsOk && scheduleMod.deleteSchedule((int)v[0]);
            response = ack(102, ok);
            break;
        }

        // 103: TO SET BEEP COUNT -> EX: *$103,2,#
        case 103: {
            bool ok = argsOk && scheduleMod.setBeepCount((int)v[0]);
            response = ack(103, ok);
            break;
        }

        // 104: TO SET REFILL DATE -> EX: *$104,03,09,11,04,09,11,#
        case 104: {
            bool ok = argsOk && refillMod.setRefillDates((int)v[0], (int)v[1], (int)v[2],
                                                         (int)v[3], (int)v[4], (int)v[5]);
            response = ack(104, ok);
            break;
        }

        // 105: TO SET DEVICE DATE AND TIME -> EX: *$105,10,08,24,10,20,30,#
        case 105: {
            bool ok = argsOk && scheduleMod.setDateTime((int)v[0], (int)v[1], (int)v[2],
                                                        (int)v[3], (int)v[4], (int)v[5]);
            response = ack(105, ok);
            break;
        }

        // 106: TO SET REFILL DONE -> EX: *$106,#
        case 106: {
            response = ack(106, argsOk && refillMod.setRefillDone());
            break;
        }

        // 110: TO CLEAR ALL LOGS -> EX: *$110,1,#   (confirm token "1" zaroori)
        case 110: {
            bool ok = argsOk && v[0] == 1 && scheduleMod.clearAllLogs();
            response = ack(110, ok);
            break;
        }

        // 111: TO FACTORY RESET -> EX: *$111,1,#   (confirm token "1" zaroori)
        case 111: {
            bool ok = false;
            if (argsOk && v[0] == 1) {
                bool a = scheduleMod.factoryReset();
                bool b = refillMod.resetRefill();
                ok = a && b;
            }
            response = ack(111, ok);
            break;
        }

        // 201: TO GET DEVICE DETAILS -> EX: *$201,#
        case 201: {
            if (!argsOk) { response = "&@ERR-2-!"; break; }
            response = "&@201-" + scheduleMod.getAllSchedulesString() + "-" +
                       String(scheduleMod.getBeepCount()) + "-" +
                       refillMod.getRefillDateString() + "-" +
                       scheduleMod.getDateTimeString() + "-!";
            break;
        }

        // 202: TO GET DEVICE DATE AND TIME -> EX: *$202,#
        case 202: {
            if (!argsOk) { response = "&@ERR-2-!"; break; }
            response = "&@202-" + scheduleMod.getDateTimeString() + "-!";
            break;
        }

        // 205: TO GET TOTAL NUMBER OF SCHEDULE LOGS -> EX: *$205,#
        case 205: {
            if (!argsOk) { response = "&@ERR-2-!"; break; }
            response = "&@205-" + String(scheduleMod.getTotalScheduleLogs()) + "-!";
            break;
        }

        // 206: TO GET TOTAL NUMBER OF MISSED LOGS -> EX: *$206,#
        case 206: {
            if (!argsOk) { response = "&@ERR-2-!"; break; }
            response = "&@206-" + String(scheduleMod.getTotalMissedLogs()) + "-!";
            break;
        }

        // 207: TO GET MAC ADDRESS -> EX: *$207,#   (WiFi init ki zaroorat nahi: efuse base MAC)
        case 207: {
            if (!argsOk) { response = "&@ERR-2-!"; break; }
            uint64_t m = ESP.getEfuseMac();
            char mac[13];
            snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
                     (unsigned)((m >> 0) & 0xFF), (unsigned)((m >> 8) & 0xFF),
                     (unsigned)((m >> 16) & 0xFF), (unsigned)((m >> 24) & 0xFF),
                     (unsigned)((m >> 32) & 0xFF), (unsigned)((m >> 40) & 0xFF));
            response = String("&@207-") + mac + "-!";
            break;
        }

        // 240: RESPONSE FROM APP TO DEVICE FOR SCHEDULE LOG -> EX: *$240,1,#
        // Missing/invalid/extra index -> existing "no log" packet (format unchanged)
        case 240: {
            long n = argsOk ? v[0] : 0;
            if (n < 1 || n > 65535) n = 0;
            response = scheduleMod.getScheduleLogPacket((uint16_t)n);
            break;
        }

        // 250: RESPONSE FROM APP TO DEVICE FOR MISSED LOG -> EX: *$250,1,#
        case 250: {
            long n = argsOk ? v[0] : 0;
            if (n < 1 || n > 65535) n = 0;
            response = scheduleMod.getMissedLogPacket((uint16_t)n);
            break;
        }

        default:
            response = "&@ERR-2-!";
            break;
    }

    if (response.length() > 0) {
        sendReply(response, src);
    }
}
