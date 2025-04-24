#pragma once

#include "server_connection_interface.hpp"

#include <WiFi.h>
#include <WiFiClient.h>

#include "configuration.hpp"

class ServerConnectionWifi : public ServerConnectionInterface
{
public:
    void setup() override;
    bool isHealthy() override;
    void loop() override;
    IPAddress getCurrentIp() override;
    void end() override;
};