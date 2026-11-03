#include "cmd_parser.h"
#include "schedule_module.h"
#include "refill_module.h"
#include "gsm_module.h"
#include <WiFi.h>

CommandParser cmdParser;

void CommandParser::setBleCallback(BleSendCallback cb) {
    bleSend = cb;
}

int CommandParser::splitCSV(const String& str, String* tokens, int maxTokens) {
    int count = 0;
    int start = 0;
    while (count < maxTokens) {
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

void CommandParser::sendReply(const String& reply, CommandSource src) {
    // Debug ke liye UART par hamesha print karein
    Serial.println(reply);

    if (src == SRC_BLE_APP && bleSend != nullptr) {
        bleSend(reply);
    } else if (src == SRC_GSM_SERVER) {
        gsmMod.sendResponseToServer(reply);
    }
}

void CommandParser::processCommand(const String& rawCmd, CommandSource src) {
    int startPos = rawCmd.indexOf("*$");
    int endPos = rawCmd.indexOf('#', startPos);

    if (startPos == -1 || endPos == -1) return;

    // Extract payload between "*$" and "#"
    String payload = rawCmd.substring(startPos + 2, endPos);
    String tokens[10];
    int tokenCount = splitCSV(payload, tokens, 10);

    if (tokenCount < 1) return;
    int cmdCode = tokens[0].toInt();
    String response = "";

    switch (cmdCode) {
        // 101: TO SET SCHEDULE -> EX: *$101,1,9,0,10,0,#
        case 101: {
            bool ok = false;
            if (tokenCount >= 6) {
                ok = scheduleMod.setSchedule(
                    tokens[1].toInt(), tokens[2].toInt(),
                    tokens[3].toInt(), tokens[4].toInt(), tokens[5].toInt()
                );
            }
            response = String("&@101-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 102: TO DELETE EXISTING SCHEDULE -> EX: *$102,1,#
        case 102: {
            bool ok = false;
            if (tokenCount >= 2) {
                ok = scheduleMod.deleteSchedule(tokens[1].toInt());
            }
            response = String("&@102-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 103: TO SET BEEP COUNT -> EX: *$103,2,#
        case 103: {
            bool ok = false;
            if (tokenCount >= 2) {
                ok = scheduleMod.setBeepCount(tokens[1].toInt());
            }
            response = String("&@103-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 104: TO SET REFILL DATE -> EX: *$104,03,09,11,04,09,11,#
        case 104: {
            bool ok = false;
            if (tokenCount >= 7) {
                ok = refillMod.setRefillDates(
                    tokens[1].toInt(), tokens[2].toInt(), tokens[3].toInt(),
                    tokens[4].toInt(), tokens[5].toInt(), tokens[6].toInt()
                );
            }
            response = String("&@104-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 105: TO SET DEVICE DATE AND TIME -> EX: *$105,10,08,24,10,20,30,#
        case 105: {
            bool ok = false;
            if (tokenCount >= 7) {
                ok = scheduleMod.setDateTime(
                    tokens[1].toInt(), tokens[2].toInt(), tokens[3].toInt(),
                    tokens[4].toInt(), tokens[5].toInt(), tokens[6].toInt()
                );
            }
            response = String("&@105-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 106: TO SET REFILL DONE -> EX: *$106,#
        case 106: {
            bool ok = refillMod.setRefillDone();
            response = String("&@106-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 110: TO CLEAR ALL LOGS -> EX: *$110,1,#
        case 110: {
            bool ok = scheduleMod.clearAllLogs();
            response = String("&@110-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 111: TO FACTORY RESET -> EX: *$111,1,#
        case 111: {
            bool ok = scheduleMod.factoryReset();
            refillMod.resetRefill();
            response = String("&@111-") + (ok ? "1" : "2") + "-!";
            break;
        }

        // 201: TO GET DEVICE DETAILS -> EX: *$201,#
        case 201: {
            response = "&@201-" + scheduleMod.getAllSchedulesString() + "-" +
                       String(scheduleMod.getBeepCount()) + "-" +
                       refillMod.getRefillDateString() + "-" +
                       scheduleMod.getDateTimeString() + "-!";
            break;
        }

        // 202: TO GET DEVICE DATE AND TIME -> EX: *$202,#
        case 202: {
            response = "&@202-" + scheduleMod.getDateTimeString() + "-!";
            break;
        }

        // 205: TO GET TOTAL NUMBER OF SCHEDULE LOGS -> EX: *$205,#
        case 205: {
            response = "&@205-" + String(scheduleMod.getTotalScheduleLogs()) + "-!";
            break;
        }

        // 206: TO GET TOTAL NUMBER OF MISSED LOGS -> EX: *$206,#
        case 206: {
            response = "&@206-" + String(scheduleMod.getTotalMissedLogs()) + "-!";
            break;
        }

        // 207: TO GET MAC ADDRESS -> EX: *$207,#
        case 207: {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            response = "&@207-" + mac + "-!";
            break;
        }

        // 240: RESPONSE FROM APP TO DEVICE FOR SCHEDULE LOG -> EX: *$240,1,#
        case 240: {
            uint16_t logNo = (tokenCount >= 2) ? tokens[1].toInt() : 1;
            response = scheduleMod.getScheduleLogPacket(logNo);
            break;
        }

        // 250: RESPONSE FROM APP TO DEVICE FOR MISSED LOG -> EX: *$250,1,#
        case 250: {
            uint16_t logNo = (tokenCount >= 2) ? tokens[1].toInt() : 1;
            response = scheduleMod.getMissedLogPacket(logNo);
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