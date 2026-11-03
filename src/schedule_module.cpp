#include "schedule_module.h"
#include <Preferences.h>
#include <Wire.h>

// ---------------------------------------------------------------- RX8900CE (Epson ETM45E application manual)
#define RX8900_I2C_ADDR   0x32   // 0110010b
#define RX8900_REG_SEC    0x00   // 0x00..0x06 SEC MIN HOUR WEEK DAY MONTH YEAR (BCD, WEEK one-hot bit0 = SUN)
#define RX8900_REG_EXT    0x0D   // Extension: TEST WADA USEL TE FSEL1 FSEL0 TSEL1 TSEL0
#define RX8900_REG_FLAG   0x0E   // Flag: b5 UF, b4 TF, b3 AF, b1 VLF, b0 VDET (b7, b6, b2 unused)
#define RX8900_REG_CTRL   0x0F   // Control: CSEL1 CSEL0 UIE TIE AIE - - RESET
#define RX8900_FLAG_VLF   0x02   // 1 = supply dropped / oscillation stopped -> time data lost
#define RX8900_EXT_INIT   0x02   // power-on default: TEST=0 (must always be 0), TE=0, FSEL=00, TSEL=10
#define RX8900_CTRL_INIT  0x40   // power-on default: CSEL=01 (2 s compensation), UIE=TIE=AIE=0, RESET=0

static const uint32_t MAG_DEBOUNCE_MS     = 40;
static const uint32_t BEEP_PERIOD_MS      = 400;
static const uint32_t FLUSH_RETRY_MS      = 30000;
static const uint32_t RTC_JUMP_TOLERANCE  = 5;      // s: bigger RTC jumps need a confirming second read
static const uint32_t RTC_REPAIR_EVERY_MS = 60000;

// NVS layout (namespace "merm_sched"). Byte layout of every key is unchanged from the previous
// firmware so a field upgrade keeps all user data and an older firmware can still read it.
//   ver(u8)=2  beepCnt(u8)  schedData(55 B: 5 x 11 B ScheduleItem)  inst(20 B)  done(20 B, new in v2)
//   sLogs(1500 B) sHead/sCnt(u16)   mLogs(1750 B) mHead/mCnt(u16)   legacy: totSLogs/totMLogs(u16)
static const char*   NVS_NS         = "merm_sched";
static const uint8_t NVS_SCHEMA_VER = 2;
static const size_t  SCHED_REC      = 11;

static_assert(sizeof(ScheduleItem) == SCHED_REC, "ScheduleItem layout changed: NVS compatibility");
static_assert(sizeof(ScheduleLogEntry) == 6, "ScheduleLogEntry layout changed: NVS compatibility");
static_assert(sizeof(MissedLogEntry) == 7, "MissedLogEntry layout changed: NVS compatibility");

ScheduleModule scheduleMod;
static Preferences prefs;

uint8_t ScheduleModule::decToBcd(uint8_t val) { return (uint8_t)(((val / 10) << 4) | (val % 10)); }
uint8_t ScheduleModule::bcdToDec(uint8_t val) { return (uint8_t)((val >> 4) * 10 + (val & 0x0F)); }

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
    if (m < 3) y = (uint16_t)(y - 1);
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

