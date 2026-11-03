#include "schedule_module.h"
#include <Preferences.h>
#include <Wire.h>

#define RX8900_I2C_ADDR  0x32
#define RX8900_REG_SEC   0x00
#define RX8900_REG_FLAG  0x0E

static const uint32_t MAG_DEBOUNCE_MS = 40;
static const uint32_t BEEP_PERIOD_MS  = 400;
static const uint32_t FLUSH_RETRY_MS  = 30000;

ScheduleModule scheduleMod;
static Preferences prefs;

uint8_t ScheduleModule::decToBcd(uint8_t val) { return ((val / 10 * 16) + (val % 10)); }
uint8_t ScheduleModule::bcdToDec(uint8_t val) { return ((val / 16 * 10) + (val % 16)); }

// ---------------------------------------------------------------- date helpers
static bool isLeap(uint16_t y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static uint8_t daysInMonth(uint8_t m, uint16_t y) {
    static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 31;
    if (m == 2 && isLeap(y)) return 29;
    return d[m - 1];
}

static bool validDateTime(const DateTimeData& t) {
    return t.year >= 2000 && t.year <= 2099 && t.month >= 1 && t.month <= 12 &&
           t.day >= 1 && t.day <= daysInMonth(t.month, t.year) &&
           t.hour < 24 && t.minute < 60 && t.second < 60;
}

static uint32_t makeDayKey(uint16_t y, uint8_t m, uint8_t d) {
    return (uint32_t)y * 10000UL + (uint32_t)m * 100UL + d;
}

static uint32_t prevDayKey(const DateTimeData& dt) {
    uint16_t y = dt.year; uint8_t m = dt.month, d = dt.day;
    if (d > 1) { d--; }
    else {
        if (m > 1) m--; else { m = 12; y--; }
        d = daysInMonth(m, y);
    }
    return makeDayKey(y, m, d);
}

static uint8_t weekdayOf(uint16_t y, uint8_t m, uint8_t d) { // 0 = Sunday
    static const uint8_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static bool bcdOk(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

// ---------------------------------------------------------------- lifecycle
void ScheduleModule::begin() {
    pinMode(PIN_MAG_SENSOR, INPUT_PULLUP);
    pinMode(PIN_LED1_SCHED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);

    digitalWrite(PIN_LED1_SCHED, LOW);
    digitalWrite(PIN_BUZZER, LOW);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setTimeOut(50); // I2C hang hone par loop ko block na kare
    loadFromStorage();
    rtcOk = syncRTCRead();

    dbRaw = dbStable = (digitalRead(PIN_MAG_SENSOR) == BOX_OPEN_STATE);
    dbChangeMs = millis();
    openLatch = false;
    lastRtcTick = millis();
}

void ScheduleModule::resetRuntime() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        instKey[i] = 0;
        schedules[i].takenToday = false;
        schedules[i].missedLoggedToday = false;
        schedules[i].alertActive = false;
        schedules[i].beepsDone = 0;
    }
    cfgDirty = true;
}

// ---------------------------------------------------------------- storage
void ScheduleModule::loadFromStorage() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
        instKey[i] = 0;
    }
    beepCount = 2;
    schedCount = schedHead = missedCount = missedHead = 0;

    if (!prefs.begin("merm_sched", false)) return;

    uint8_t b = prefs.getUChar("beepCnt", 2);
    if (b >= 1 && b <= 50) beepCount = b;

    if (prefs.getBytesLength("schedData") == sizeof(schedules)) {
        prefs.getBytes("schedData", schedules, sizeof(schedules));
    }
    if (prefs.getBytesLength("inst") == sizeof(instKey)) {
        prefs.getBytes("inst", instKey, sizeof(instKey));
    }
    // Corrupt/garbage data se bachne ke liye sanitize
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        ScheduleItem& s = schedules[i];
        s.schedNo = i + 1;
        uint16_t st = s.startHour * 60 + s.startMin, en = s.endHour * 60 + s.endMin;
        if (s.startHour > 23 || s.endHour > 23 || s.startMin > 59 || s.endMin > 59 || st == en) {
            s = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
            instKey[i] = 0;
        }
        s.alertActive = false; // reboot ke baad alert dobara evaluate hoga
        s.beepsDone = 0;
    }

    if (prefs.getBytesLength("sLogs") == sizeof(schedLogs)) {
        prefs.getBytes("sLogs", schedLogs, sizeof(schedLogs));
        if (prefs.isKey("sCnt")) {
            schedCount = prefs.getUShort("sCnt", 0);
            schedHead  = prefs.getUShort("sHead", 0);
        } else { // purane firmware ka format (total counter)
            uint16_t tot = prefs.getUShort("totSLogs", 0);
            schedCount = tot > MAX_LOGS ? MAX_LOGS : tot;
            schedHead  = tot % MAX_LOGS;
        }
    }
    if (prefs.getBytesLength("mLogs") == sizeof(missedLogs)) {
        prefs.getBytes("mLogs", missedLogs, sizeof(missedLogs));
        if (prefs.isKey("mCnt")) {
            missedCount = prefs.getUShort("mCnt", 0);
            missedHead  = prefs.getUShort("mHead", 0);
        } else {
            uint16_t tot = prefs.getUShort("totMLogs", 0);
            missedCount = tot > MAX_LOGS ? MAX_LOGS : tot;
            missedHead  = tot % MAX_LOGS;
        }
    }
    if (schedCount > MAX_LOGS || schedHead >= MAX_LOGS) schedCount = schedHead = 0;
    if (missedCount > MAX_LOGS || missedHead >= MAX_LOGS) missedCount = missedHead = 0;
    prefs.end();
}

