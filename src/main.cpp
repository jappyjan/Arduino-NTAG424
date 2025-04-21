#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_PN532_NTAG424.h>
#include <WiFi.h>
// #include <HTTPClient.h> // Removed: Using WebSockets now
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h> // Added: New WebSocket library
#ifdef ESP32
#include <esp_wifi.h> // Add specific ESP32 WiFi header
#endif
#include <WiFiClient.h> // Add standard WiFiClient for TCP test

// Use the namespace for the ArduinoWebsockets library
using namespace websockets; // Re-introduce namespace

#define WIFI_SSID "Darknet"
#define WIFI_PASSWORD "WirWollen16Kekse"

// --- Server Configuration ---
const char *serverHostname = "192.168.178.91"; // Replace with your server's IP or hostname
const int serverPort = 3000;                   // Default Node.js server port
const char *readerPath = "/reader";            // Define path for reader

// --- PN532 Configuration ---
// #define PN532_SCK (4) // Using HW SPI SCK (GPIO 6)
// #define PN532_MISO (5) // Using HW SPI MISO (GPIO 2)
// #define PN532_MOSI (6) // Using HW SPI MOSI (GPIO 7)
#define PN532_SS (1) // CS pin for PN532 (must be unused)

// Use Hardware SPI
// Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS); // Old constructor
Adafruit_PN532 nfc(PN532_SS); // New constructor for HW SPI

// --- WebSocket Configuration ---
// websockets::WebSocketClient webSocket; // Revert this attempt
// Use WebsocketsClient (plural) with the namespace
WebsocketsClient webSocket;
bool webSocketConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000; // Try to reconnect every 5 seconds

// --- NTAG424 Configuration ---
const uint8_t AUTH_CMD = 0x71; // AuthenticateEV2First

// --- State Management ---
enum ReaderState
{
  STATE_INIT,
  STATE_WIFI_CONNECTING,
  STATE_WEBSOCKET_CONNECTING,
  STATE_PN532_INIT,
  STATE_WAITING_FOR_CARD,
  STATE_CARD_DETECTED,
  STATE_PROCESSING_COMMAND,
  STATE_ERROR
};
ReaderState currentState = STATE_INIT;
String currentCardUid = "";
unsigned long lastCardSeenTime = 0;
// unsigned long lastPollTime = 0; // Removed: No polling needed with WS
unsigned long lastStatusSendTime = 0;
const unsigned long CARD_REMOVAL_TIMEOUT_MS = 1000; // Time after last detection to consider card removed
// const unsigned long COMMAND_POLL_INTERVAL_MS = 1000; // Removed: No polling needed
const unsigned long STATUS_SEND_INTERVAL_MS = 5000; // How often to send status updates

// --- Buffers & Temporary Storage ---
#define MAX_LOG_LINES 10
String commandLogs[MAX_LOG_LINES];
uint8_t logCount = 0;

// --- Function Prototypes ---
void setupWiFi();
void startWiFiAP(); // Kept for potential future use
void setupPN532();
void setupWebSocket();
// void webSocketEvent(WStype_t type, uint8_t *payload, size_t length); // Removed: Old event handler
void handleWebSocketMessage(WebsocketsMessage message);        // Use type directly due to namespace
void handleWebSocketEvent(WebsocketsEvent event, String data); // Use type directly due to namespace
void sendWebSocketMessage(const JsonDocument &doc);
void sendLogOverWebSocket(const String &logMessage);
void printHex(uint8_t *data, uint8_t len);
String uidToString(uint8_t *uid, uint8_t len);
void clearLogs();
void addLog(const String &logMessage);
// void sendStatusToServer(bool cardPresent, const String &uid); // Removed: Replaced by WS
void sendStatusUpdate(); // New function to send status via WS
// void pollForCommand(const String &uid); // Removed: Replaced by WS events
void executeCommand(const JsonObject &cmdPayload);
void handleEnrollCommand(const JsonObject &cmdPayload);
void handleAuthenticateCommand(const JsonObject &cmdPayload);
// void sendCommandResult(const String &uid, bool success, const String &message); // Removed: Replaced by WS
void sendCommandResultWebSocket(const String &uid, bool success, const String &message); // New function for WS
const char *wifiStatusToString(wl_status_t status);
bool testTcpConnection(const char *host, uint16_t port); // Added: TCP Test function prototype

// --- Setup ---
void setup()
{
  Serial.begin(115200);
  while (!Serial)
    delay(10);

  delay(2000);

  Serial.println();
  Serial.println("NTAG424 Proxy Client Starting (WebSocket Mode)...");

  // currentState = STATE_WIFI_CONNECTING; // Old initial state
  currentState = STATE_PN532_INIT; // New initial state: Start with HW init
}

