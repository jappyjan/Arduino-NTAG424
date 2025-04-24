#pragma once

#include "server_connection_interface.hpp"
#include "pins.hpp"

#include <EthernetENC.h>

class ServerConnectionEthernet : public ServerConnectionInterface
{
public:
    void setup() override;
    bool isHealthy() override;
    void loop() override;
    IPAddress getCurrentIp() override;
    void end() override;
};