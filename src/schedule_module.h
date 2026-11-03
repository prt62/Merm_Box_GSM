#ifndef SCHEDULE_MODULE_H
#define SCHEDULE_MODULE_H

#include "config.h"

class ScheduleModule {
public:
    void begin();
    void update(); // Main loop me call hoga

    // Configuration Commands
    bool setSchedule(uint8_t idx, uint8_t sHr, uint8_t sMin, uint8_t eHr, uint8_t eMin);
    bool deleteSchedule(uint8_t idx);
    bool setBeepCount(uint8_t count);
    uint8_t getBeepCount();

    // RTC Date & Time (105 & 202)
    bool setDateTime(uint8_t dd, uint8_t mm, uint16_t yyyy, uint8_t hh, uint8_t min, uint8_t ss);
    DateTimeData getDateTime();
    String getDateTimeString();     // Format: DD-MM-YYYY-HH-MM-SS
    String getAllSchedulesString(); // For 201 Device Details

    // Logs Management (110, 205, 206, 240, 250)
    bool clearAllLogs();
    bool factoryReset();
    uint16_t getTotalScheduleLogs();
    uint16_t getTotalMissedLogs();
    String getScheduleLogPacket(uint16_t logNumber); // 1-indexed for 240
    String getMissedLogPacket(uint16_t logNumber);   // 1-indexed for 250

    bool isBoxOpen();

private:
    ScheduleItem schedules[MAX_SCHEDULES];
    uint8_t beepCount = 2;

    uint16_t totalSchedLogs = 0;
    uint16_t totalMissedLogs = 0;
    ScheduleLogEntry schedLogs[MAX_LOGS];
    MissedLogEntry missedLogs[MAX_LOGS];

    unsigned long lastRtcTick = 0;
    unsigned long lastBeepToggle = 0;
    bool buzzerState = false;
    DateTimeData currentDT = {3, 11, 2026, 9, 0, 0};

    void saveToStorage();
    void loadFromStorage();
    void recordScheduleLog(const DateTimeData& dt);
    void recordMissedLog(const DateTimeData& dt, const ScheduleItem& item);
    void syncRTCRead();
    void syncRTCWrite(const DateTimeData& dt);
    uint8_t decToBcd(uint8_t val);
    uint8_t bcdToDec(uint8_t val);
};

extern ScheduleModule scheduleMod;

#endif // SCHEDULE_MODULE_H