// --- Main Loop ---
void loop()
{
  unsigned long now = millis();

  // Always run the WebSocket poll function
  if (webSocket.available())
  { // Check if connection is active/available
    webSocket.poll();
  }
  else
  {
    // Handle disconnection logic if needed, or rely on the event handler
    if (webSocketConnected)
    { // If we thought we were connected, mark as disconnected
      Serial.println("[WSc] WebSocket seems unavailable, marking as disconnected.");
      webSocketConnected = false;
      // Trigger state change immediately?
      if (currentState != STATE_WIFI_CONNECTING && currentState != STATE_WEBSOCKET_CONNECTING)
      {
        currentState = STATE_WEBSOCKET_CONNECTING;
        lastReconnectAttempt = 0; // Allow immediate reconnect attempt
        currentCardUid = "";      // Reset card state
      }
    }
  }

  switch (currentState)
  {
  case STATE_INIT:
    Serial.println("Error: Reached Init state in loop. Restarting.");
    currentState = STATE_WIFI_CONNECTING;
    delay(1000);
    break;

  case STATE_PN532_INIT: // **** NEW FIRST STEP ****
    Serial.println("Initializing PN532...");
    setupPN532(); // This also blocks but includes check
    if (currentState != STATE_ERROR)
    {
      Serial.println("PN532 Initialized.");
      // currentState = STATE_WAITING_FOR_CARD; // Old transition
      currentState = STATE_WIFI_CONNECTING; // NEW transition: Proceed to WiFi
      // lastStatusSendTime = now; // Status timer started later
      // sendStatusUpdate();       // Notify server we are ready (no card initially) // Done later
    }
    // On error, setupPN532 sets currentState = STATE_ERROR
    break;

  case STATE_WIFI_CONNECTING: // **** NEW SECOND STEP ****
    Serial.println("Connecting to WiFi...");
    setupWiFi(); // Blocking call for simplicity here
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("WiFi Connected!");
      // ---> ADD TCP TEST HERE <---
      Serial.println("Performing basic TCP connection test...");
      if (testTcpConnection(serverHostname, serverPort))
      {
        Serial.println("TCP Test SUCCESSFUL.");
      }
      else
      {
        Serial.println("TCP Test FAILED. Check server, port, firewall, and network route.");
        // Optional: Add a delay or different state handling if TCP fails
        // currentState = STATE_ERROR; // Or retry WiFi?
        // return; // Prevent immediate WS attempt if TCP fails?
      }
      // ---> END TCP TEST <---
      currentState = STATE_WEBSOCKET_CONNECTING; // Transition to WS
      lastReconnectAttempt = 0;                  // Allow immediate connection attempt
    }
    else
    {
      Serial.print("WiFi Connection Failed (Status: ");
      Serial.print(wifiStatusToString(WiFi.status()));
      Serial.println("). Retrying in 5s...");
      delay(5000);
    }
    break;

  case STATE_WEBSOCKET_CONNECTING: // **** NEW THIRD STEP ****
    if (!webSocketConnected && (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS))
    {
      Serial.println("Attempting WebSocket connection...");
      setupWebSocket(); // This starts the connection attempt
      lastReconnectAttempt = now;
      // Connection success/failure is handled by the handleWebSocketEvent callback
      // The callback will transition to STATE_WAITING_FOR_CARD on success
    }
    // Add a timeout? If WS fails repeatedly, maybe go to error state?
    break;

  case STATE_WAITING_FOR_CARD: // **** NEW FOURTH STEP ****
  {
    // Send initial status update *once* when entering this state
    if (lastStatusSendTime == 0)
    {                           // Check if it's the first time entering
      lastStatusSendTime = now; // Initialize status timer
      sendStatusUpdate();       // Notify server we are ready
    }

    uint8_t uid[7];
    uint8_t uidLength;
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50); // Short non-blocking poll

    if (success)
    {
      if (nfc.ntag424_isNTAG424()) // Check if it's the right tag type
      {
        String detectedUid = uidToString(uid, uidLength);
        if (detectedUid != currentCardUid) // Only transition if it's a *new* card detection
        {
          currentCardUid = detectedUid;
          Serial.print("NTAG424 Card Detected: ");
          Serial.println(currentCardUid);
          lastCardSeenTime = now;
          sendStatusUpdate(); // Send status immediately on new card detection
          currentState = STATE_CARD_DETECTED;
          // lastPollTime = now; // Removed
        }
        // else: Same card still seen, do nothing here, handled in CARD_DETECTED
      }
      else
      {
        Serial.println("Detected non-NTAG424 card. Ignoring.");
        // Maybe clear currentCardUid if a non-NTAG card is held?
        // if (currentCardUid != "") {
        //   currentCardUid = "";
        //   sendStatusUpdate(); // Notify card removed
        // }
      }
    }
    else if (currentCardUid != "") // Card was present, now it's not detected
    {
      // Use timeout logic similar to CARD_DETECTED state
      if (now - lastCardSeenTime > CARD_REMOVAL_TIMEOUT_MS)
      {
        Serial.print("Card removed (detected in WAITING): ");
        Serial.println(currentCardUid);
        currentCardUid = "";
        sendStatusUpdate(); // Notify server card is gone
        // Stay in WAITING_FOR_CARD
      }
    }

    // Send periodic status update even if no card is present
    if (now - lastStatusSendTime > STATUS_SEND_INTERVAL_MS)
    {
      sendStatusUpdate();
      lastStatusSendTime = now;
    }
  }
  break;

  case STATE_CARD_DETECTED:
  {
    // Check if card is still present
    uint8_t uid[7];
    uint8_t uidLength;
    bool stillPresent = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50); // Quick check

    if (stillPresent && uidToString(uid, uidLength) == currentCardUid)
    {
      lastCardSeenTime = now; // Update last seen time
    }
    else
    {
      // Card might be gone or a different card appeared
      if (now - lastCardSeenTime > CARD_REMOVAL_TIMEOUT_MS)
      {
        Serial.print("Card removed: ");
        Serial.println(currentCardUid);
        currentCardUid = "";
        sendStatusUpdate(); // Notify server card is gone
        currentState = STATE_WAITING_FOR_CARD;
        // lastHeartbeatTime = now; // Removed
        lastStatusSendTime = now; // Reset status timer
        break;                    // Exit CARD_DETECTED state
      }
      // Else: Could be a brief read failure, wait until timeout
    }

    // No command polling needed. Commands arrive via WebSocket event.

    // Send periodic status update while card is present
    if (now - lastStatusSendTime > STATUS_SEND_INTERVAL_MS)
    {
      sendStatusUpdate();
      lastStatusSendTime = now;
    }
  }
  break;

  case STATE_PROCESSING_COMMAND:
    // State is changed within webSocketEvent or executeCommand
    // No periodic actions needed here, wait for command completion or WS message
    delay(10); // Small delay to prevent busy-looping
    break;

  case STATE_ERROR:
    Serial.println("Error state reached. Check logs. Halting actions.");
    // Maybe try to reconnect WiFi/WS after a delay?
    delay(10000);
    // Attempt recovery
    if (!nfc.getFirmwareVersion()) // Check PN532 first? Maybe not ideal as it requires SPI.
    {
      Serial.println("Error Recovery: PN532 not responding. Retrying PN532 init.");
      currentState = STATE_PN532_INIT; // Try re-initializing PN532 first
      // Ensure WS/WiFi flags are reset if PN532 fails later?
      if (webSocket.available())
      {
        webSocket.close();
      }
      webSocketConnected = false;
      WiFi.disconnect(true); // Force WiFi disconnect too? Risky if network is needed for logs.
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Error Recovery: WiFi disconnected. Retrying WiFi connect.");
      currentState = STATE_WIFI_CONNECTING;
      if (webSocket.available())
      {
        webSocket.close();
      }
      webSocketConnected = false;
    }
    else if (!webSocketConnected)
    {
      Serial.println("Error Recovery: WebSocket disconnected. Retrying WS connect.");
      currentState = STATE_WEBSOCKET_CONNECTING;
    }
    else
    {
      // If HW, WiFi and WS seem ok, what else could be wrong? Re-init PN532 as a guess.
      Serial.println("Error Recovery: Unknown error cause. Retrying PN532 init.");
      currentState = STATE_PN532_INIT;
    }
    break;
  }

  // General checks
  if (currentState > STATE_WIFI_CONNECTING && WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi disconnected. Attempting to reconnect...");
    currentState = STATE_WIFI_CONNECTING;
    currentCardUid = ""; // Reset card state
    // Use close() instead of disconnect()
    if (webSocket.available())
    {
      webSocket.close();
    }
    webSocketConnected = false;
  }
  else if (currentState > STATE_WEBSOCKET_CONNECTING && !webSocketConnected)
  {
    // This condition is now less reliable as connection drop might be detected by poll() or events first.
    // The logic inside loop() -> webSocket.poll() section handles setting webSocketConnected = false
    // and transitioning state back to STATE_WEBSOCKET_CONNECTING.
    // Serial.println("WebSocket disconnected (detected in loop check). Attempting to reconnect...");
    // currentState = STATE_WEBSOCKET_CONNECTING;
    // currentCardUid = "";      // Reset card state
    // lastReconnectAttempt = 0; // Allow immediate reconnect attempt
  }

  delay(10); // Small base delay
}

