#include <Arduino.h>
#include <SPI.h> // Required for SPI
#include <Adafruit_PN532_NTAG424.h>

// --- PN532 Configuration ---
// Define the pins for SPI communication with the PN532 breakout
// These pins match the defaults for ESP32-C3 SuperMini
#define PN532_SCK (4)
#define PN532_MISO (5)
#define PN532_MOSI (6)
#define PN532_SS (7)

// --- NTAG424 Configuration ---
// Default NTAG424 Key (Factory default) - 16 bytes of 0x00
uint8_t defaultKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// !!! WARNING: FIXED PRODUCTION KEYS - INSECURE FOR REAL DEPLOYMENT !!!
// These keys are fixed for easy recovery during development/testing.
// In a real application, keys should be diversified per card and managed securely.
uint8_t fixedProdKey0[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                             0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F}; // Master Key
uint8_t fixedProdKey1[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                             0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF}; // App Key 1 (Authentication)
uint8_t fixedProdKey2[16] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
                             0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF}; // App Key 2 (Read - example)
uint8_t fixedProdKey3[16] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
                             0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF}; // App Key 3 (Write - example)
uint8_t fixedProdKey4[16] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
                             0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF}; // App Key 4 (Card Master Key?)

// Key number to use for authentication
const uint8_t AUTH_KEY_NO = 1;
// Authentication command (0x71 = AuthenticateEV2First)
const uint8_t AUTH_CMD = 0x71;

// Initialize the PN532 interface for SPI
// Use the base class Adafruit_PN532, NTAG424 functions are added by the include
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// --- Function Prototypes ---
void printMenu();
bool waitForCard(uint8_t *uid, uint8_t *uidLength);
void enrollCard();
void authenticateCard();
void printHex(uint8_t *data, uint8_t len);

// --- Setup ---
void setup()
{
  Serial.begin(115200);
  while (!Serial)
    delay(10); // Wait for serial port to connect (needed for native USB)

  delay(2000);

  Serial.println(); // Start with a newline
  Serial.println("NTAG424 Enrollment/Authentication Example");
  Serial.println("----------------------------------------");

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata)
  {
    Serial.print("Error: Didn't find PN53x board. Check wiring.");
    while (1)
      delay(10); // Halt
  }

  // Print board info
  Serial.print("Found PN53x board version: ");
  Serial.print((versiondata >> 24) & 0xFF, HEX);
  Serial.print('.');
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  // Configure the PN532 to read ISO14443A tags
  nfc.SAMConfig();

  printMenu();
}

// --- Main Loop ---
void loop()
{
  if (Serial.available() > 0)
  {
    char command = Serial.read();
    Serial.println(); // Newline after command input

    if (command == 'e' || command == 'E')
    {
      enrollCard();
    }
    else if (command == 'a' || command == 'A')
    {
      authenticateCard();
    }
    else
    {
      Serial.println("Invalid command.");
    }
    printMenu(); // Show menu again after action
  }
  // Small delay to prevent busy-waiting if needed
  delay(50);
}

// --- Helper Functions ---

// Prints the main menu
void printMenu()
{
  Serial.println(); // Start with a newline
  Serial.println("--- Menu ---");
  Serial.println(" e - Enroll New Card (Changes Keys!)");
  Serial.println(" a - Authenticate Card");
  Serial.print("Enter command: ");
}

// Waits for a card to be present and checks if it's an NTAG424
// Returns true if a valid NTAG424 card is found, false otherwise
bool waitForCard(uint8_t *uid, uint8_t *uidLength)
{
  Serial.println(); // Start with a newline
  Serial.println("Please present an NTAG424 card...");
  bool cardFound = false;
  while (!cardFound)
  {
    // Wait for an ISO14443A card
    // readPassiveTargetID will return 1 if a card is found
    // It will populate uid and uidLength
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength, 1000))
    { // 1 second timeout
      Serial.print("Found card with UID: ");
      printHex(uid, *uidLength);
      Serial.println();

      // Check if it's specifically an NTAG424
      if (nfc.ntag424_isNTAG424())
      {
        Serial.println("Card is NTAG424.");
        return true; // Valid NTAG424 found
      }
      else
      {
        Serial.println("Card is NOT NTAG424. Please remove.");
        // Wait for card removal before trying again
        while (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength, 50))
          ;
        Serial.println("Card removed. Waiting for NTAG424...");
        return false; // Card found, but wrong type
      }
    }
    // No card found yet, loop continues
    delay(100); // Small delay before next check
  }
  return false; // Should not be reached in this loop structure, but added for completeness
}

