#ifndef SCHEDULE_MODULE_H
#define SCHEDULE_MODULE_H

#include "config.h"

// NOTE: Ye module sirf main loop task se chalna chahiye (single-threaded).
// BLE / GSM / UART commands main.cpp me queue ho kar loop() me process hote hain.
class ScheduleModule {
public:
    void begin();
    void update(); // Main loop me call hoga

    // Configuration Commands (int params: range check yahin hota hai, uint8_t wrap nahi hota)
    // Return true sirf tab jab naya config RAM + NVS dono me commit ho gaya.
    // Flash write fail ho to purana config RAM me wapas aa jata hai (ACK = fail).
    bool setSchedule(int idx, int sHr, int sMin, int eHr, int eMin);
    bool deleteSchedule(int idx);
    bool setBeepCount(int count);
    uint8_t getBeepCount();

    // RTC Date & Time (105 & 202)
    // true sirf tab jab RX8900 me time likh kar read-back se verify ho gaya.
    bool setDateTime(int dd, int mm, int yyyy, int hh, int min, int ss);
    DateTimeData getDateTime();
    bool isTimeValid() const { return timeValid; } // false: RTC power-loss/missing aur 105 abhi nahi aaya
    bool isRtcOk() const { return rtcOk; }
    String getDateTimeString();     // Format: DD-MM-YYYY-HH-MM-SS
    String getAllSchedulesString(); // For 201 Device Details

    // Logs Management (110, 205, 206, 240, 250)
    bool clearAllLogs();
    bool factoryReset();
    uint16_t getTotalScheduleLogs(); // stored logs (max MAX_LOGS)
    uint16_t getTotalMissedLogs();   // stored logs (max MAX_LOGS)
    String getScheduleLogPacket(uint16_t logNumber); // 1 = oldest stored log
    String getMissedLogPacket(uint16_t logNumber);   // 1 = oldest stored log

    bool isBoxOpen();

private:
    enum RtcRead : uint8_t { RTC_READ_OK, RTC_READ_FAIL, RTC_READ_VLF };

    ScheduleItem schedules[MAX_SCHEDULES];
    uint32_t instKey[MAX_SCHEDULES];   // YYYYMMDD of the window instance in progress (0 = idle)
    uint32_t doneKey[MAX_SCHEDULES];   // YYYYMMDD of the last completed instance (time jump-back guard)
    uint8_t beepCount = 2;

    // Ring buffers (oldest-first when read): head = next write slot
    uint16_t schedCount = 0, schedHead = 0;
    uint16_t missedCount = 0, missedHead = 0;
    ScheduleLogEntry schedLogs[MAX_LOGS];
    MissedLogEntry missedLogs[MAX_LOGS];

    // Dirty flags -> sirf wahi blob flash par likhte hain jo badla ho
    bool cfgDirty = false, sLogDirty = false, mLogDirty = false;
    bool flushBackoff = false;          // last periodic flush failed -> wait FLUSH_RETRY_MS
    uint32_t nextFlushTryMs = 0;

    uint32_t lastRtcTick = 0;
    uint32_t lastBeepToggle = 0;
    uint32_t lastRtcRepairMs = 0;
    bool rtcRepairTried = false;
    bool buzzerState = false;
    bool rtcOk = false;      // last RX8900 read was valid and VLF = 0
    bool timeValid = false;  // currentDT is trustworthy (RTC read or accepted 105)
    DateTimeData currentDT = {3, 11, 2026, 9, 0, 0};

    // Magnetic sensor debounce + latch (chhota open pulse miss na ho)
    bool dbRaw = false, dbStable = false, openLatch = false;
    uint32_t dbChangeMs = 0;

    bool saveConfig();
    bool commitConfig();
    bool saveSchedLogs();
    bool saveMissedLogs();
    bool saveLogCounters();
    bool flushStorage(bool force);
    void loadFromStorage();
    void setDefaults();

    void pollMagSensor(uint32_t nowMs);
    void evaluateSchedules();
    void openInstance(int i, uint32_t key);
    void closeInstance(int i);
    void reconcileAfterTimeChange();
    void outputsIdle();
    void updateBuzzer(uint32_t nowMs);
    void clockTick(uint32_t ticks);
    void advanceSoftware(uint32_t seconds);

    void recordScheduleLog(const DateTimeData& dt);
    void recordMissedLog(uint32_t dayKey, const ScheduleItem& item);

    RtcRead readRtc(DateTimeData& out);
    bool writeRtcVerified(const DateTimeData& dt);
    uint8_t decToBcd(uint8_t val);
    uint8_t bcdToDec(uint8_t val);
};

extern ScheduleModule scheduleMod;

#endif // SCHEDULE_MODULE_H
