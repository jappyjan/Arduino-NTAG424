#pragma once

#include <ArduinoWebsockets.h>

class API
{
public:
    API();
    ~API();

    void setup();

private:
    WebsocketsClient webSocket;

    void handleWebSocketMessage(WebsocketsMessage message);
    void handleWebSocketEvent(WebsocketsEvent event, String data);
};
