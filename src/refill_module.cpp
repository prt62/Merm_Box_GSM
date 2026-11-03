#include "refill_module.h"
#include <Preferences.h>

RefillModule refillMod;
static Preferences refillPrefs;

static const RefillConfig REFILL_DEFAULT = {false, 0, 0, 0, 0, 0, 0, true};
static const size_t REFILL_REC = 8;   // NVS "refCfg" byte layout (unchanged): cfg, pre d/m/y, due d/m/y, done
static_assert(sizeof(RefillConfig) == REFILL_REC, "RefillConfig layout changed: NVS compatibility");

// yy = 0..99 => 2000..2099. 29-Feb sirf leap year me valid.
bool RefillModule::validDate(int dd, int mm, int yy) {
    if (yy < 0 || yy > 99 || mm < 1 || mm > 12 || dd < 1) return false;
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int year = 2000 + yy;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    const int maxDay = (mm == 2 && leap) ? 29 : dim[mm - 1];
    return dd <= maxDay;
}

void RefillModule::begin() {
    pinMode(PIN_LED2_REFILL, OUTPUT);
    digitalWrite(PIN_LED2_REFILL, LOW);
    loadFromStorage();
}

void RefillModule::loadFromStorage() {
    cfg = REFILL_DEFAULT;
    if (!refillPrefs.begin("merm_refill", false)) {
        Serial.println("[NVS] merm_refill open failed: refill reminder disabled");
        return;
    }
    uint8_t b[REFILL_REC];
    if (refillPrefs.getBytesLength("refCfg") == sizeof(b) &&
        refillPrefs.getBytes("refCfg", b, sizeof(b)) == sizeof(b)) {
        // bools raw byte se decode (0/1 ke alawa value UB nahi banti)
        RefillConfig t = {b[0] != 0, b[1], b[2], b[3], b[4], b[5], b[6], b[7] != 0};
        bool ok = !t.configured ||
                  (validDate(t.preDD, t.preMM, t.preYY) && validDate(t.dueDD, t.dueMM, t.dueYY) &&
                   toDateVal(t.preDD, t.preMM, t.preYY) <= toDateVal(t.dueDD, t.dueMM, t.dueYY));
        if (ok) cfg = t;
    }
    refillPrefs.end();
}

bool RefillModule::saveToStorage(const RefillConfig& c) {
    if (!refillPrefs.begin("merm_refill", false)) return false;
    const uint8_t b[REFILL_REC] = {(uint8_t)(c.configured ? 1 : 0), c.preDD, c.preMM, c.preYY,
                                   c.dueDD, c.dueMM, c.dueYY, (uint8_t)(c.refillDone ? 1 : 0)};
    bool ok = (refillPrefs.putBytes("refCfg", b, sizeof(b)) == sizeof(b));
    refillPrefs.end();
    return ok;
}

uint32_t RefillModule::toDateVal(uint8_t dd, uint8_t mm, uint8_t yy) {
    return ((uint32_t)yy * 10000UL) + ((uint32_t)mm * 100UL) + dd;
}

void RefillModule::update(const DateTimeData& currentDT, bool timeValid) {
    if (!timeValid || !cfg.configured || cfg.refillDone) {
        digitalWrite(PIN_LED2_REFILL, LOW);
        return;
    }

    uint32_t curVal = toDateVal(currentDT.day, currentDT.month, (uint8_t)(currentDT.year % 100));
    uint32_t preVal = toDateVal(cfg.preDD, cfg.preMM, cfg.preYY);
    uint32_t dueVal = toDateVal(cfg.dueDD, cfg.dueMM, cfg.dueYY);

    if (curVal >= dueVal) {
        // Due date aa gayi ya cross ho gayi -> Solid LED2 ON
        digitalWrite(PIN_LED2_REFILL, HIGH);
    } else if (curVal >= preVal) {
        // Pre-reminder period -> Blink LED2
        digitalWrite(PIN_LED2_REFILL, (uint8_t)((millis() / 500) % 2));
    } else {
        digitalWrite(PIN_LED2_REFILL, LOW);
    }
}

bool RefillModule::setRefillDates(int preDD, int preMM, int preYY,
                                  int dueDD, int dueMM, int dueYY) {
    if (preYY >= 2000 && preYY <= 2099) preYY -= 2000; // 4-digit year bhi chalega
    if (dueYY >= 2000 && dueYY <= 2099) dueYY -= 2000;
    if (!validDate(preDD, preMM, preYY) || !validDate(dueDD, dueMM, dueYY)) return false;
    if (toDateVal((uint8_t)preDD, (uint8_t)preMM, (uint8_t)preYY) >
        toDateVal((uint8_t)dueDD, (uint8_t)dueMM, (uint8_t)dueYY)) return false; // pre-reminder due se pehle

    RefillConfig next = {true, (uint8_t)preDD, (uint8_t)preMM, (uint8_t)preYY,
                         (uint8_t)dueDD, (uint8_t)dueMM, (uint8_t)dueYY, false};
    if (!saveToStorage(next)) return false;   // fail: purana config hi active
    cfg = next;
    return true;
}

bool RefillModule::setRefillDone() {
    if (!cfg.configured) return false;        // kuch configured hi nahi: "done" mark karne ko kuch nahi
    if (cfg.refillDone) return true;          // idempotent
    RefillConfig next = cfg;
    next.refillDone = true;
    if (!saveToStorage(next)) return false;
    cfg = next;
    digitalWrite(PIN_LED2_REFILL, LOW);
    return true;
}

bool RefillModule::resetRefill() {
    if (!saveToStorage(REFILL_DEFAULT)) return false;
    cfg = REFILL_DEFAULT;
    digitalWrite(PIN_LED2_REFILL, LOW);
    return true;
}

String RefillModule::getRefillDateString() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d-%02d/%02d/%02d",
             cfg.preDD, cfg.preMM, cfg.preYY,
             cfg.dueDD, cfg.dueMM, cfg.dueYY);
    return String(buf);
}