static bool bcdOk(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

// seconds since 2000-01-01 00:00:00 (valid for 2000..2099)
static uint32_t toEpoch(const DateTimeData& t) {
    uint32_t days = 0;
    for (uint16_t y = 2000; y < t.year; y++) days += isLeap(y) ? 366 : 365;
    for (uint8_t m = 1; m < t.month; m++) days += daysInMonth(m, t.year);
    days += (uint32_t)(t.day - 1);
    return ((days * 24UL + t.hour) * 60UL + t.minute) * 60UL + t.second;
}

static DateTimeData fromEpoch(uint32_t e) {
    DateTimeData t;
    uint32_t days = e / 86400UL, rem = e % 86400UL;
    t.hour = (uint8_t)(rem / 3600UL);
    t.minute = (uint8_t)((rem % 3600UL) / 60UL);
    t.second = (uint8_t)(rem % 60UL);
    uint16_t y = 2000;
    while (days >= (uint32_t)(isLeap(y) ? 366 : 365)) { days -= isLeap(y) ? 366 : 365; y++; }
    uint8_t m = 1;
    while (days >= daysInMonth(m, y)) { days -= daysInMonth(m, y); m++; }
    t.year = y; t.month = m; t.day = (uint8_t)(days + 1);
    return t;
}

static uint32_t absDiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

// Window of schedule s at minute-of-day cur: normal (start<end) and midnight-crossing (start>end)
static void windowAt(const ScheduleItem& s, uint16_t cur, uint32_t today, uint32_t yest,
                     bool& inWindow, uint32_t& key) {
    const uint16_t st = (uint16_t)(s.startHour * 60 + s.startMin);
    const uint16_t en = (uint16_t)(s.endHour * 60 + s.endMin);
    inWindow = false; key = 0;
    if (st < en) {
        inWindow = (cur >= st && cur < en);
        key = today;
    } else if (st > en) {
        if (cur >= st)     { inWindow = true; key = today; }
        else if (cur < en) { inWindow = true; key = yest;  }
    }
}

// ---------------------------------------------------------------- NVS encoding (explicit bytes: no bool UB)
static void encodeSchedule(const ScheduleItem& s, uint8_t* b) {
    b[0] = s.enabled ? 1 : 0; b[1] = s.schedNo; b[2] = s.startHour; b[3] = s.startMin;
    b[4] = s.endHour; b[5] = s.endMin; b[6] = s.lastTriggeredDay; b[7] = s.takenToday ? 1 : 0;
    b[8] = s.missedLoggedToday ? 1 : 0; b[9] = s.alertActive ? 1 : 0; b[10] = s.beepsDone;
}
static void decodeSchedule(const uint8_t* b, ScheduleItem& s) {
    s.enabled = b[0] != 0; s.schedNo = b[1]; s.startHour = b[2]; s.startMin = b[3];
    s.endHour = b[4]; s.endMin = b[5]; s.lastTriggeredDay = b[6]; s.takenToday = b[7] != 0;
    s.missedLoggedToday = b[8] != 0; s.alertActive = b[9] != 0; s.beepsDone = b[10];
}
static ScheduleItem defaultSchedule(int i) {
    ScheduleItem s = {false, (uint8_t)(i + 1), 0, 0, 0, 0, 0, false, false, false, 0};
    return s;
}

// Ring buffer bookkeeping is written as head then count. While count < MAX_LOGS the invariant is
// head == count, so a write interrupted between the two keys can be repaired at boot.
static void repairRing(uint16_t& head, uint16_t& count) {
    if (count > MAX_LOGS || head >= MAX_LOGS) { head = count = 0; return; }
    if (count < MAX_LOGS && head != count) {
        if (head == (uint16_t)((count + 1) % MAX_LOGS)) count = (uint16_t)(count + 1);
        else count = head;
    }
}

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

    // Time sirf tab valid jab RTC do baar consistent aur VLF=0 de (garbage/floating bus se bachav)
    DateTimeData t1 = currentDT, t2 = currentDT;
    RtcRead r1 = readRtc(t1);
    RtcRead r2 = (r1 == RTC_READ_OK) ? readRtc(t2) : r1;
    if (r1 == RTC_READ_OK && r2 == RTC_READ_OK && absDiff(toEpoch(t1), toEpoch(t2)) <= 1) {
        currentDT = t2;
        rtcOk = timeValid = true;
    } else {
        rtcOk = timeValid = false;
        if (r1 == RTC_READ_VLF && validDateTime(t1)) currentDT = t1; // sirf display ke liye
        Serial.println(r1 == RTC_READ_VLF ? "[RTC] VLF set (backup lost): time invalid, schedules paused until *$105"
                                          : "[RTC] RX8900 not readable: time invalid, schedules paused until *$105");
    }

    dbRaw = dbStable = (digitalRead(PIN_MAG_SENSOR) == BOX_OPEN_STATE);
    dbChangeMs = millis();
    openLatch = false;
    lastRtcTick = millis();
}

