#ifndef REFILL_MODULE_H
#define REFILL_MODULE_H

#include "config.h"

class RefillModule {
public:
    void begin();
    // timeValid=false (RTC power-loss/missing, 105 abhi nahi aaya): LED2 off, galat date par alert nahi
    void update(const DateTimeData& currentDT, bool timeValid = true);

    // Command 104: *$104,preDD,preMM,preYY,dueDD,dueMM,dueYY,#  (YY = 2 or 4 digit)
    // true sirf jab dates valid hon aur NVS me commit ho gayi hon (fail = purani dates rehti hain)
    bool setRefillDates(int preDD, int preMM, int preYY,
                        int dueDD, int dueMM, int dueYY);

    // Command 106: *$106,#  -> false agar koi refill configured hi nahi hai
    bool setRefillDone();
    bool resetRefill();

    String getRefillDateString();

private:
    RefillConfig cfg;
    void loadFromStorage();
    bool saveToStorage(const RefillConfig& c);
    static uint32_t toDateVal(uint8_t dd, uint8_t mm, uint8_t yy);
    static bool validDate(int dd, int mm, int yy);
};

extern RefillModule refillMod;

#endif // REFILL_MODULE_H
