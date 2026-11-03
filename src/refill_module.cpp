#include "refill_module.h"
#include <Preferences.h>

RefillModule refillMod;
static Preferences refillPrefs;

void RefillModule::begin() {
    pinMode(PIN_LED2_REFILL, OUTPUT);
    digitalWrite(PIN_LED2_REFILL, LOW);
    loadFromStorage();
}

void RefillModule::loadFromStorage() {
    refillPrefs.begin("merm_refill", false);
    if (refillPrefs.isKey("refCfg")) {
        refillPrefs.getBytes("refCfg", &cfg, sizeof(cfg));
    } else {
        cfg = {false, 0, 0, 0, 0, 0, 0, true};
    }
    refillPrefs.end();
}

void RefillModule::saveToStorage() {
    refillPrefs.begin("merm_refill", false);
    refillPrefs.putBytes("refCfg", &cfg, sizeof(cfg));
    refillPrefs.end();
}

uint32_t RefillModule::toDateVal(uint8_t dd, uint8_t mm, uint8_t yy) {
    return ((uint32_t)yy * 10000UL) + ((uint32_t)mm * 100UL) + dd;
}

void RefillModule::update(const DateTimeData& currentDT) {
    if (!cfg.configured || cfg.refillDone) {
        digitalWrite(PIN_LED2_REFILL, LOW);
        return;
    }

    uint32_t curVal = toDateVal(currentDT.day, currentDT.month, currentDT.year % 100);
    uint32_t preVal = toDateVal(cfg.preDD, cfg.preMM, cfg.preYY);
    uint32_t dueVal = toDateVal(cfg.dueDD, cfg.dueMM, cfg.dueYY);

    if (curVal >= dueVal) {
        // Due date aa gayi ya cross ho gayi -> Solid LED2 ON
        digitalWrite(PIN_LED2_REFILL, HIGH);
    } else if (curVal >= preVal) {
        // Pre-reminder period -> Blink LED2
        digitalWrite(PIN_LED2_REFILL, (millis() / 500) % 2);
    } else {
        digitalWrite(PIN_LED2_REFILL, LOW);
    }
}

bool RefillModule::setRefillDates(uint8_t preDD, uint8_t preMM, uint8_t preYY,
                                  uint8_t dueDD, uint8_t dueMM, uint8_t dueYY) {
    cfg.configured = true;
    cfg.preDD = preDD;
    cfg.preMM = preMM;
    cfg.preYY = preYY;
    cfg.dueDD = dueDD;
    cfg.dueMM = dueMM;
    cfg.dueYY = dueYY;
    cfg.refillDone = false;
    saveToStorage();
    return true;
}

bool RefillModule::setRefillDone() {
    cfg.refillDone = true;
    digitalWrite(PIN_LED2_REFILL, LOW);
    saveToStorage();
    return true;
}

void RefillModule::resetRefill() {
    cfg = {false, 0, 0, 0, 0, 0, 0, true};
    digitalWrite(PIN_LED2_REFILL, LOW);
    saveToStorage();
}

String RefillModule::getRefillDateString() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d-%02d/%02d/%02d",
             cfg.preDD, cfg.preMM, cfg.preYY,
             cfg.dueDD, cfg.dueMM, cfg.dueYY);
    return String(buf);
}