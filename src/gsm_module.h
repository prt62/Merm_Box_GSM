#ifndef GSM_MODULE_H
#define GSM_MODULE_H

#include "config.h"

class GSMModule {
public:
    void begin();
    void update();
    void sendResponseToServer(const String& resp);

private:
    enum GsmState {
        GSM_OFF,
        GSM_POWER_EN,
        GSM_PWRKEY_PULSE,
        GSM_WAIT_READY,
        GSM_INIT_AT,
        GSM_ONLINE
    };

    GsmState state = GSM_OFF;
    unsigned long stateTimer = 0;
    String rxBuffer = "";

    void sendAT(const String& cmd);
    void processIncomingLine(const String& line);
};

extern GSMModule gsmMod;

#endif // GSM_MODULE_H