void ScheduleModule::setDefaults() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = defaultSchedule(i);
        instKey[i] = 0;
        doneKey[i] = 0;
    }
    beepCount = 2;
    schedCount = schedHead = missedCount = missedHead = 0;
}

// ---------------------------------------------------------------- storage
void ScheduleModule::loadFromStorage() {
    setDefaults();
    if (!prefs.begin(NVS_NS, false)) {
        Serial.println("[NVS] merm_sched open failed: running with defaults");
        return;
    }

    uint8_t b = prefs.getUChar("beepCnt", 2);
    if (b >= 1 && b <= 50) beepCount = b;

    uint8_t raw[SCHED_REC * MAX_SCHEDULES];
    if (prefs.getBytesLength("schedData") == sizeof(raw) &&
        prefs.getBytes("schedData", raw, sizeof(raw)) == sizeof(raw)) {
        for (int i = 0; i < MAX_SCHEDULES; i++) decodeSchedule(raw + i * SCHED_REC, schedules[i]);
    }
    uint32_t keys[MAX_SCHEDULES];
    if (prefs.getBytesLength("inst") == sizeof(keys) && prefs.getBytes("inst", keys, sizeof(keys)) == sizeof(keys)) {
        memcpy(instKey, keys, sizeof(keys));
    }
    if (prefs.getBytesLength("done") == sizeof(keys) && prefs.getBytes("done", keys, sizeof(keys)) == sizeof(keys)) {
        memcpy(doneKey, keys, sizeof(keys));
    }
    // Corrupt/garbage data se bachne ke liye sanitize
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        ScheduleItem& s = schedules[i];
        s.schedNo = (uint8_t)(i + 1);
        uint16_t st = (uint16_t)(s.startHour * 60 + s.startMin), en = (uint16_t)(s.endHour * 60 + s.endMin);
        if (s.startHour > 23 || s.endHour > 23 || s.startMin > 59 || s.endMin > 59 || st == en) {
            s = defaultSchedule(i);
            instKey[i] = doneKey[i] = 0;
        }
        if (instKey[i] != 0 && (instKey[i] < 20000101UL || instKey[i] > 20991231UL)) instKey[i] = 0;
        if (doneKey[i] != 0 && (doneKey[i] < 20000101UL || doneKey[i] > 20991231UL)) doneKey[i] = 0;
        s.alertActive = false; // reboot ke baad alert dobara evaluate hoga
        s.beepsDone = 0;
    }

    if (prefs.getBytesLength("sLogs") == sizeof(schedLogs) &&
        prefs.getBytes("sLogs", schedLogs, sizeof(schedLogs)) == sizeof(schedLogs)) {
        if (prefs.isKey("sCnt")) {
            schedCount = prefs.getUShort("sCnt", 0);
            schedHead  = prefs.getUShort("sHead", 0);
        } else { // purane firmware ka format (total counter)
            uint16_t tot = prefs.getUShort("totSLogs", 0);
            schedCount = tot > MAX_LOGS ? MAX_LOGS : tot;
            schedHead  = (uint16_t)(tot % MAX_LOGS);
        }
    }
    if (prefs.getBytesLength("mLogs") == sizeof(missedLogs) &&
        prefs.getBytes("mLogs", missedLogs, sizeof(missedLogs)) == sizeof(missedLogs)) {
        if (prefs.isKey("mCnt")) {
            missedCount = prefs.getUShort("mCnt", 0);
            missedHead  = prefs.getUShort("mHead", 0);
        } else {
            uint16_t tot = prefs.getUShort("totMLogs", 0);
            missedCount = tot > MAX_LOGS ? MAX_LOGS : tot;
            missedHead  = (uint16_t)(tot % MAX_LOGS);
        }
    }
    repairRing(schedHead, schedCount);
    repairRing(missedHead, missedCount);
    prefs.end();
}

