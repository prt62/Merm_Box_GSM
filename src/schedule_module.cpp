#include "schedule_module.h"
#include <Preferences.h>
#include <Wire.h>

#define RX8900_I2C_ADDR 0x32

ScheduleModule scheduleMod;
static Preferences prefs;

uint8_t ScheduleModule::decToBcd(uint8_t val) { return ((val / 10 * 16) + (val % 10)); }
uint8_t ScheduleModule::bcdToDec(uint8_t val) { return ((val / 16 * 10) + (val % 16)); }

void ScheduleModule::begin() {
    pinMode(PIN_MAG_SENSOR, INPUT_PULLUP);
    pinMode(PIN_LED1_SCHED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);

    digitalWrite(PIN_LED1_SCHED, LOW);
    digitalWrite(PIN_BUZZER, LOW);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    loadFromStorage();
    syncRTCRead();
}

void ScheduleModule::loadFromStorage() {
    prefs.begin("merm_sched", false);
    beepCount = prefs.getUChar("beepCnt", 2);
    totalSchedLogs = prefs.getUShort("totSLogs", 0);
    totalMissedLogs = prefs.getUShort("totMLogs", 0);

    if (prefs.isKey("schedData")) {
        prefs.getBytes("schedData", schedules, sizeof(schedules));
    } else {
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            schedules[i] = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
        }
    }

    if (prefs.isKey("sLogs")) {
        prefs.getBytes("sLogs", schedLogs, sizeof(schedLogs));
    }
    if (prefs.isKey("mLogs")) {
        prefs.getBytes("mLogs", missedLogs, sizeof(missedLogs));
    }
    prefs.end();
}

void ScheduleModule::saveToStorage() {
    prefs.begin("merm_sched", false);
    prefs.putUChar("beepCnt", beepCount);
    prefs.putUShort("totSLogs", totalSchedLogs);
    prefs.putUShort("totMLogs", totalMissedLogs);
    prefs.putBytes("schedData", schedules, sizeof(schedules));
    prefs.putBytes("sLogs", schedLogs, sizeof(schedLogs));
    prefs.putBytes("mLogs", missedLogs, sizeof(missedLogs));
    prefs.end();
}

bool ScheduleModule::isBoxOpen() {
    return (digitalRead(PIN_MAG_SENSOR) == BOX_OPEN_STATE);
}

void ScheduleModule::syncRTCRead() {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() == 0 && Wire.requestFrom(RX8900_I2C_ADDR, 7) == 7) {
        currentDT.second = bcdToDec(Wire.read() & 0x7F);
        currentDT.minute = bcdToDec(Wire.read() & 0x7F);
        currentDT.hour   = bcdToDec(Wire.read() & 0x3F);
        Wire.read(); // Skip weekday
        currentDT.day    = bcdToDec(Wire.read() & 0x3F);
        currentDT.month  = bcdToDec(Wire.read() & 0x1F);
        currentDT.year   = 2000 + bcdToDec(Wire.read());
    }
}

void ScheduleModule::syncRTCWrite(const DateTimeData& dt) {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write(0x00);
    Wire.write(decToBcd(dt.second));
    Wire.write(decToBcd(dt.minute));
    Wire.write(decToBcd(dt.hour));
    Wire.write(0x01); // Weekday placeholder
    Wire.write(decToBcd(dt.day));
    Wire.write(decToBcd(dt.month));
    Wire.write(decToBcd(dt.year % 100));
    Wire.endTransmission();
}

