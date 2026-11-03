#include "refill_module.h"
#include <Preferences.h>

RefillModule refillMod;
static Preferences refillPrefs;

static const RefillConfig REFILL_DEFAULT = {false, 0, 0, 0, 0, 0, 0, true};

bool RefillModule::validDate(int dd, int mm, int yy) {
    if (yy < 0 || yy > 99 || mm < 1 || mm > 12 || dd < 1) return false;
    static const uint8_t dim[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // Feb: leap-safe upper bound
    return dd <= dim[mm - 1];
}

void RefillModule::begin() {
    pinMode(PIN_LED2_REFILL, OUTPUT);
    digitalWrite(PIN_LED2_REFILL, LOW);
    loadFromStorage();
}

void RefillModule::loadFromStorage() {
    cfg = REFILL_DEFAULT;
    if (!refillPrefs.begin("merm_refill", false)) return;
    if (refillPrefs.getBytesLength("refCfg") == sizeof(cfg)) {
        RefillConfig tmp;
        refillPrefs.getBytes("refCfg", &tmp, sizeof(tmp));
        // Sanity: corrupt data ko reject karein
        if (!tmp.configured ||
            (validDate(tmp.preDD, tmp.preMM, tmp.preYY) && validDate(tmp.dueDD, tmp.dueMM, tmp.dueYY))) {
            cfg = tmp;
            cfg.configured = tmp.configured ? true : false;
            cfg.refillDone = tmp.refillDone ? true : false;
        }
    }
    refillPrefs.end();
}

bool RefillModule::saveToStorage() {
    if (!refillPrefs.begin("merm_refill", false)) return false;
    bool ok = (refillPrefs.putBytes("refCfg", &cfg, sizeof(cfg)) == sizeof(cfg));
    refillPrefs.end();
    return ok;
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

bool RefillModule::setRefillDates(int preDD, int preMM, int preYY,
                                  int dueDD, int dueMM, int dueYY) {
    if (preYY >= 2000 && preYY <= 2099) preYY -= 2000; // 4-digit year bhi chalega
    if (dueYY >= 2000 && dueYY <= 2099) dueYY -= 2000;
    if (!validDate(preDD, preMM, preYY) || !validDate(dueDD, dueMM, dueYY)) return false;
    if (toDateVal(preDD, preMM, preYY) > toDateVal(dueDD, dueMM, dueYY)) return false; // pre-reminder due se pehle

    cfg.configured = true;
    cfg.preDD = preDD; cfg.preMM = preMM; cfg.preYY = preYY;
    cfg.dueDD = dueDD; cfg.dueMM = dueMM; cfg.dueYY = dueYY;
    cfg.refillDone = false;
    return saveToStorage();
}

bool RefillModule::setRefillDone() {
    cfg.refillDone = true;
    digitalWrite(PIN_LED2_REFILL, LOW);
    return saveToStorage();
}

bool RefillModule::resetRefill() {
    cfg = REFILL_DEFAULT;
    digitalWrite(PIN_LED2_REFILL, LOW);
    return saveToStorage();
}

String RefillModule::getRefillDateString() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d-%02d/%02d/%02d",
             cfg.preDD, cfg.preMM, cfg.preYY,
             cfg.dueDD, cfg.dueMM, cfg.dueYY);
    return String(buf);
}
