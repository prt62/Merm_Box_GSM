#ifndef REFILL_MODULE_H
#define REFILL_MODULE_H

#include "config.h"

class RefillModule {
public:
    void begin();
    void update(const DateTimeData& currentDT);

    // Command 104: *$104,preDD,preMM,preYY,dueDD,dueMM,dueYY,#
    bool setRefillDates(int preDD, int preMM, int preYY,
                        int dueDD, int dueMM, int dueYY);

    // Command 106: *$106,#
    bool setRefillDone();
    bool resetRefill();

    String getRefillDateString();

private:
    RefillConfig cfg;
    void loadFromStorage();
    bool saveToStorage();
    uint32_t toDateVal(uint8_t dd, uint8_t mm, uint8_t yy);
    static bool validDate(int dd, int mm, int yy);
};

extern RefillModule refillMod;

#endif // REFILL_MODULE_H