bool ScheduleModule::saveConfig() {
    if (!prefs.begin("merm_sched", false)) return false;
    bool ok = true;
    ok &= (prefs.putUChar("beepCnt", beepCount) == 1);
    ok &= (prefs.putBytes("schedData", schedules, sizeof(schedules)) == sizeof(schedules));
    ok &= (prefs.putBytes("inst", instKey, sizeof(instKey)) == sizeof(instKey));
    prefs.end();
    return ok;
}

bool ScheduleModule::saveSchedLogs() {
    if (!prefs.begin("merm_sched", false)) return false;
    bool ok = true;
    ok &= (prefs.putBytes("sLogs", schedLogs, sizeof(schedLogs)) == sizeof(schedLogs));
    ok &= (prefs.putUShort("sCnt", schedCount) == 2);
    ok &= (prefs.putUShort("sHead", schedHead) == 2);
    prefs.end();
    return ok;
}

bool ScheduleModule::saveMissedLogs() {
    if (!prefs.begin("merm_sched", false)) return false;
    bool ok = true;
    ok &= (prefs.putBytes("mLogs", missedLogs, sizeof(missedLogs)) == sizeof(missedLogs));
    ok &= (prefs.putUShort("mCnt", missedCount) == 2);
    ok &= (prefs.putUShort("mHead", missedHead) == 2);
    prefs.end();
    return ok;
}

// force=true: command path (turant likho). force=false: periodic path (fail hone par backoff).
bool ScheduleModule::flushStorage(bool force) {
    if (!cfgDirty && !sLogDirty && !mLogDirty) return true;
    if (!force && (int32_t)(millis() - nextFlushTryMs) < 0) return false;

    bool ok = true;
    if (cfgDirty)  { if (saveConfig())     cfgDirty = false;  else ok = false; }
    if (sLogDirty) { if (saveSchedLogs())  sLogDirty = false; else ok = false; }
    if (mLogDirty) { if (saveMissedLogs()) mLogDirty = false; else ok = false; }
    if (!ok) nextFlushTryMs = millis() + FLUSH_RETRY_MS;
    return ok;
}

// ---------------------------------------------------------------- RTC
bool ScheduleModule::syncRTCRead() {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_SEC);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)RX8900_I2C_ADDR, (uint8_t)7) != 7) return false;

    uint8_t raw[7];
    for (int i = 0; i < 7; i++) raw[i] = Wire.read();

    uint8_t sec = raw[0] & 0x7F, mn = raw[1] & 0x7F, hr = raw[2] & 0x3F;
    uint8_t day = raw[4] & 0x3F, mon = raw[5] & 0x1F, yr = raw[6];
    if (!bcdOk(sec) || !bcdOk(mn) || !bcdOk(hr) || !bcdOk(day) || !bcdOk(mon) || !bcdOk(yr)) return false;

    DateTimeData t = {bcdToDec(day), bcdToDec(mon), (uint16_t)(2000 + bcdToDec(yr)),
                      bcdToDec(hr), bcdToDec(mn), bcdToDec(sec)};
    if (!validDateTime(t)) return false; // floating bus / garbage read
    currentDT = t;
    return true;
}