bool ScheduleModule::saveConfig() {
    if (!prefs.begin(NVS_NS, false)) return false;
    uint8_t raw[SCHED_REC * MAX_SCHEDULES];
    for (int i = 0; i < MAX_SCHEDULES; i++) encodeSchedule(schedules[i], raw + i * SCHED_REC);
    bool ok = true;
    if (prefs.getUChar("ver", 0) != NVS_SCHEMA_VER) ok &= (prefs.putUChar("ver", NVS_SCHEMA_VER) == 1);
    ok &= (prefs.putUChar("beepCnt", beepCount) == 1);
    ok &= (prefs.putBytes("schedData", raw, sizeof(raw)) == sizeof(raw));
    ok &= (prefs.putBytes("inst", instKey, sizeof(instKey)) == sizeof(instKey));
    ok &= (prefs.putBytes("done", doneKey, sizeof(doneKey)) == sizeof(doneKey));
    prefs.end();
    return ok;
}

// Command path: persist config now. On failure the caller rolls RAM back.
bool ScheduleModule::commitConfig() {
    if (saveConfig()) { cfgDirty = false; return true; }
    cfgDirty = true;                       // re-assert whatever RAM holds after the rollback
    flushBackoff = true;
    nextFlushTryMs = millis() + FLUSH_RETRY_MS;
    return false;
}

bool ScheduleModule::saveSchedLogs() {
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = true;
    ok &= (prefs.putBytes("sLogs", schedLogs, sizeof(schedLogs)) == sizeof(schedLogs));
    ok &= (prefs.putUShort("sHead", schedHead) == 2);   // head before count (see repairRing)
    ok &= (prefs.putUShort("sCnt", schedCount) == 2);
    prefs.end();
    return ok;
}

bool ScheduleModule::saveMissedLogs() {
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = true;
    ok &= (prefs.putBytes("mLogs", missedLogs, sizeof(missedLogs)) == sizeof(missedLogs));
    ok &= (prefs.putUShort("mHead", missedHead) == 2);
    ok &= (prefs.putUShort("mCnt", missedCount) == 2);
    prefs.end();
    return ok;
}

bool ScheduleModule::saveLogCounters() {
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = true;
    ok &= (prefs.putUShort("sHead", schedHead) == 2);
    ok &= (prefs.putUShort("sCnt", schedCount) == 2);
    ok &= (prefs.putUShort("mHead", missedHead) == 2);
    ok &= (prefs.putUShort("mCnt", missedCount) == 2);
    prefs.end();
    return ok;
}

// force=true: turant likho. force=false: periodic path (fail hone par 30 s backoff).
// Backoff ab ek flag se gate hota hai: pehle nextFlushTryMs=0 ke saath (int32_t)(millis()-0) < 0
// 24.8 din uptime ke baad true ho jata tha aur 24.8 din tak koi log/state flash par nahi jata tha.
bool ScheduleModule::flushStorage(bool force) {
    if (!cfgDirty && !sLogDirty && !mLogDirty) return true;
    if (!force && flushBackoff && (int32_t)(millis() - nextFlushTryMs) < 0) return false;

    bool ok = true;
    if (cfgDirty)  { if (saveConfig())     cfgDirty = false;  else ok = false; }
    if (sLogDirty) { if (saveSchedLogs())  sLogDirty = false; else ok = false; }
    if (mLogDirty) { if (saveMissedLogs()) mLogDirty = false; else ok = false; }
    flushBackoff = !ok;
    if (!ok) nextFlushTryMs = millis() + FLUSH_RETRY_MS;
    return ok;
}