void ScheduleModule::update() {
    unsigned long nowMs = millis();

    // Har 1 second me RTC update aur schedule check
    if (nowMs - lastRtcTick >= 1000) {
        lastRtcTick = nowMs;
        syncRTCRead();

        // Fallback software clock increment agar RTC IC mount na ho
        currentDT.second++;
        if (currentDT.second >= 60) {
            currentDT.second = 0;
            currentDT.minute++;
            if (currentDT.minute >= 60) {
                currentDT.minute = 0;
                currentDT.hour = (currentDT.hour + 1) % 24;
            }
        }

        uint16_t curMinutes = currentDT.hour * 60 + currentDT.minute;
        bool anyAlertActive = false;
        bool boxOpenNow = isBoxOpen();

        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (!schedules[i].enabled) continue;

            // Naye din par daily flags reset karein
            if (schedules[i].lastTriggeredDay != currentDT.day) {
                schedules[i].lastTriggeredDay = currentDT.day;
                schedules[i].takenToday = false;
                schedules[i].missedLoggedToday = false;
                schedules[i].alertActive = false;
                schedules[i].beepsDone = 0;
            }

            uint16_t startMins = schedules[i].startHour * 60 + schedules[i].startMin;
            uint16_t endMins   = schedules[i].endHour * 60 + schedules[i].endMin;

            // Check karein kya abhi schedule window active hai
            if (curMinutes >= startMins && curMinutes < endMins) {
                if (!schedules[i].takenToday) {
                    schedules[i].alertActive = true;

                    // Agar user ne box khol liya
                    if (boxOpenNow) {
                        schedules[i].takenToday = true;
                        schedules[i].alertActive = false;
                        recordScheduleLog(currentDT);
                    }
                }
            } else if (curMinutes >= endMins) {
                // Schedule window khatam ho gaya aur box nahi khula -> Missed Log
                if (!schedules[i].takenToday && !schedules[i].missedLoggedToday) {
                    schedules[i].missedLoggedToday = true;
                    schedules[i].alertActive = false;
                    recordMissedLog(currentDT, schedules[i]);
                }
                schedules[i].alertActive = false;
            }

            if (schedules[i].alertActive) {
                anyAlertActive = true;
            }
        }

        // LED1 tab tak jalegi jab tak box nahi khulega (jab tak alertActive hai)
        digitalWrite(PIN_LED1_SCHED, anyAlertActive ? HIGH : LOW);
        if (!anyAlertActive) {
            digitalWrite(PIN_BUZZER, LOW);
            buzzerState = false;
        }
    }

    // Non-blocking Buzzer Beep Control
    if (nowMs - lastBeepToggle >= 400) {
        lastBeepToggle = nowMs;
        bool needBeep = false;

        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (schedules[i].enabled && schedules[i].alertActive) {
                if (schedules[i].beepsDone < beepCount * 2) {
                    needBeep = true;
                    schedules[i].beepsDone++;
                    break;
                }
            }
        }

        if (needBeep) {
            buzzerState = !buzzerState;
            digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
        } else {
            digitalWrite(PIN_BUZZER, LOW);
        }
    }
}

bool ScheduleModule::setSchedule(uint8_t idx, uint8_t sHr, uint8_t sMin, uint8_t eHr, uint8_t eMin) {
    if (idx < 1 || idx > MAX_SCHEDULES) return false;
    if (sHr > 23 || sMin > 59 || eHr > 23 || eMin > 59) return false;

    uint8_t i = idx - 1;
    schedules[i].enabled = true;
    schedules[i].schedNo = idx;
    schedules[i].startHour = sHr;
    schedules[i].startMin = sMin;
    schedules[i].endHour = eHr;
    schedules[i].endMin = eMin;
    schedules[i].takenToday = false;
    schedules[i].missedLoggedToday = false;
    schedules[i].alertActive = false;
    schedules[i].beepsDone = 0;
    schedules[i].lastTriggeredDay = currentDT.day;

    saveToStorage();
    return true;
}

bool ScheduleModule::deleteSchedule(uint8_t idx) {
    if (idx < 1 || idx > MAX_SCHEDULES) return false;
    uint8_t i = idx - 1;
    schedules[i].enabled = false;
    schedules[i].alertActive = false;
    saveToStorage();
    return true;
}

bool ScheduleModule::setBeepCount(uint8_t count) {
    if (count == 0 || count > 50) return false;
    beepCount = count;
    saveToStorage();
    return true;
}

uint8_t ScheduleModule::getBeepCount() {
    return beepCount;
}