// --- WiFi Functions ---
void setupWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return; // Already connected

  WiFi.disconnect(true); // Disconnect explicitly
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);
  // WiFi.setHostname("NTAG424-Reader"); // Optional: Set hostname

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi ");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) // ~10 seconds timeout
  {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("WiFi connected! IP Address: ");
    Serial.println(WiFi.localIP());
    // Don't change state here, handled in the main loop
  }
  else
  {
    Serial.print("WiFi connection failed. Status: ");
    Serial.println(wifiStatusToString(WiFi.status()));
    // Print available networks for debugging
    Serial.println("Scanning available networks...");
    int n = WiFi.scanNetworks();
    if (n == 0)
    {
      Serial.println("No networks found");
    }
    else
    {
      Serial.print(n);
      Serial.println(" networks found:");
      for (int i = 0; i < n; ++i)
      {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(WiFi.SSID(i));
        Serial.print(" (");
        Serial.print(WiFi.RSSI(i));
        Serial.print(")");
        Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : " *");
        delay(10);
      }
    }
    Serial.println("\nCheck SSID and password.");
    // Stay in WIFI_CONNECTING state (handled by loop)
  }
}

// Kept for potential future use
void startWiFiAP()
{
  Serial.println("Starting WiFi AP...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAP("NTAG424_Reader_AP", "password123");
  delay(300);
  Serial.println("WiFi AP started.");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  // AP mode doesn't usually connect to server, so might need different state logic
  // currentState = STATE_PN532_INIT; // Or a specific AP state
}

// --- WebSocket Functions ---
void setupWebSocket()
{
  // Ensure previous connection is closed if retrying
  if (webSocket.available()) // Check if already connected
  {
    webSocket.close(); // Use close() instead of disconnect()
    webSocketConnected = false;
    delay(100);
  }

  // Register event handlers using lambda functions or regular functions
  webSocket.onMessage(handleWebSocketMessage);
  webSocket.onEvent(handleWebSocketEvent);

  // Connect to the server path for the reader
  Serial.printf("[WSc] Attempting to connect to ws://%s:%d%s\n", serverHostname, serverPort, readerPath);
  bool connected = webSocket.connect(serverHostname, serverPort, readerPath);

  if (connected)
  {
    Serial.println("[WSc] WebSocket connect() call seemingly successful (initial check). Waiting for Open event.");
    // Note: Connection status (webSocketConnected=true) is set in the event handler upon 'Connect' event
  }
  else
  {
    Serial.println("[WSc] WebSocket connect() call failed immediately.");
    // Maybe trigger error state or just let the reconnect logic handle it
    webSocketConnected = false; // Ensure flag is false
  }
  // Connection status is primarily handled by handleWebSocketEvent
}

// --- NEW WebSocket Event Handlers (ArduinoWebsockets) ---

// Handles incoming WebSocket messages
void handleWebSocketMessage(WebsocketsMessage message)
{
  Serial.print("[WSc] Received message: ");
  Serial.println(message.data());

  // Use ArduinoJson v6 for ESP32 (Reverted for now)
  DynamicJsonDocument doc(1024); // Adjust size as needed for commands
  DeserializationError error = deserializeJson(doc, message.data());

  if (error)
  {
    Serial.print(F("[WSc] deserializeJson() failed: "));
    Serial.println(error.c_str());
    sendLogOverWebSocket("Error: Failed to parse incoming JSON: " + String(error.c_str()));
    return;
  }

  const char *msgType = doc["type"];
  if (msgType && strcmp(msgType, "command") == 0)
  {
    JsonObject cmdPayload = doc["payload"].as<JsonObject>();
    if (cmdPayload)
    {
      String command = cmdPayload["command"].as<String>();
      Serial.print("[WSc] Received command from server: ");
      Serial.println(command);
      currentState = STATE_PROCESSING_COMMAND;
      clearLogs(); // Clear logs for the new command
      executeCommand(cmdPayload);
      // executeCommand will change state back when done or failed
    }
    else
    {
      Serial.println("[WSc] Received 'command' type message but payload is invalid.");
      sendLogOverWebSocket("Error: Invalid command payload received.");
    }
  }
  else if (msgType)
  {
    Serial.printf("[WSc] Received unhandled message type: %s\n", msgType);
  }
  else
  {
    Serial.println("[WSc] Received message without a 'type' field.");
    sendLogOverWebSocket("Error: Received message without type field.");
  }
}

// Handles WebSocket connection events (connect, disconnect, errors)
void handleWebSocketEvent(WebsocketsEvent event, String data)
{
  switch (event)
  {
  case WebsocketsEvent::ConnectionOpened:
    Serial.println("[WSc] Event: Connection Opened");
    webSocketConnected = true;
    // Only transition state if we were trying to connect
    if (currentState == STATE_WEBSOCKET_CONNECTING)
    {
      // currentState = STATE_PN532_INIT; // Old transition
      currentState = STATE_WAITING_FOR_CARD; // NEW transition: Proceed to wait for card
    }
    // Send initial status update upon connection (also done when entering WAITING_FOR_CARD state)
    // sendStatusUpdate(); // Maybe remove this one if handled reliably by state entry? Keep for redundancy for now.
    break;
  case WebsocketsEvent::ConnectionClosed:
    Serial.println("[WSc] Event: Connection Closed");
    webSocketConnected = false;
    // If we are in a state expecting a connection, go back to reconnecting
    if (currentState != STATE_WIFI_CONNECTING && currentState != STATE_WEBSOCKET_CONNECTING)
    {
      Serial.println("[WSc] Connection closed unexpectedly. Re-entering WS Connect state.");
      currentState = STATE_WEBSOCKET_CONNECTING;
      lastReconnectAttempt = 0; // Allow immediate reconnect attempt
      currentCardUid = "";      // Reset card state
    }
    // Attempt recovery
    if (!nfc.getFirmwareVersion()) // Check PN532 first? Maybe not ideal as it requires SPI.
    {
      Serial.println("Error Recovery: PN532 not responding. Retrying PN532 init.");
      currentState = STATE_PN532_INIT; // Try re-initializing PN532 first
      // Ensure WS/WiFi flags are reset if PN532 fails later?
      if (webSocket.available())
      {
        webSocket.close();
      }
      webSocketConnected = false;
      WiFi.disconnect(true); // Force WiFi disconnect too? Risky if network is needed for logs.
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Error Recovery: WiFi disconnected. Retrying WiFi connect.");
      currentState = STATE_WIFI_CONNECTING;
      if (webSocket.available())
      {
        webSocket.close();
      }
      webSocketConnected = false;
    }
    else if (!webSocketConnected)
    {
      Serial.println("Error Recovery: WebSocket disconnected. Retrying WS connect.");
      currentState = STATE_WEBSOCKET_CONNECTING;
    }
    else
    {
      // If HW, WiFi and WS seem ok, what else could be wrong? Re-init PN532 as a guess.
      Serial.println("Error Recovery: Unknown error cause. Retrying PN532 init.");
      currentState = STATE_PN532_INIT;
    }
    break;
  case WebsocketsEvent::GotPing:
    Serial.println("[WSc] Event: Got Ping");
    // Library handles Pong automatically
    break;
  case WebsocketsEvent::GotPong:
    Serial.println("[WSc] Event: Got Pong");
    break;
  default:
    Serial.printf("[WSc] Event: Unknown (%d)\n", static_cast<int>(event));
    break;
  }
}

void sendWebSocketMessage(const JsonDocument &doc)
{
  if (!webSocketConnected || !webSocket.available()) // Check connection status
  {
    Serial.println("Cannot send WebSocket message: Not connected or available.");
    return;
  }
  String jsonString;
  serializeJson(doc, jsonString);
  // Serial.print("Sending WS message: "); Serial.println(jsonString); // Debug
  if (!webSocket.send(jsonString))
  { // Use send() instead of sendTXT()
    Serial.println("[WSc] webSocket.send() failed.");
    // Handle send failure? Maybe mark as disconnected?
  }
}

// Specific function to send logs easily
void sendLogOverWebSocket(const String &logMessage)
{
  DynamicJsonDocument logDoc(256); // Reverted ArduinoJson change
  logDoc["type"] = "log";
  logDoc["payload"] = logMessage;
  sendWebSocketMessage(logDoc);
}

// --- PN532 Functions ---
void setupPN532()
{
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata)
  {
    Serial.print("Error: Didn't find PN53x board. Check wiring.");
    addLog("Error: Didn't find PN53x board. Check wiring."); // Send log via WS if connected
    // --> Error state is now set *only* here for PN532 init failure <--
    currentState = STATE_ERROR; // Set error state
    return;
  }

  Serial.print("Found PN53x board version: ");
  Serial.print((versiondata >> 24) & 0xFF, HEX);
  Serial.print('.');
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  nfc.SAMConfig(); // Configure the board for ISO14443A
}