// ---------------------------------------------------------------- RTC
// Ek hi burst me 0x00..0x0E: time + Flag register (VLF) coherent milte hain
// (RX8900 access ke dauran time counter hold rakhta hai).
ScheduleModule::RtcRead ScheduleModule::readRtc(DateTimeData& out) {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_SEC);
    if (Wire.endTransmission() != 0) return RTC_READ_FAIL;
    const uint8_t n = RX8900_REG_FLAG + 1;
    if (Wire.requestFrom((uint8_t)RX8900_I2C_ADDR, n) != n) return RTC_READ_FAIL;

    uint8_t raw[RX8900_REG_FLAG + 1];
    bool allFF = true;
    for (uint8_t i = 0; i < n; i++) {
        int v = Wire.read();
        if (v < 0) return RTC_READ_FAIL;
        raw[i] = (uint8_t)v;
        if (raw[i] != 0xFF) allFF = false;
    }
    if (allFF) return RTC_READ_FAIL;                          // floating / stuck-high bus
    const uint8_t flags = raw[RX8900_REG_FLAG];

    uint8_t sec = raw[0] & 0x7F, mn = raw[1] & 0x7F, hr = raw[2] & 0x3F;
    uint8_t day = raw[4] & 0x3F, mon = raw[5] & 0x1F, yr = raw[6];
    bool decoded = false;
    if (bcdOk(sec) && bcdOk(mn) && bcdOk(hr) && bcdOk(day) && bcdOk(mon) && bcdOk(yr)) {
        DateTimeData t = {bcdToDec(day), bcdToDec(mon), (uint16_t)(2000 + bcdToDec(yr)),
                          bcdToDec(hr), bcdToDec(mn), bcdToDec(sec)};
        if (validDateTime(t)) { out = t; decoded = true; }
    }
    if (flags & RX8900_FLAG_VLF) return RTC_READ_VLF;           // time data lost: never trust it
    return decoded ? RTC_READ_OK : RTC_READ_FAIL;
}

// Datasheet: VLF=1 / power-up ke baad saare registers initialise karo, TEST hamesha 0.
// Order: TEST/ext -> time -> flags+control. Flags (VLF) time likhne ke BAAD clear hote hain, taaki
// adhoora write kabhi "VLF=0 + garbage time" na chhode. Phir read-back se verify.
bool ScheduleModule::writeRtcVerified(const DateTimeData& dt) {
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_EXT);
    Wire.write((uint8_t)RX8900_EXT_INIT);
    if (Wire.endTransmission() != 0) return false;

    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_SEC);
    Wire.write(decToBcd(dt.second));
    Wire.write(decToBcd(dt.minute));
    Wire.write(decToBcd(dt.hour));
    Wire.write((uint8_t)(1 << weekdayOf(dt.year, dt.month, dt.day))); // WEEK one-hot, bit0 = Sunday
    Wire.write(decToBcd(dt.day));
    Wire.write(decToBcd(dt.month));
    Wire.write(decToBcd((uint8_t)(dt.year % 100)));
    if (Wire.endTransmission() != 0) return false;

    // UF/TF/AF/VLF/VDET clear (alarm/timer/update interrupts ye firmware use nahi karta), interrupts off
    Wire.beginTransmission(RX8900_I2C_ADDR);
    Wire.write((uint8_t)RX8900_REG_FLAG);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)RX8900_CTRL_INIT);
    if (Wire.endTransmission() != 0) return false;

    DateTimeData rb = dt;
    if (readRtc(rb) != RTC_READ_OK) return false;
    return absDiff(toEpoch(rb), toEpoch(dt)) <= 2;
}

void ScheduleModule::advanceSoftware(uint32_t seconds) {
    if (!validDateTime(currentDT)) return;
    currentDT = fromEpoch(toEpoch(currentDT) + seconds);
}

