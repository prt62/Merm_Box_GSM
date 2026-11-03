#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==================== SCHEMATIC PIN DEFINITIONS ====================
// I2C Pins (RX8900CE RTC, AT24C64 EEPROM, OLED, AHT21B)
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22
#define PIN_RTC_INT         14
#define PIN_OLED_EN         5

// SPI Flash (W25Q64)
#define PIN_FLASH_CS        4
#define PIN_FLASH_CLK       18
#define PIN_FLASH_MISO      19
#define PIN_FLASH_MOSI      23

// Magnetic Sensor, LEDs & Buzzer
#define PIN_MAG_SENSOR      12  // SW (S1) with 47K pull-up R26
#define PIN_BUZZER          13  // P1 ADD. CONN. (Pin 2 - TX/IO13)
#define PIN_LED1_SCHED      32  // LED1 (Schedule Alert LED)
#define PIN_LED2_REFILL     33  // LED2 (Refill Alert LED)

// GSM MC65 / MC60 Module Pins
#define PIN_VGSM_EN         25  // High = Turn ON VGSM MOSFET Q1 via Q2
#define PIN_GSM_PWR_KEY     26  // High pulse pulls PWRKEY low via Q3
#define PIN_GSM_TX          16  // ESP32 IO16 -> MC65 GSM_TX net
#define PIN_GSM_RX          17  // ESP32 IO17 -> MC65 GSM_RX net
#define PIN_GSM_DTR         2   // GSM DTR control

// Power Monitoring Pins
#define PIN_IN_PWR_SENSE    34  // USB 5V input sense
#define PIN_BV_SENSE        35  // Battery voltage sense

// Magnetic Sensor State (LOW = Box Closed, HIGH = Box Opened due to R26 Pull-up)
#define BOX_OPEN_STATE      HIGH
#define BOX_CLOSED_STATE    LOW

#define MAX_SCHEDULES       5
#define MAX_LOGS            250

// ==================== DATA STRUCTURES ====================
struct DateTimeData {
    uint8_t day;
    uint8_t month;
    uint16_t year; // Full year e.g., 2026
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

struct ScheduleItem {
    bool enabled;
    uint8_t schedNo;     // 1 to 5
    uint8_t startHour;   // 0 to 23
    uint8_t startMin;    // 0 to 59
    uint8_t endHour;     // 0 to 23
    uint8_t endMin;      // 0 to 59
    // Runtime tracking per day
    uint8_t lastTriggeredDay;
    bool takenToday;
    bool missedLoggedToday;
    bool alertActive;
    uint8_t beepsDone;
};

struct RefillConfig {
    bool configured;
    uint8_t preDD, preMM, preYY;
    uint8_t dueDD, dueMM, dueYY;
    bool refillDone;
};

// Code 240: Schedule Log (Jab box time par khola gaya)
struct ScheduleLogEntry {
    uint8_t dd, mm, yy;
    uint8_t hh, min, ss;
};

// Code 250: Missed Log (Jab schedule khatam hone tak box nahi khula)
struct MissedLogEntry {
    uint8_t dd, mm, yy;
    uint8_t startHH, startMM;
    uint8_t endHH, endMM;
};

enum CommandSource {
    SRC_UART = 0,
    SRC_BLE_APP = 1,
    SRC_GSM_SERVER = 2
};

#endif // CONFIG_H