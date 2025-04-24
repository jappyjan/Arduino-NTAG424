#include "server_connection.hpp"

ServerConnection::ServerConnection()
{
    // Constructor implementation
}

ServerConnection::~ServerConnection()
{
    if (this->health_check_task != NULL)
    {
        vTaskDelete(this->health_check_task);
    }
    if (this->interface_loop_task != NULL)
    {
        vTaskDelete(this->interface_loop_task);
    }
    this->interface.end();
}

void ServerConnection::setup()
{
    Serial.println("Setting up server connection...");
    this->interface.setup();

    Serial.println("Starting server connection task...");
    xTaskCreate([](void *param)
                { static_cast<ServerConnection *>(param)->health_check_task_handler(); }, "ServerConnection_Healthcheck", 2048, this, 5, &this->health_check_task);

    xTaskCreate([](void *param)
                { static_cast<ServerConnection *>(param)->interface_loop_task_handler(); }, "ServerConnection_InterfaceLoop", 2048, this, 5, &this->interface_loop_task);
}

void ServerConnection::health_check_task_handler()
{
    while (true)
    {
        bool is_currently_healthy = this->interface.isHealthy();

        if (is_currently_healthy != is_healthy)
        {
            if (is_currently_healthy)
            {
                Serial.println("Internet connection established, current IP: " + this->interface.getCurrentIp().toString());
            }

            if (!is_currently_healthy)
            {
                Serial.println("Internet connection lost");
            }
        }

        is_healthy = is_currently_healthy;

        vTaskDelay(1000);
    }
}

void ServerConnection::interface_loop_task_handler()
{
    while (true)
    {
        this->interface.loop();
    }
}