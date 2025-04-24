#pragma once

#include "server_connection_interface.hpp"

#ifdef SERVER_CONNECTION_ETHERNET
#ifdef SERVER_CONNECTION_WIFI
#error "Only one server connection type can be defined"
#endif
#endif

#ifndef SERVER_CONNECTION_ETHERNET
#ifndef SERVER_CONNECTION_WIFI
#error "No server connection type defined"
#endif
#endif

#ifdef SERVER_CONNECTION_ETHERNET
#include "server_connection_ethernet.hpp"
#elif SERVER_CONNECTION_WIFI
#include "server_connection_wifi.hpp"
#endif

class ServerConnection
{
public:
    ServerConnection();
    ~ServerConnection();

    void setup();
    void health_check_task_handler();
    void interface_loop_task_handler();

private:
#ifdef SERVER_CONNECTION_ETHERNET
    ServerConnectionEthernet interface;
#elif SERVER_CONNECTION_WIFI
    ServerConnectionWifi interface;
#endif

    TaskHandle_t health_check_task;
    TaskHandle_t interface_loop_task;
    bool is_healthy;
};