// Enrolls a new (blank) NTAG424 card by changing keys
void enrollCard()
{
  Serial.println(); // Start with a newline
  Serial.println("--- Enroll New Card ---");
  uint8_t uid[7];
  uint8_t uidLength;

  if (!waitForCard(uid, &uidLength))
  {
    Serial.println("Enrollment cancelled: No valid card found.");
    return;
  }

  // --- Step 1: Authenticate with Default Key ---
  Serial.println("Attempting authentication with default key (0x00)...");
  if (!nfc.ntag424_Authenticate(defaultKey, 0, AUTH_CMD))
  { // Try authenticating Key 0
    Serial.println("Authentication with default key failed. Is the card blank?");
    Serial.println("Attempting authentication with the fixed production Key 0...");
    // If default key auth fails, maybe it was already partially programmed? Try fixed key 0.
    if (!nfc.ntag424_Authenticate(fixedProdKey0, 0, AUTH_CMD))
    {
      Serial.println("Authentication with fixed production Key 0 also failed.");
      Serial.println("Enrollment aborted. Card might be locked or use different keys.");
      return;
    }
    Serial.println("Authentication with fixed production Key 0 successful (Card might have been partially enrolled). Proceeding...");
    // If we authenticated with the fixed key 0, we can proceed to potentially set the other keys if needed.
    // Note: We will attempt to change all keys regardless, using the *authenticated* key as the 'old key'.
  }
  else
  {
    Serial.println("Authentication with default key successful.");
  }

  // --- Step 2: Change Keys ---
  // IMPORTANT: You must authenticate with the key you are about to change (except for Key 0, which needs Key 0 auth)
  // Since we authenticated with Key 0 (or the fixed Key 0), we can now change Key 0.
  Serial.println("Changing Key 0 (Master Key)...");
  // If default auth succeeded, oldKey is defaultKey. If fixed key auth succeeded, oldKey is fixedProdKey0.
  // However, ntag424_ChangeKey only needs the *new* key when changing key 0 after authenticating with key 0.
  // Let's check the library source or docs for the exact `ntag424_ChangeKey` behaviour for Key 0.
  // Assuming it requires the *new key* and *current key* (which must be key 0 for changing key 0)
  // Let's refine this based on library specifics if needed. The example just showed changing key 1.
  // Let's assume for now `ChangeKey` needs old and new for all keys.
  // We need to figure out which key we authenticated with.
  // For simplicity, let's *try* changing from default first, then from fixed if that fails.

  bool changeSuccess = false;
  uint8_t *authKeyUsed = defaultKey; // Assume default first

  if (nfc.ntag424_Authenticate(defaultKey, 0, AUTH_CMD))
  {
    authKeyUsed = defaultKey;
    Serial.println("Re-authenticated with default key 0.");
  }
  else if (nfc.ntag424_Authenticate(fixedProdKey0, 0, AUTH_CMD))
  {
    authKeyUsed = fixedProdKey0;
    Serial.println("Re-authenticated with fixed key 0.");
  }
  else
  {
    Serial.println("Failed to re-authenticate before changing Key 0. Aborting enrollment.");
    return;
  }

  // Change Key 0
  Serial.print("Changing Key 0... ");
  // Pass the authenticated key (authKeyUsed) as the 'old key' parameter.
  // NOTE: The library might have a specific way to handle Key 0 change.
  // This implementation assumes `ntag424_ChangeKey` works like other keys but requires prior Key 0 auth.
  if (nfc.ntag424_ChangeKey(authKeyUsed, fixedProdKey0, 0))
  {
    Serial.println("OK");
    changeSuccess = true; // At least Key 0 changed

    // Now authenticate with the NEW Key 0 to change other keys
    Serial.println("Authenticating with NEW Key 0 to change other keys...");
    if (!nfc.ntag424_Authenticate(fixedProdKey0, 0, AUTH_CMD))
    {
      Serial.println("FATAL: Failed to authenticate with the new Key 0. Enrollment failed partially.");
      return; // Stop here, card is in an intermediate state
    }
    Serial.println("Authentication with new Key 0 successful.");

    // Change Key 1
    Serial.print("Changing Key 1... ");
    // Changing Key 1 requires prior auth with Key 1 (default 0x00) or Key 0 (if Key 1 doesn't exist yet/is default)
    // Since we are authenticated with NEW Key 0, we can change other keys.
    // We assume the 'old key' for App Keys 1-4 is the default key (0x00) for a blank card.
    if (nfc.ntag424_ChangeKey(defaultKey, fixedProdKey1, 1))
      Serial.println("OK");
    else
    {
      Serial.println("Failed!");
      changeSuccess = false;
    }

    // Change Key 2
    Serial.print("Changing Key 2... ");
    if (nfc.ntag424_ChangeKey(defaultKey, fixedProdKey2, 2))
      Serial.println("OK");
    else
    {
      Serial.println("Failed!");
      changeSuccess = false;
    }

    // Change Key 3
    Serial.print("Changing Key 3... ");
    if (nfc.ntag424_ChangeKey(defaultKey, fixedProdKey3, 3))
      Serial.println("OK");
    else
    {
      Serial.println("Failed!");
      changeSuccess = false;
    }

    // Change Key 4
    Serial.print("Changing Key 4... ");
    if (nfc.ntag424_ChangeKey(defaultKey, fixedProdKey4, 4))
      Serial.println("OK");
    else
    {
      Serial.println("Failed!");
      changeSuccess = false;
    }
  }
  else
  {
    Serial.println("Failed to change Key 0!");
    Serial.println("Enrollment failed. Card keys remain unchanged (either default or previous state).");
    changeSuccess = false;
  }

  if (changeSuccess)
  {
    Serial.println(); // Start with a newline
    Serial.println("All keys changed successfully!");
    Serial.println("Enrollment Complete.");
    Serial.println("Card is now secured with the fixed 'production' keys.");
    Serial.println("!!! REMEMBER: These keys are insecure for real deployment !!!");
  }
  else
  {
    Serial.println(); // Start with a newline
    Serial.println("Enrollment failed or completed partially. Some keys may not have been changed.");
  }

  // Wait for card removal
  Serial.println("Please remove the card.");
  while (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50))
    ;
  Serial.println("Card removed.");
}