bool ScheduleModule::setDateTime(uint8_t dd, uint8_t mm, uint16_t yyyy, uint8_t hh, uint8_t min, uint8_t ss) {
    if (dd < 1 || dd > 31 || mm < 1 || mm > 12 || hh > 23 || min > 59 || ss > 59) return false;
    if (yyyy < 100) yyyy += 2000;
    currentDT = {dd, mm, yyyy, hh, min, ss};
    syncRTCWrite(currentDT);
    return true;
}

DateTimeData ScheduleModule::getDateTime() {
    return currentDT;
}

String ScheduleModule::getDateTimeString() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d-%02d-%04d-%02d-%02d-%02d",
             currentDT.day, currentDT.month, currentDT.year,
             currentDT.hour, currentDT.minute, currentDT.second);
    return String(buf);
}

String ScheduleModule::getAllSchedulesString() {
    String out = "";
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        char buf[24];
        if (schedules[i].enabled) {
            snprintf(buf, sizeof(buf), "%d:%02d:%02d:%02d:%02d",
                     i + 1, schedules[i].startHour, schedules[i].startMin,
                     schedules[i].endHour, schedules[i].endMin);
        } else {
            snprintf(buf, sizeof(buf), "%d:00:00:00:00", i + 1);
        }
        out += String(buf);
        if (i < MAX_SCHEDULES - 1) out += ",";
    }
    return out;
}

void ScheduleModule::recordScheduleLog(const DateTimeData& dt) {
    uint16_t slot = totalSchedLogs % MAX_LOGS;
    schedLogs[slot] = {dt.day, dt.month, (uint8_t)(dt.year % 100), dt.hour, dt.minute, dt.second};
    totalSchedLogs++;
    saveToStorage();
}

void ScheduleModule::recordMissedLog(const DateTimeData& dt, const ScheduleItem& item) {
    uint16_t slot = totalMissedLogs % MAX_LOGS;
    missedLogs[slot] = {dt.day, dt.month, (uint8_t)(dt.year % 100),
                        item.startHour, item.startMin, item.endHour, item.endMin};
    totalMissedLogs++;
    saveToStorage();
}

bool ScheduleModule::clearAllLogs() {
    totalSchedLogs = 0;
    totalMissedLogs = 0;
    memset(schedLogs, 0, sizeof(schedLogs));
    memset(missedLogs, 0, sizeof(missedLogs));
    saveToStorage();
    return true;
}

bool ScheduleModule::factoryReset() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
    }
    beepCount = 2;
    clearAllLogs();
    return true;
}

uint16_t ScheduleModule::getTotalScheduleLogs() { return totalSchedLogs; }
uint16_t ScheduleModule::getTotalMissedLogs() { return totalMissedLogs; }

// Ex. String : &@240-200-22-24-01-24-11-41-20-!
String ScheduleModule::getScheduleLogPacket(uint16_t logNumber) {
    if (totalSchedLogs == 0 || logNumber < 1 || logNumber > totalSchedLogs) {
        return "&@240-0-0-00-00-00-00-00-00-!";
    }
    uint16_t idx = (logNumber - 1) % MAX_LOGS;
    ScheduleLogEntry& e = schedLogs[idx];
    char buf[64];
    snprintf(buf, sizeof(buf), "&@240-%u-%u-%02d-%02d-%02d-%02d-%02d-%02d-!",
             totalSchedLogs, logNumber, e.dd, e.mm, e.yy, e.hh, e.min, e.ss);
    return String(buf);
}

// Ex. String : &@250-200-22-24-01-24-09-30-10-30-!
String ScheduleModule::getMissedLogPacket(uint16_t logNumber) {
    if (totalMissedLogs == 0 || logNumber < 1 || logNumber > totalMissedLogs) {
        return "&@250-0-0-00-00-00-00-00-00-00-!";
    }
    uint16_t idx = (logNumber - 1) % MAX_LOGS;
    MissedLogEntry& e = missedLogs[idx];
    char buf[64];
    snprintf(buf, sizeof(buf), "&@250-%u-%u-%02d-%02d-%02d-%02d-%02d-%02d-%02d-!",
             totalMissedLogs, logNumber, e.dd, e.mm, e.yy,
             e.startHH, e.startMM, e.endHH, e.endMM);
    return String(buf);
}