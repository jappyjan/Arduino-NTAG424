#include "server_connection_wifi.hpp"

void ServerConnectionWifi::setup()
{
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool ServerConnectionWifi::isHealthy()
{
    return WiFi.status() == WL_CONNECTED;
}

void ServerConnectionWifi::loop()
{
}

IPAddress ServerConnectionWifi::getCurrentIp()
{
    return WiFi.localIP();
}

void ServerConnectionWifi::end()
{
    WiFi.disconnect();
}