bool ScheduleModule::syncRTCWrite(const DateTimeData& dt) {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_SEC);
    Wire.write(decToBcd(dt.second));
    Wire.write(decToBcd(dt.minute));
    Wire.write(decToBcd(dt.hour));
    Wire.write((uint8_t)(1 << weekdayOf(dt.year, dt.month, dt.day))); // RX8900 week reg one-hot (bit0 = Sunday)
    Wire.write(decToBcd(dt.day));
    Wire.write(decToBcd(dt.month));
    Wire.write(decToBcd(dt.year % 100));
    if (Wire.endTransmission() != 0) return false;

    // Time set ke baad Flag register (VLF etc.) clear karein
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_FLAG);
    Wire.write((uint8_t)0x00);
    return Wire.endTransmission() == 0;
}

// RTC na mile to software clock (poora calendar rollover ke saath)
void ScheduleModule::softwareTick() {
    if (++currentDT.second < 60) return;
    currentDT.second = 0;
    if (++currentDT.minute < 60) return;
    currentDT.minute = 0;
    if (++currentDT.hour < 24) return;
    currentDT.hour = 0;
    if (++currentDT.day <= daysInMonth(currentDT.month, currentDT.year)) return;
    currentDT.day = 1;
    if (++currentDT.month <= 12) return;
    currentDT.month = 1;
    currentDT.year++;
}

// ---------------------------------------------------------------- sensor
void ScheduleModule::pollMagSensor(uint32_t nowMs) {
    bool raw = (digitalRead(PIN_MAG_SENSOR) == BOX_OPEN_STATE);
    if (raw != dbRaw) {
        dbRaw = raw;
        dbChangeMs = nowMs;
    } else if (raw != dbStable && (nowMs - dbChangeMs) >= MAG_DEBOUNCE_MS) {
        dbStable = raw;
        if (raw) openLatch = true; // chhota open event bhi 1-sec tick tak yaad rahega
    }
}

bool ScheduleModule::isBoxOpen() {
    return dbStable;
}

// ---------------------------------------------------------------- main update
void ScheduleModule::update() {
    uint32_t nowMs = millis();
    pollMagSensor(nowMs);

    if ((nowMs - lastRtcTick) >= 1000) {
        lastRtcTick = nowMs;

        // RTC master hai. Sirf RTC read fail ho to software clock chalao
        // (pehle dono lag rahe the -> time 1 sec aage + hour rollover pe day nahi badhta tha)
        rtcOk = syncRTCRead();
        if (!rtcOk) softwareTick();

        evaluateSchedules();
        flushStorage(false);
    }

    updateBuzzer(nowMs);
}

