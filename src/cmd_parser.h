#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#include "config.h"

typedef void (*BleSendCallback)(const String& data);

class CommandParser {
public:
    void setBleCallback(BleSendCallback cb);
    void processCommand(const String& rawCmd, CommandSource src);

private:
    BleSendCallback bleSend = nullptr;
    void sendReply(const String& reply, CommandSource src);
    int splitCSV(const String& str, String* tokens, int maxTokens);
};

extern CommandParser cmdParser;

#endif // CMD_PARSER_H