// RTC master hai. Software clock sirf tab chalta hai jab RTC read fail ho (kabhi double-count nahi).
void ScheduleModule::clockTick(uint32_t ticks) {
    DateTimeData rt = currentDT;
    RtcRead r = readRtc(rt);

    if (r == RTC_READ_OK) {
        rtcOk = true;
        rtcRepairTried = false;
        if (!timeValid) {                                 // pehla valid time: do consistent reads chahiye
            DateTimeData rt2 = rt;
            if (readRtc(rt2) == RTC_READ_OK && absDiff(toEpoch(rt2), toEpoch(rt)) <= 1) {
                currentDT = rt2;
                timeValid = true;
            } else {
                advanceSoftware(ticks);
            }
            return;
        }
        uint32_t expected = toEpoch(currentDT) + ticks;
        uint32_t got = toEpoch(rt);
        if (absDiff(got, expected) <= RTC_JUMP_TOLERANCE) { currentDT = rt; return; }
        // Bada jump: ek single glitched read window ko jaldi khatam karke false missed log bana deta tha.
        DateTimeData rt2 = rt;
        if (readRtc(rt2) == RTC_READ_OK && absDiff(toEpoch(rt2), got) <= 1) { currentDT = rt2; return; }
        advanceSoftware(ticks);                           // unconfirmed -> is tick ko ignore
        return;
    }

    rtcOk = false;
    advanceSoftware(ticks);
    if (r == RTC_READ_VLF && timeValid) {
        // RTC ne time kho diya par hamare paas valid time hai: RTC wapas set karo (rate-limited)
        uint32_t now = millis();
        if (!rtcRepairTried || (now - lastRtcRepairMs) >= RTC_REPAIR_EVERY_MS) {
            rtcRepairTried = true;
            lastRtcRepairMs = now;
            if (writeRtcVerified(currentDT)) rtcOk = true;
        }
    }
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

    uint32_t elapsed = nowMs - lastRtcTick;
    if (elapsed >= 1000) {
        uint32_t ticks = elapsed / 1000;
        lastRtcTick += ticks * 1000;      // phase-locked: loop jitter se software clock drift nahi hota
        clockTick(ticks);

        if (timeValid) evaluateSchedules();
        else outputsIdle();               // time pata nahi: alert/log fabricated time par nahi
        flushStorage(false);
    }

    updateBuzzer(nowMs);
}

void ScheduleModule::outputsIdle() {
    for (int i = 0; i < MAX_SCHEDULES; i++) schedules[i].alertActive = false;
    openLatch = false;
    digitalWrite(PIN_LED1_SCHED, LOW);
    buzzerState = false;
    digitalWrite(PIN_BUZZER, LOW);
}

void ScheduleModule::openInstance(int i, uint32_t key) {
    ScheduleItem& s = schedules[i];
    instKey[i] = key;
    s.lastTriggeredDay = (uint8_t)(key % 100);
    s.takenToday = false;
    s.missedLoggedToday = false;
    s.alertActive = false;
    s.beepsDone = 0;
    cfgDirty = true;
}

// Window khatam: box nahi khula -> Missed log (sirf ek baar, window ki sahi date ke saath)
void ScheduleModule::closeInstance(int i) {
    ScheduleItem& s = schedules[i];
    if (!s.takenToday && !s.missedLoggedToday) {
        s.missedLoggedToday = true;
        recordMissedLog(instKey[i], s);
    }
    doneKey[i] = instKey[i];
    instKey[i] = 0;
    s.alertActive = false;
    s.beepsDone = 0;
    cfgDirty = true;
}

