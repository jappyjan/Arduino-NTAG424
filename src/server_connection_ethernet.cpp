#include "server_connection_ethernet.hpp"

void ServerConnectionEthernet::setup()
{
    Serial.println("Setting up Ethernet...");

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    Ethernet.init(PIN_SPI_CS_ETH);

    Serial.println("Verifying Ethernet hardware...");

    if (Ethernet.hardwareStatus() == EthernetNoHardware)
    {
        Serial.println("Ethernet hardware not found");
        delay(5000);
    }

    Ethernet.begin(mac);

    int attempts = 0;
    while (!this->isHealthy() && attempts < 60)
    {
        Serial.println("Waiting for Ethernet connection...");
        delay(1000);
        attempts++;
    }

    if (!this->isHealthy())
    {
        Serial.println("Failed to connect to Ethernet");
        ESP.restart();
    }
}

bool ServerConnectionEthernet::isHealthy()
{
    return Ethernet.linkStatus() == LinkON;
}

IPAddress ServerConnectionEthernet::getCurrentIp()
{
    return Ethernet.localIP();
}

void ServerConnectionEthernet::end()
{
    Ethernet.end();
}

void ServerConnectionEthernet::loop()
{
    Ethernet.maintain();
}