void ScheduleModule::evaluateSchedules() {
    const uint16_t cur = currentDT.hour * 60 + currentDT.minute;
    const uint32_t today = makeDayKey(currentDT.year, currentDT.month, currentDT.day);
    const uint32_t yest = prevDayKey(currentDT);
    const bool opened = openLatch || dbStable;
    openLatch = false;
    bool anyAlertActive = false;

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        ScheduleItem& s = schedules[i];
        if (!s.enabled) continue;

        const uint16_t st = s.startHour * 60 + s.startMin;
        const uint16_t en = s.endHour * 60 + s.endMin;

        // Window check: normal (start<end) aur midnight cross karne wali (start>end) dono
        bool inWindow = false;
        uint32_t key = 0; // window kis din shuru hui
        if (st < en) {
            inWindow = (cur >= st && cur < en);
            key = today;
        } else if (st > en) {
            if (cur >= st)      { inWindow = true; key = today; }
            else if (cur < en)  { inWindow = true; key = yest;  }
        }

        if (inWindow) {
            if (instKey[i] != key) { // nayi window instance -> flags reset
                instKey[i] = key;
                s.lastTriggeredDay = key % 100;
                s.takenToday = false;
                s.missedLoggedToday = false;
                s.alertActive = false;
                s.beepsDone = 0;
                cfgDirty = true;
            }
            if (!s.takenToday) {
                if (opened) {
                    s.takenToday = true;
                    s.alertActive = false;
                    recordScheduleLog(currentDT);
                    cfgDirty = true;
                } else {
                    s.alertActive = true;
                }
            } else {
                s.alertActive = false;
            }
        } else if (instKey[i] != 0) {
            // Window abhi khatam hui. Box nahi khula -> Missed log (sirf ek baar, window ki sahi date ke saath)
            if (!s.takenToday && !s.missedLoggedToday) {
                s.missedLoggedToday = true;
                recordMissedLog(instKey[i], s);
            }
            instKey[i] = 0;
            s.alertActive = false;
            s.beepsDone = 0;
            cfgDirty = true;
        }

        if (s.alertActive) anyAlertActive = true;
    }

    // LED1 tab tak jalegi jab tak alert active hai
    digitalWrite(PIN_LED1_SCHED, anyAlertActive ? HIGH : LOW);
    if (!anyAlertActive) {
        buzzerState = false;
        digitalWrite(PIN_BUZZER, LOW);
    }
}

// Non-blocking buzzer
void ScheduleModule::updateBuzzer(uint32_t nowMs) {
    if ((nowMs - lastBeepToggle) < BEEP_PERIOD_MS) return;
    lastBeepToggle = nowMs;

    bool needBeep = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (schedules[i].enabled && schedules[i].alertActive &&
            schedules[i].beepsDone < (uint16_t)beepCount * 2) {
            needBeep = true;
            schedules[i].beepsDone++;
            break;
        }
    }
    buzzerState = needBeep ? !buzzerState : false;
    digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
}

// ---------------------------------------------------------------- commands
bool ScheduleModule::setSchedule(int idx, int sHr, int sMin, int eHr, int eMin) {
    if (idx < 1 || idx > MAX_SCHEDULES) return false;
    if (sHr < 0 || sHr > 23 || sMin < 0 || sMin > 59 || eHr < 0 || eHr > 23 || eMin < 0 || eMin > 59) return false;
    if (sHr * 60 + sMin == eHr * 60 + eMin) return false; // zero-length window

    uint8_t i = idx - 1;
    schedules[i] = {true, (uint8_t)idx, (uint8_t)sHr, (uint8_t)sMin, (uint8_t)eHr, (uint8_t)eMin,
                    0, false, false, false, 0};
    instKey[i] = 0;
    cfgDirty = true;
    return flushStorage(true);
}

bool ScheduleModule::deleteSchedule(int idx) {
    if (idx < 1 || idx > MAX_SCHEDULES) return false;
    uint8_t i = idx - 1;
    schedules[i].enabled = false;
    schedules[i].alertActive = false;
    schedules[i].takenToday = false;
    schedules[i].missedLoggedToday = false;
    schedules[i].beepsDone = 0;
    instKey[i] = 0;
    cfgDirty = true;
    return flushStorage(true);
}

bool ScheduleModule::setBeepCount(int count) {
    if (count < 1 || count > 50) return false;
    beepCount = (uint8_t)count;
    cfgDirty = true;
    return flushStorage(true);
}

uint8_t ScheduleModule::getBeepCount() {
    return beepCount;
}