void ScheduleModule::evaluateSchedules() {
    const uint16_t cur = (uint16_t)(currentDT.hour * 60 + currentDT.minute);
    const uint32_t today = makeDayKey(currentDT.year, currentDT.month, currentDT.day);
    const uint32_t yest = prevDayKey(currentDT);
    const bool opened = openLatch || dbStable;
    openLatch = false;
    bool anyAlertActive = false;

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        ScheduleItem& s = schedules[i];
        if (!s.enabled) continue;

        bool inWindow; uint32_t key;
        windowAt(s, cur, today, yest, inWindow, key);

        if (inWindow && instKey[i] != key) {
            if (instKey[i] != 0) closeInstance(i);          // seedha ek instance se dusre me (RTC jump)
            if (doneKey[i] == key) {                        // ye instance pehle hi complete ho chuka
                s.alertActive = false;                      // (clock peeche gaya) -> dobara trigger nahi
                continue;
            }
            openInstance(i, key);
        }

        if (inWindow) {
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
            closeInstance(i);
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

// Time set (105) ke baad: jo instance naye time par bhi wahi hai (same din, window ke andar) wo
// apni state rakhta hai (app ka time-sync taken dose ko dobara alert/missed nahi banata). Baaki
// instances bina missed log ke band (time change se false missed event nahi).
void ScheduleModule::reconcileAfterTimeChange() {
    const uint16_t cur = (uint16_t)(currentDT.hour * 60 + currentDT.minute);
    const uint32_t today = makeDayKey(currentDT.year, currentDT.month, currentDT.day);
    const uint32_t yest = prevDayKey(currentDT);
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (instKey[i] == 0) continue;
        bool inWindow; uint32_t key;
        windowAt(schedules[i], cur, today, yest, inWindow, key);
        if (schedules[i].enabled && inWindow && key == instKey[i]) continue;
        ScheduleItem& s = schedules[i];
        instKey[i] = 0;
        s.takenToday = false;
        s.missedLoggedToday = false;
        s.alertActive = false;
        s.beepsDone = 0;
        cfgDirty = true;
    }
    openLatch = false;
}

// Non-blocking buzzer: beepCount complete beeps (2 toggles each) per alert instance
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

    const int i = idx - 1;
    ScheduleItem& s = schedules[i];
    // App aksar connect par saare schedules dobara bhejti hai: same schedule = no-op (taken state,
    // alert aur flash wear bache rehte hain)
    if (s.enabled && s.startHour == sHr && s.startMin == sMin && s.endHour == eHr && s.endMin == eMin) {
        return cfgDirty ? commitConfig() : true;
    }

    const ScheduleItem oldItem = s;
    const uint32_t oldInst = instKey[i], oldDone = doneKey[i];
    s = {true, (uint8_t)idx, (uint8_t)sHr, (uint8_t)sMin, (uint8_t)eHr, (uint8_t)eMin, 0, false, false, false, 0};
    instKey[i] = 0;
    doneKey[i] = 0;
    if (commitConfig()) return true;

    s = oldItem; instKey[i] = oldInst; doneKey[i] = oldDone; // rollback: purana valid schedule chalta rahe
    return false;
}

bool ScheduleModule::deleteSchedule(int idx) {
    if (idx < 1 || idx > MAX_SCHEDULES) return false;
    const int i = idx - 1;
    ScheduleItem& s = schedules[i];
    if (!s.enabled && instKey[i] == 0) return cfgDirty ? commitConfig() : true;

    const ScheduleItem oldItem = s;
    const uint32_t oldInst = instKey[i], oldDone = doneKey[i];
    s.enabled = false;
    s.alertActive = false;
    s.takenToday = false;
    s.missedLoggedToday = false;
    s.beepsDone = 0;
    instKey[i] = 0;
    doneKey[i] = 0;
    if (commitConfig()) return true;

    s = oldItem; instKey[i] = oldInst; doneKey[i] = oldDone;
    return false;
}

bool ScheduleModule::setBeepCount(int count) {
    if (count < 1 || count > 50) return false;
    if (beepCount == count) return cfgDirty ? commitConfig() : true;
    const uint8_t old = beepCount;
    beepCount = (uint8_t)count;
    if (commitConfig()) return true;
    beepCount = old;
    return false;
}

uint8_t ScheduleModule::getBeepCount() {
    return beepCount;
}

bool ScheduleModule::setDateTime(int dd, int mm, int yyyy, int hh, int min, int ss) {
    if (yyyy >= 0 && yyyy < 100) yyyy += 2000;
    if (yyyy < 2000 || yyyy > 2099) return false;
    if (dd < 1 || dd > 31 || mm < 1 || mm > 12 || hh < 0 || hh > 23 || min < 0 || min > 59 || ss < 0 || ss > 59) return false;
    DateTimeData t = {(uint8_t)dd, (uint8_t)mm, (uint16_t)yyyy, (uint8_t)hh, (uint8_t)min, (uint8_t)ss};
    if (!validDateTime(t)) return false; // 31 Feb, 29 Feb (non-leap) wagairah reject

    const bool written = writeRtcVerified(t);
    if (!written && rtcOk) {
        // RTC jawab de raha hai par set nahi hua: agle read par purana time wapas aa jata.
        // Isliye kuch mat badlo aur failure ACK do.
        return false;
    }
    // RTC set ho gaya, ya RTC hai hi nahi (tab software clock hi sahara hai; ACK fail = persist nahi hua)
    currentDT = t;
    timeValid = true;
    rtcOk = written;
    lastRtcTick = millis();
    reconcileAfterTimeChange();
    evaluateSchedules();
    flushStorage(true);              // runtime state; iska result 105 ka ACK nahi badalta
    return written;
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
    schedHead = (uint16_t)((schedHead + 1) % MAX_LOGS);
    if (schedCount < MAX_LOGS) schedCount++;
    sLogDirty = true;
}

