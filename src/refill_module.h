#ifndef REFILL_MODULE_H
#define REFILL_MODULE_H

#include "config.h"

class RefillModule {
public:
    void begin();
    void update(const DateTimeData& currentDT);

    // Command 104: *$104,preDD,preMM,preYY,dueDD,dueMM,dueYY,#
    bool setRefillDates(uint8_t preDD, uint8_t preMM, uint8_t preYY,
                        uint8_t dueDD, uint8_t dueMM, uint8_t dueYY);

    // Command 106: *$106,#
    bool setRefillDone();
    void resetRefill();

    String getRefillDateString();

private:
    RefillConfig cfg;
    void loadFromStorage();
    void saveToStorage();
    uint32_t toDateVal(uint8_t dd, uint8_t mm, uint8_t yy);
};

extern RefillModule refillMod;

#endif // REFILL_MODULE_H