bool ScheduleModule::setDateTime(int dd, int mm, int yyyy, int hh, int min, int ss) {
    if (yyyy >= 0 && yyyy < 100) yyyy += 2000;
    if (yyyy < 2000 || yyyy > 2099) return false;
    DateTimeData t = {(uint8_t)(dd < 0 ? 0 : dd > 255 ? 255 : dd), (uint8_t)(mm < 0 ? 0 : mm > 255 ? 255 : mm),
                      (uint16_t)yyyy,
                      (uint8_t)(hh < 0 ? 255 : hh > 255 ? 255 : hh),
                      (uint8_t)(min < 0 ? 255 : min > 255 ? 255 : min),
                      (uint8_t)(ss < 0 ? 255 : ss > 255 ? 255 : ss)};
    if (!validDateTime(t)) return false; // 31 Feb, 25:00 wagairah reject

    currentDT = t;
    syncRTCWrite(currentDT);   // RTC na ho to bhi software clock set rahegi
    resetRuntime();            // time jump ke baad purani window state/spurious missed log nahi
    digitalWrite(PIN_LED1_SCHED, LOW);
    digitalWrite(PIN_BUZZER, LOW);
    buzzerState = false;
    lastRtcTick = millis();
    return flushStorage(true);
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
        char buf[40];
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

// ---------------------------------------------------------------- logs
void ScheduleModule::recordScheduleLog(const DateTimeData& dt) {
    schedLogs[schedHead] = {dt.day, dt.month, (uint8_t)(dt.year % 100), dt.hour, dt.minute, dt.second};
    schedHead = (schedHead + 1) % MAX_LOGS;
    if (schedCount < MAX_LOGS) schedCount++;
    sLogDirty = true;
}

void ScheduleModule::recordMissedLog(uint32_t dayKey, const ScheduleItem& item) {
    uint8_t dd = dayKey % 100, mm = (dayKey / 100) % 100, yy = (dayKey / 10000) % 100;
    missedLogs[missedHead] = {dd, mm, yy, item.startHour, item.startMin, item.endHour, item.endMin};
    missedHead = (missedHead + 1) % MAX_LOGS;
    if (missedCount < MAX_LOGS) missedCount++;
    mLogDirty = true;
}

bool ScheduleModule::clearAllLogs() {
    schedCount = schedHead = missedCount = missedHead = 0;
    memset(schedLogs, 0, sizeof(schedLogs));
    memset(missedLogs, 0, sizeof(missedLogs));
    sLogDirty = mLogDirty = true;
    return flushStorage(true);
}

bool ScheduleModule::factoryReset() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
        instKey[i] = 0;
    }
    beepCount = 2;
    cfgDirty = true;
    digitalWrite(PIN_LED1_SCHED, LOW);
    digitalWrite(PIN_BUZZER, LOW);
    buzzerState = false;
    bool ok = clearAllLogs(); // ye cfg bhi flush karta hai
    return ok;
}

uint16_t ScheduleModule::getTotalScheduleLogs() { return schedCount; }
uint16_t ScheduleModule::getTotalMissedLogs() { return missedCount; }

// Ex. String : &@240-200-22-24-01-24-11-41-20-!   (logNumber 1 = sabse purana)
String ScheduleModule::getScheduleLogPacket(uint16_t logNumber) {
    if (schedCount == 0 || logNumber < 1 || logNumber > schedCount) {
        return "&@240-0-0-00-00-00-00-00-00-!";
    }
    uint16_t idx = (schedHead + MAX_LOGS - schedCount + (logNumber - 1)) % MAX_LOGS;
    const ScheduleLogEntry& e = schedLogs[idx];
    char buf[64];
    snprintf(buf, sizeof(buf), "&@240-%u-%u-%02d-%02d-%02d-%02d-%02d-%02d-!",
             (unsigned)schedCount, (unsigned)logNumber, e.dd, e.mm, e.yy, e.hh, e.min, e.ss);
    return String(buf);
}

// Ex. String : &@250-200-22-24-01-24-09-30-10-30-!
String ScheduleModule::getMissedLogPacket(uint16_t logNumber) {
    if (missedCount == 0 || logNumber < 1 || logNumber > missedCount) {
        return "&@250-0-0-00-00-00-00-00-00-00-!";
    }
    uint16_t idx = (missedHead + MAX_LOGS - missedCount + (logNumber - 1)) % MAX_LOGS;
    const MissedLogEntry& e = missedLogs[idx];
    char buf[64];
    snprintf(buf, sizeof(buf), "&@250-%u-%u-%02d-%02d-%02d-%02d-%02d-%02d-%02d-!",
             (unsigned)missedCount, (unsigned)logNumber, e.dd, e.mm, e.yy,
             e.startHH, e.startMM, e.endHH, e.endMM);
    return String(buf);
}