void ScheduleModule::recordMissedLog(uint32_t dayKey, const ScheduleItem& item) {
    uint8_t dd = (uint8_t)(dayKey % 100), mm = (uint8_t)((dayKey / 100) % 100), yy = (uint8_t)((dayKey / 10000) % 100);
    missedLogs[missedHead] = {dd, mm, yy, item.startHour, item.startMin, item.endHour, item.endMin};
    missedHead = (uint16_t)((missedHead + 1) % MAX_LOGS);
    if (missedCount < MAX_LOGS) missedCount++;
    mLogDirty = true;
}

// Sirf counters likhe jate hain (3.2 KB blobs dobara nahi): kam flash wear, aur fail hone par RAM
// wapas purane counters par (consistent). Partial write ke case me dirty flags poori state dobara likhte hain.
bool ScheduleModule::clearAllLogs() {
    const uint16_t sc = schedCount, sh = schedHead, mc = missedCount, mh = missedHead;
    schedCount = schedHead = missedCount = missedHead = 0;
    if (saveLogCounters()) {
        sLogDirty = mLogDirty = false;
        return true;
    }
    schedCount = sc; schedHead = sh; missedCount = mc; missedHead = mh;
    sLogDirty = mLogDirty = true;
    flushBackoff = true;
    nextFlushTryMs = millis() + FLUSH_RETRY_MS;
    return false;
}

// Namespace erase: NVS me defaults hi bachte hain (legacy keys bhi). Fail ho to RAM unchanged.
bool ScheduleModule::factoryReset() {
    bool ok = prefs.begin(NVS_NS, false);
    if (ok) {
        ok = prefs.clear();
        prefs.end();
    }
    if (!ok) {
        cfgDirty = sLogDirty = mLogDirty = true;   // partial erase possible: re-assert current state
        flushBackoff = true;
        nextFlushTryMs = millis() + FLUSH_RETRY_MS;
        return false;
    }
    setDefaults();
    cfgDirty = sLogDirty = mLogDirty = false;
    digitalWrite(PIN_LED1_SCHED, LOW);
    digitalWrite(PIN_BUZZER, LOW);
    buzzerState = false;
    return true;
}

uint16_t ScheduleModule::getTotalScheduleLogs() { return schedCount; }
uint16_t ScheduleModule::getTotalMissedLogs() { return missedCount; }

// Ex. String : &@240-200-22-24-01-24-11-41-20-!   (logNumber 1 = sabse purana)
String ScheduleModule::getScheduleLogPacket(uint16_t logNumber) {
    if (schedCount == 0 || logNumber < 1 || logNumber > schedCount) {
        return "&@240-0-0-00-00-00-00-00-00-!";
    }
    uint16_t idx = (uint16_t)((schedHead + MAX_LOGS - schedCount + (logNumber - 1)) % MAX_LOGS);
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
    uint16_t idx = (uint16_t)((missedHead + MAX_LOGS - missedCount + (logNumber - 1)) % MAX_LOGS);
    const MissedLogEntry& e = missedLogs[idx];
    char buf[64];
    snprintf(buf, sizeof(buf), "&@250-%u-%u-%02d-%02d-%02d-%02d-%02d-%02d-%02d-!",
             (unsigned)missedCount, (unsigned)logNumber, e.dd, e.mm, e.yy,
             e.startHH, e.startMM, e.endHH, e.endMM);
    return String(buf);
}
