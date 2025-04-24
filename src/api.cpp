#include "api.hpp"

#include "configuration.hpp"

API::API()
{
}

API::~API()
{
}

void API::setup()
{
    Serial.println("Setting up API...");

    if (webSocket.available())
    {
        webSocket.close();
        delay(100);
    }

    webSocket.onMessage(this->handleWebSocketMessage);
    webSocket.onEvent(this->handleWebSocketEvent);

    Serial.printf("[WSc] Attempting to connect to ws://%s:%d\n", SERVER_HOSTNAME, SERVER_PORT);
    bool connected = webSocket.connect(SERVER_HOSTNAME, SERVER_PORT, "api/fab-reader/ws");

    if (!connected)
    {
        Serial.println("[WSc] WebSocket connect() call failed immediately.");
        return;
    }

    Serial.println("[WSc] WebSocket connect() call seemingly successful (initial check). Waiting for Open event.");
}

void API::handleWebSocketMessage(WebsocketsMessage message)
{
    Serial.println("Received message from server: " + message.data());
    if (!message.isText())
    {
        Serial.println("Received non-text message from server, ignoring it: " + message.data());
        return;
    }

    String payload = message.data();

    /**
     * Expected payload format:
     * ACTION <4 chars>
     * DATA <rest of payload>
     */
    String action = payload.substring(0, 4);
    String data = payload.substring(4);

    if (action == "PING")
    {
        Serial.println("Received PING from server, sending PONG");
        webSocket.send("PONG");
    }
}

void API::handleWebSocketEvent(WebsocketsEvent event, String data)
{
    Serial.println("Received event from server: " + data);
}