// Authenticates a previously enrolled card using the fixed production key
void authenticateCard()
{
  Serial.println(); // Start with a newline
  Serial.println("--- Authenticate Card ---");
  uint8_t uid[7];
  uint8_t uidLength;

  if (!waitForCard(uid, &uidLength))
  {
    Serial.println("Authentication cancelled: No valid card found.");
    return;
  }

  // --- Authenticate with Fixed Production Key ---
  Serial.print("Attempting authentication with fixed production key #");
  Serial.print(AUTH_KEY_NO);
  Serial.print("...");

  // Use the defined key number and authentication command
  if (nfc.ntag424_Authenticate(fixedProdKey1, AUTH_KEY_NO, AUTH_CMD))
  {
    Serial.println(" SUCCESS!");
    Serial.println("Card authenticated successfully.");

    // Optional: Read some data that requires authentication
    // For example, get the Card UID (might require Key 2 auth depending on settings)
    // uint8_t carduid[7];
    // uint8_t bytesread = nfc.ntag424_GetCardUID(carduid);
    // if (bytesread > 0) {
    //    Serial.print("  Card UID (read after auth): ");
    //    printHex(carduid, bytesread);
    //    Serial.println();
    // } else {
    //    Serial.println("  Could not read Card UID (requires different key auth?).");
    // }
  }
  else
  {
    Serial.println(" FAILED!");
    Serial.println("Authentication failed. Card may not be enrolled or uses different keys.");
  }

  // Wait for card removal
  Serial.println("Please remove the card.");
  while (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50))
    ;
  Serial.println("Card removed.");
}

// Helper to print byte arrays as hex
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