// --- Helper Functions ---
void printHex(uint8_t *data, uint8_t len)
{
  for (int i = 0; i < len; i++)
  {
    if (data[i] < 0x10)
      Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
}

String uidToString(uint8_t *uid, uint8_t len)
{
  String uidStr = "";
  for (uint8_t i = 0; i < len; i++)
  {
    if (uid[i] < 0x10)
      uidStr += "0";
    uidStr += String(uid[i], HEX);
  }
  uidStr.toUpperCase();
  return uidStr;
}

void clearLogs()
{
  logCount = 0;
}

// Add log locally and send over WebSocket
void addLog(const String &logMessage)
{
  Serial.println(logMessage); // Print to Serial for local debug
  if (logCount < MAX_LOG_LINES)
  {
    commandLogs[logCount++] = logMessage;
  }
  // Send important logs (or all logs?) over WebSocket immediately
  // Be mindful of flooding the connection if logs are very frequent.
  sendLogOverWebSocket(logMessage);
}

// --- Communication Functions (WebSocket Based) ---

// Sends the current status (card present/absent and UID) via WebSocket
void sendStatusUpdate()
{
  if (!webSocketConnected)
    return;

  // Use ArduinoJson v6 (Reverted)
  DynamicJsonDocument statusDoc(JSON_OBJECT_SIZE(2) + JSON_STRING_SIZE(currentCardUid.length()) + 50); // Type + Payload + UID
  statusDoc["type"] = "status";
  JsonObject payload = statusDoc.createNestedObject("payload"); // Keep deprecated
  if (currentCardUid != "")
  {
    payload["uid"] = currentCardUid;
  }
  else
  {
    payload["uid"] = nullptr; // Send null if no card
  }
  // payload["readerId"] = "READER_01"; // Optional

  sendWebSocketMessage(statusDoc);
  lastStatusSendTime = millis(); // Update time after sending
}

// Sends the result of a command execution back to the server via WebSocket
void sendCommandResultWebSocket(const String &uid, bool success, const String &message)
{
  if (!webSocketConnected)
  {
    Serial.println("Cannot send command result: WebSocket not connected.");
    // Store result to send later? For now, just drop it.
    currentState = STATE_WAITING_FOR_CARD; // Still go back to waiting state locally
    return;
  }

  // Use ArduinoJson v6 (Reverted)
  // Estimate size: type, payload object, uid, success, message, logs array + strings
  DynamicJsonDocument resultDoc(JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(logCount) + (logCount * 100) + 200);

  resultDoc["type"] = "command_result";
  JsonObject payload = resultDoc.createNestedObject("payload"); // Keep deprecated
  payload["uid"] = uid;
  payload["success"] = success;
  payload["message"] = message;

  JsonArray logArray = payload.createNestedArray("logs"); // Keep deprecated
  for (int i = 0; i < logCount; i++)
  {
    logArray.add(commandLogs[i]);
  }

  sendWebSocketMessage(resultDoc);

  // Command finished, go back to waiting state
  currentState = STATE_WAITING_FOR_CARD;
  lastStatusSendTime = millis(); // Reset status timer
}

/* --- Removed HTTP Functions ---
void sendStatusToServer(bool cardPresent, const String &uid) { ... }
void pollForCommand(const String &uid) { ... }
void sendCommandResult(const String &uid, bool success, const String &message) { ... }
*/

// --- Command Execution ---
// executeCommand is called from webSocketEvent when a command message arrives
void executeCommand(const JsonObject &cmdPayload) // Changed param to use payload directly
{
  String command = cmdPayload["command"].as<String>();
  String uid = cmdPayload["uid"].as<String>(); // UID should match currentCardUid, maybe check?

  if (uid != currentCardUid && currentCardUid != "")
  {
    addLog("Error: Received command for UID " + uid + " but current card is " + currentCardUid);
    sendCommandResultWebSocket(uid, false, "Command UID mismatch");
    return;
  }
  if (currentCardUid == "")
  {
    addLog("Error: Received command but no card is present.");
    sendCommandResultWebSocket(uid, false, "Command received but no card present");
    return;
  }

  addLog("Executing command: " + command);

  if (command == "enroll")
  {
    handleEnrollCommand(cmdPayload); // Pass the whole payload
  }
  else if (command == "authenticate")
  {
    handleAuthenticateCommand(cmdPayload); // Pass the whole payload
  }
  else
  {
    addLog("Error: Unknown command received: " + command);
    sendCommandResultWebSocket(uid, false, "Unknown command");
    // State change back to WAITING is handled by sendCommandResultWebSocket
  }
  // State change handled within specific command handlers or sendCommandResultWebSocket
}

// Helper to parse key from JSON array (byte array) to uint8_t array
// Note: Server now sends byte arrays directly in JSON
// Update signature to accept JsonArrayConst
bool parseKeyFromJsonBytes(const JsonArrayConst &jsonKeyBytes, uint8_t *keyBuffer, size_t keySize)
{
  if (!jsonKeyBytes || jsonKeyBytes.size() != keySize)
  {
    addLog("Error: Invalid key byte array size in JSON (Expected " + String(keySize) + ", Got " + String(jsonKeyBytes.size()) + ").");
    return false;
  }
  for (size_t i = 0; i < keySize; ++i)
  {
    if (!jsonKeyBytes[i].is<uint8_t>()) // Check if element is a number (byte)
    {
      addLog("Error: Non-byte value found in key byte array at index " + String(i) + ".");
      // Add more debug: print the element type if possible
      // addLog("Element type: " + String(jsonKeyBytes[i].type()));
      return false;
    }
    keyBuffer[i] = jsonKeyBytes[i].as<uint8_t>();
  }
  // Debug: Print parsed key
  // Serial.print("Parsed key bytes: "); printHex(keyBuffer, keySize); Serial.println();
  return true;
}

void handleEnrollCommand(const JsonObject &cmdPayload)
{
  String uid = cmdPayload["uid"].as<String>();
  // Use const version for reading
  JsonObjectConst keysJson = cmdPayload["keys"].as<JsonObjectConst>();

  if (!keysJson)
  {
    addLog("Error: Enroll command missing or invalid 'keys' object in payload.");
    sendCommandResultWebSocket(uid, false, "Enroll command missing/invalid keys object");
    return;
  }

  // Buffers for keys (16 bytes each)
  uint8_t defaultKey[16], masterKey[16], authKey[16], readKey[16], writeKey[16], changeKey[16];

  // Parse keys from JSON (expecting byte arrays)
  // Reverted the extra checks for simplicity now
  if (!parseKeyFromJsonBytes(keysJson["defaultKey"].as<JsonArrayConst>(), defaultKey, 16) ||
      !parseKeyFromJsonBytes(keysJson["masterKey"].as<JsonArrayConst>(), masterKey, 16) ||
      !parseKeyFromJsonBytes(keysJson["authKey"].as<JsonArrayConst>(), authKey, 16) ||
      !parseKeyFromJsonBytes(keysJson["readKey"].as<JsonArrayConst>(), readKey, 16) ||
      !parseKeyFromJsonBytes(keysJson["writeKey"].as<JsonArrayConst>(), writeKey, 16) ||
      !parseKeyFromJsonBytes(keysJson["changeKey"].as<JsonArrayConst>(), changeKey, 16))
  {
    // parseKeyFromJsonBytes adds logs
    sendCommandResultWebSocket(uid, false, "Failed to parse one or more keys from JSON payload");
    return;
  }

  addLog("Keys parsed successfully.");

  bool initialAuthSuccess = false;
  bool needToChangeKey0 = false;

  // --- Attempt Authentication ---
  addLog("Attempting authentication with default key (Key 0)...");
  if (nfc.ntag424_Authenticate(defaultKey, 0, AUTH_CMD))
  {
    addLog("Authentication with default key successful (blank card).");
    initialAuthSuccess = true;
    needToChangeKey0 = true; // Need to change Key 0 from default to master
  }
  else
  {
    addLog("Auth with default key failed. Trying master key (Key 0)...");
    if (nfc.ntag424_Authenticate(masterKey, 0, AUTH_CMD))
    {
      addLog("Authentication with master key successful (partially enrolled card).");
      initialAuthSuccess = true;
      needToChangeKey0 = false; // Key 0 is already the master key
    }
    else
    {
      addLog("Authentication failed with both default and master keys.");
      sendCommandResultWebSocket(uid, false, "Auth failed with both default and master keys");
      return;
    }
  }

  if (needToChangeKey0)
  {
    addLog("Changing Key 0 (Master Key)...");
    if (nfc.ntag424_ChangeKey(defaultKey, masterKey, 0))
    {
      addLog("Key 0 changed successfully.");
    }
    else
    {
      addLog("Error: Failed to change Key 0 to Master Key.");
      sendCommandResultWebSocket(uid, false, "Failed to change Key 0 to Master Key");
      return;
    }
  }
  else
  {
    addLog("Skipping Key 0 change (already authenticated with Master Key).");
    // We are already authenticated with the master key (Key 0).
  }

  // --- Change Keys 1-4 ---
  // Now, change Keys 1-4. Authentication *must* be with the Master Key (Key 0) which we confirmed above.
  // The 'oldKey' parameter for ntag424_ChangeKey when changing keys 1-4 *should* be the
  // key currently stored in that slot. We'll try the default key first, and if that fails,
  // we'll try overwriting the target key with itself (to handle already-programmed cards).
  uint8_t *targetKeys[] = {authKey, readKey, writeKey, changeKey}; // Array of pointers to the new keys

  // Note: changeSuccess was initialized before the Key 0 block and might be false if Key 0 change failed.
  // We continue attempting app key changes but the final result will reflect the overall success.
  bool anyAppKeyFailed = false; // Track failures specifically for app keys

  for (uint8_t keyNo = 1; keyNo <= 4; keyNo++)
  {
    uint8_t *newKey = targetKeys[keyNo - 1];
    uint8_t *currentDefaultKey = defaultKey; // The key we assume might be there initially
    bool keyChangeSuccess = false;

    addLog("Setting Key " + String(keyNo) + "... AUTH: MasterKey");

    // Attempt 1: Change from defaultKey (like a blank card)
    addLog("  Attempt 1: Change from Default -> New");
    if (nfc.ntag424_ChangeKey(currentDefaultKey /* Old Key */, newKey /* New Key */, keyNo /* Key Number */))
    {
      addLog("  -> OK (Set from default)");
      keyChangeSuccess = true;
    }
    else
    {
      addLog("  -> Failed (Maybe not default?)");
      // Attempt 2: If changing from default failed, try changing from the NEW key to the NEW key.
      // This handles cases where the key is already set to the target value.
      // Requires appropriate permissions (which we should have via Master Key auth).
      addLog("  Attempt 2: Change from New -> New (Overwrite self)");
      if (nfc.ntag424_ChangeKey(newKey /* Old Key (Target) */, newKey /* New Key */, keyNo /* Key Number */))
      {
        addLog("  -> OK (Already set / Overwrote self)");
        keyChangeSuccess = true;
      }
      else
      {
        addLog("  -> Failed!");
        // If both attempts failed, we cannot set the key with current permissions/state.
      }
    }

    if (!keyChangeSuccess)
    {
      addLog("  ERROR: Failed to set Key " + String(keyNo) + ". It might have an unexpected value or be configuration-locked.");
      anyAppKeyFailed = true; // Mark that at least one app key failed
                              // Continue trying other keys? Or abort? For now, continue but report failure at the end.
    }
  }

  // --- Final Result ---
  // Final success depends on BOTH Key 0 change (if attempted) AND all App Key changes being successful.
  if (initialAuthSuccess && !anyAppKeyFailed)
  {
    addLog("All necessary keys changed successfully!");
    sendCommandResultWebSocket(uid, true, "Enrollment successful");
  }
  else
  {
    addLog("Enrollment completed partially. Some keys may not have been changed correctly.");
    // Provide a more specific message if possible
    String finalMessage = "Enrollment failed partially";
    if (!initialAuthSuccess)
    { // Check if Key 0 change itself failed
      finalMessage += " (Master Key issue)";
    }
    else if (anyAppKeyFailed)
    {
      finalMessage += " (Application Key issue)";
    }
    sendCommandResultWebSocket(uid, false, finalMessage);
  }
}

void handleAuthenticateCommand(const JsonObject &cmdPayload)
{
  String uid = cmdPayload["uid"].as<String>();

  // Reverted extra checks
  int keyNoInt = cmdPayload["keyNo"].as<int>();

  if (keyNoInt < 0 || keyNoInt > 4)
  { // Validate key number range (0-4)
    addLog("Error: Authenticate command received invalid 'keyNo': " + String(keyNoInt));
    sendCommandResultWebSocket(uid, false, "Authenticate command invalid key number");
    return;
  }
  uint8_t keyNo = (uint8_t)keyNoInt;

  // Use const version for reading
  JsonArrayConst keyJson = cmdPayload["authKey"].as<JsonArrayConst>();

  if (!keyJson || keyNo == 0) // Ensure keyNo is valid (1-4 typically for app keys, though 0 is Master)
  {
    addLog("Error: Authenticate command missing 'authKey' or invalid 'keyNo' (Key 0 needs special handling if intended).");
    sendCommandResultWebSocket(uid, false, "Authenticate command missing key or invalid key number");
    return;
  }

  uint8_t authKey[16];
  if (!parseKeyFromJsonBytes(keyJson, authKey, 16))
  {
    addLog("Error: Failed to parse auth key byte array from JSON.");
    sendCommandResultWebSocket(uid, false, "Failed to parse auth key from payload");
    return;
  }

  addLog("Attempting authentication with received key #" + String(keyNo));
  // Serial.print("Using Key: "); printHex(authKey, 16); Serial.println(); // Debug

  bool authSuccess = false;
  const int MAX_AUTH_ATTEMPTS = 3;
  const unsigned long AUTH_RETRY_DELAY_MS = 100; // Small delay between retries

  for (int attempt = 1; attempt <= MAX_AUTH_ATTEMPTS; ++attempt)
  {
    addLog("Authentication attempt " + String(attempt) + "/" + String(MAX_AUTH_ATTEMPTS) + "...");
    if (nfc.ntag424_Authenticate(authKey, keyNo, AUTH_CMD))
    {
      authSuccess = true;
      addLog("Authentication SUCCESS!");
      break; // Exit loop on success
    }
    else
    {
      addLog("Authentication FAILED on attempt " + String(attempt));
      if (attempt < MAX_AUTH_ATTEMPTS)
      {
        delay(AUTH_RETRY_DELAY_MS); // Wait before retrying
      }
    }
  }

  // Send final result after all attempts
  if (authSuccess)
  {
    sendCommandResultWebSocket(uid, true, "Card authenticated successfully with Key #" + String(keyNo));
  }
  else
  {
    sendCommandResultWebSocket(uid, false, "Authentication failed after " + String(MAX_AUTH_ATTEMPTS) + " attempts with Key #" + String(keyNo));
  }
}

// Added function to translate WiFi status codes
const char *wifiStatusToString(wl_status_t status)
{
  switch (status)
  {
  case WL_NO_SHIELD:
    return "WL_NO_SHIELD";
  case WL_IDLE_STATUS:
    return "WL_IDLE_STATUS";
  case WL_NO_SSID_AVAIL:
    return "WL_NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "WL_SCAN_COMPLETED";
  case WL_CONNECTED:
    return "WL_CONNECTED";
  case WL_CONNECT_FAILED:
    return "WL_CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "WL_CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "WL_DISCONNECTED";
    // #ifdef ESP32 // ESP32 specific statuses if needed
    //   case WL_ERR_WIFI_INIT: return "WL_ERR_WIFI_INIT";
    // #endif
  default:
    static char unknown[20];
    sprintf(unknown, "UNKNOWN (%d)", status);
    return unknown;
  }
}

// Added: Function to test basic TCP connection
bool testTcpConnection(const char *host, uint16_t port)
{
  WiFiClient client;
  Serial.printf("Attempting TCP connection to %s:%d...\\n", host, port);
  if (!client.connect(host, port))
  {
    Serial.println("TCP connection failed.");
    // Note: WiFiClient doesn't provide detailed error codes easily here.
    // Common reasons: Host down, port closed, firewall block, network route issue.
    return false;
  }
  Serial.println("TCP connection successful. Closing connection.");
  client.stop();
  return true;
}
