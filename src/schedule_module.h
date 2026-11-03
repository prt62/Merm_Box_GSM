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
    bool setSchedule(int idx, int sHr, int sMin, int eHr, int eMin);
    bool deleteSchedule(int idx);
    bool setBeepCount(int count);
    uint8_t getBeepCount();

    // RTC Date & Time (105 & 202)
    bool setDateTime(int dd, int mm, int yyyy, int hh, int min, int ss);
    DateTimeData getDateTime();
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
    ScheduleItem schedules[MAX_SCHEDULES];
    uint32_t instKey[MAX_SCHEDULES];   // YYYYMMDD of the window instance in progress (0 = idle)
    uint8_t beepCount = 2;

    // Ring buffers (oldest-first when read): head = next write slot
    uint16_t schedCount = 0, schedHead = 0;
    uint16_t missedCount = 0, missedHead = 0;
    ScheduleLogEntry schedLogs[MAX_LOGS];
    MissedLogEntry missedLogs[MAX_LOGS];

    // Dirty flags -> sirf wahi blob flash par likhte hain jo badla ho
    bool cfgDirty = false, sLogDirty = false, mLogDirty = false;
    uint32_t nextFlushTryMs = 0;

    uint32_t lastRtcTick = 0;
    uint32_t lastBeepToggle = 0;
    bool buzzerState = false;
    bool rtcOk = false;
    DateTimeData currentDT = {3, 11, 2026, 9, 0, 0};

    // Magnetic sensor debounce + latch (chhota open pulse miss na ho)
    bool dbRaw = false, dbStable = false, openLatch = false;
    uint32_t dbChangeMs = 0;

    bool saveConfig();
    bool saveSchedLogs();
    bool saveMissedLogs();
    bool flushStorage(bool force);
    void loadFromStorage();

    void pollMagSensor(uint32_t nowMs);
    void evaluateSchedules();
    void updateBuzzer(uint32_t nowMs);
    void resetRuntime();
    void softwareTick();

    void recordScheduleLog(const DateTimeData& dt);
    void recordMissedLog(uint32_t dayKey, const ScheduleItem& item);

    bool syncRTCRead();
    bool syncRTCWrite(const DateTimeData& dt);
    uint8_t decToBcd(uint8_t val);
    uint8_t bcdToDec(uint8_t val);
};

extern ScheduleModule scheduleMod;

#endif // SCHEDULE_MODULE_H
