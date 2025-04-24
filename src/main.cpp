#include "server_connection.hpp"
#include "pins.hpp"

#include <SPI.h>

ServerConnection server_connection;

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println("FABReader starting...");

  // Initialize SPI with custom pins
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI); // Use defined SCK, MISO, MOSI

  server_connection.setup();
}

void loop()
{
}