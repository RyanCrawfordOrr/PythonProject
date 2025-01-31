#include <WiFiS3.h>      // Manages Wi-Fi operations on the UNO R4 WiFi
#include <EEPROM.h>      // Provides read/write access to non-volatile memory
#include <ArduinoJson.h> // Used for sending/storing JSON objects
#include <ctype.h>       // For isxdigit, used in URL decoding

/* -------------------------------------------------------------------------
   EEPROM Offsets and Sizes
   --------------------------------------------------------------------------
   We reserve 96 bytes for EEPROM usage in this project.

   - SSID_ADDR:  Where we store the SSID string (up to 31 chars + null)
   - PASS_ADDR:  Where we store the Wi-Fi Password (up to 63 chars + null)
   - VALID_FLAG_ADDR: Single byte indicating 1=valid, 0=invalid credentials
   - FW_VERSION_ADDR: Single byte storing firmware version, used to detect
                     changes that require an EEPROM wipe.
*/
#define EEPROM_SIZE          96
#define SSID_ADDR            0
#define PASS_ADDR            32
#define VALID_FLAG_ADDR      64
#define FW_VERSION_ADDR      65

/* -------------------------------------------------------------------------
   Firmware Version
   --------------------------------------------------------------------------
   If CURRENT_FW_VERSION differs from what we read in the EEPROM,
   we wipe the EEPROM to ensure a fresh start (prevents weird mismatches).
*/
const uint8_t CURRENT_FW_VERSION = 14;

/* -------------------------------------------------------------------------
   AP Configuration
   --------------------------------------------------------------------------
   MASTER_SSID / MASTER_PASSWORD:            Default "unconfigured" AP credentials
   CONFIGURED_MASTER_SSID / ..._PASSWORD:    AP credentials after Master has
                                             router credentials saved
*/
#define MASTER_SSID               "ArduinoMasterAP"
#define MASTER_PASSWORD           "masterConfig123"
#define CONFIGURED_MASTER_SSID    "ArduinoMasterAP_Configured"
#define CONFIGURED_MASTER_PASSWORD "masterConfig123"

/* -------------------------------------------------------------------------
   Global Credentials / State
   --------------------------------------------------------------------------
   storedSSID / storedPass:  Strings retrieved from EEPROM. If credentialsValid
                             is set, these should be correct router credentials.
*/
char storedSSID[32] = {0};
char storedPass[64] = {0};
bool credentialsValid = false;  // Set to true once we confirm valid SSID/PASS

// We create a server on port 80 for Master device to host a configuration page
WiFiServer server(80);

/* -------------------------------------------------------------------------
   Device Roles
   --------------------------------------------------------------------------
   ROLE_MASTER:   Device forms an AP (either unconfigured or configured) to
                  serve the config page or let slaves retrieve credentials.
   ROLE_SLAVE:    Device tries to connect to the Master AP to retrieve
                  credentials, or if it already has them, it connects
                  directly to the router.
   ROLE_STANDBY:  Device does not become Master but doesn't see a
                  Configured Master either. It keeps scanning for a Master.
*/
enum DeviceRole {
  ROLE_MASTER,
  ROLE_SLAVE,
  ROLE_STANDBY
};

DeviceRole currentRole = ROLE_MASTER; // Default to Master until we do a scan

/* -------------------------------------------------------------------------
   Master-Related Tracking
   --------------------------------------------------------------------------
   - masterAPConfigured:    Once new router creds are saved, we start a new AP
                            "CONFIGURED_MASTER_SSID" for Slaves to grab them.
   - masterConfiguredAPDuration:   How long the Master stays in that AP mode
                                   before attempting to connect to the router.
   - masterReconnectPending: Tells us we plan to kill the AP once the time ends.
   - masterReconnectTime:   The millis() timestamp after which we kill AP.
*/
bool masterAPConfigured = false;
unsigned long masterStateChangeTime = 0;

// You can increase this to give more time for Slaves to connect.
const unsigned long masterConfiguredAPDuration = 45000; // 45s window

bool masterReconnectPending = false;
unsigned long masterReconnectTime = 0;

/* -------------------------------------------------------------------------
   Slave-Related Tracking
   --------------------------------------------------------------------------
   - slaveCredentialsReceivedTime:  Timestamp of the moment we got new creds
   - slaveCredentialTimeout:        We wait 20s to let "all" slaves presumably
                                    get credentials before finalizing
   - slaveCredentialsReceived:      True if we just received new credentials
*/
unsigned long slaveCredentialsReceivedTime = 0;
const unsigned long slaveCredentialTimeout = 20000; // 20 seconds
bool slaveCredentialsReceived = false;

/* -------------------------------------------------------------------------
   Standby / Scan Intervals
   --------------------------------------------------------------------------
   - In ROLE_STANDBY, we scan for the "CONFIGURED_MASTER_SSID" every 5 seconds
   - slaveRetryInterval is how often a Slave tries to re-join the Master if it
     lacks credentials
*/
unsigned long standbyLastCheckTime = 0;
const unsigned long standbyCheckInterval = 5000;    // 5s
unsigned long lastSlaveAttemptTime = 0;
const unsigned long slaveRetryInterval = 5000;      // 5s

// Forward Declarations for all our functions:
void handleFirmwareVersion();
void resetDevice();
void connectToWiFi();
bool retrieveCredentialsFromMaster();
bool sendGetCredentialsRequest(const String& masterIP);
bool isValidCredential(const char* credential, bool isSSID = true);
void startAccessPoint(const char* ssid, const char* password);
void handleClient(WiFiClient& client);
void parseHttpRequest(String& fullRequest, WiFiClient& client);
void sendConfigPage(WiFiClient& client, const String& errorMsg = "");
void sendConfirmationPage(WiFiClient& client, const String& message = "");
void sendCredentials(WiFiClient& client);
bool testInternetConnection();
String urlDecode(String str);
void enterStandbyMode();
void parseAndSaveCredentials(const String& request, WiFiClient& client);
void factoryResetEEPROM();

/* -------------------------------------------------------------------------
   setup()
   --------------------------------------------------------------------------
   - Initializes serial, EEPROM, checks firmware version
   - Reads stored credentials from EEPROM
   - Scans for Master AP networks to decide our role (Master, Slave, or Standby)
*/
void setup() {
  Serial.begin(115200);
  Serial.println("----- Arduino Uno R4 WiFi Setup Started -----");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Check if EEPROM is large enough
  if (EEPROM.length() < EEPROM_SIZE) {
    Serial.println("EEPROM size is smaller than expected!");
    // handle if needed
  } else {
    Serial.println("EEPROM initialized.");
  }

  // Handle firmware version mismatch
  handleFirmwareVersion();

  // Retrieve credentials from EEPROM
  EEPROM.get(SSID_ADDR, storedSSID);
  EEPROM.get(PASS_ADDR, storedPass);
  credentialsValid = (EEPROM.read(VALID_FLAG_ADDR) == 1);

  Serial.print("Credentials Valid at startup: ");
  Serial.println(credentialsValid ? "Yes" : "No");

  // -------------------------------------------------------------
  //  1) If we have credentials, connect immediately to router
  // -------------------------------------------------------------
  if (credentialsValid) {
    Serial.println("Credentials are valid. Attempting immediate WiFi connection...");
    connectToWiFi(); // This tries to connect using storedSSID/storedPass

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Successfully connected using stored credentials!");
      Serial.print("Local IP: ");
      Serial.println(WiFi.localIP());
      
      // If you want to define a new "normal operation" role, you can do so.
      // Or simply set ROLE_SLAVE if your code expects a role:
      currentRole = ROLE_SLAVE; // Or ROLE_OPERATION, if you define one

      // Because we are done setting up (connected to router), we can return
      // to skip the role detection code entirely:
      Serial.println("Skipping role detection because we're already connected.");
      Serial.println("----- Setup Completed (Already Connected) -----");
      return;
    }
    else {
      // If WiFi.status() != WL_CONNECTED after connect attempts,
      // either credentials are no longer valid or network is down
      Serial.println("Failed to connect with stored credentials. Proceeding with role detection...");
      credentialsValid = false; // Mark them invalid so we run normal flow
    }
  }

  // ----------------------------------------------------------------
  //  2) If no valid credentials or connection failed, do role detection
  // ----------------------------------------------------------------
  Serial.println("Determining device role via scan...");
  int n = WiFi.scanNetworks();
  if (n < 0) {
    Serial.println("WiFi scan failed.");
    n = 0;
  }

  bool configuredMasterFound = false;
  bool unconfiguredMasterFound = false;

  // Check for Master SSIDs
  for (int i = 0; i < n; i++) {
    String foundSSID = WiFi.SSID(i);
    foundSSID.trim(); // recommended to avoid trailing spaces
    Serial.print("Found SSID: '");
    Serial.print(foundSSID);
    Serial.print("'   length=");
    Serial.println(foundSSID.length());

    if (foundSSID == CONFIGURED_MASTER_SSID) {
      configuredMasterFound = true;
    }
    if (foundSSID == MASTER_SSID) {
      unconfiguredMasterFound = true;
    }
  }

  // Decide which role to take
  if (configuredMasterFound) {
    currentRole = ROLE_SLAVE;
    Serial.println("Configured Master AP detected. Becoming Slave...");

    // We (intentionally) do NOT have valid credentials (or they didn't work),
    // so let's attempt to join the Master AP to retrieve them
    Serial.println("Connecting to Master’s AP to retrieve credentials...");
    WiFi.begin(CONFIGURED_MASTER_SSID, CONFIGURED_MASTER_PASSWORD);
    int attempts = 0;
    const int maxAttempts = 20;
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
      delay(500);
      attempts++;
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nNow retrieving credentials...");
      bool success = retrieveCredentialsFromMaster();
      if (success) {
        // If successfully retrieved new creds, connect to router
        connectToWiFi();
      } else {
        Serial.println("Failed to retrieve credentials. Remaining Slave, will retry...");
      }
    } else {
      Serial.println("\nCould not join Master’s configured AP. Remaining Slave, will retry...");
    }
  }
  else if (unconfiguredMasterFound) {
    currentRole = ROLE_STANDBY;
    Serial.println("Unconfigured Master AP detected. Entering Standby...");
    enterStandbyMode();
  }
  else {
    // If no Master AP found, become Master ourselves
    currentRole = ROLE_MASTER;
    Serial.println("No Master AP found. Becoming Master...");
    startAccessPoint(MASTER_SSID, MASTER_PASSWORD);
  }

  Serial.println("----- Setup Completed -----");
}



/* -------------------------------------------------------------------------
   loop()
   --------------------------------------------------------------------------
   We handle different roles:

   - ROLE_MASTER: Accept incoming HTTP clients on the AP, serve config page,
     or provide credentials to Slaves
   - ROLE_SLAVE:  If no credentials, keep trying to connect to the Master AP
                  to retrieve them. If we have them, connect to the router
                  and do normal operation
   - ROLE_STANDBY: Keep scanning for a Configured Master. Once we see it,
                   become a Slave
*/
void loop() {
  switch (currentRole) {

    case ROLE_MASTER: {
      // If we’re Master, listen for HTTP connections on port 80
      WiFiClient client = server.available();
      if (client) {
        String requestBuffer;
        unsigned long start = millis();
        // Read incoming data until a blank line or 10s timeout
        while (client.connected() && (millis() - start < 10000)) {
          while (client.available()) {
            char c = client.read();
            requestBuffer += c;
            // Once we see "\r\n\r\n", we know we finished headers
            if (requestBuffer.indexOf("\r\n\r\n") != -1) {
              break;
            }
          }
          if (requestBuffer.indexOf("\r\n\r\n") != -1) {
            break;
          }
        }
        // Parse the incoming HTTP request
        parseHttpRequest(requestBuffer, client);
        client.stop();
      }

      // If we've launched a "Configured Master" AP, keep it alive for
      // masterConfiguredAPDuration (60s by default)
      if (masterAPConfigured) {
        if (masterReconnectPending && (millis() >= masterReconnectTime)) {
          Serial.println("60s window ended. Killing AP and connecting Master to router WiFi...");
          // We kill the AP
          WiFi.disconnect();
          delay(1000);

          // Then connect using the newly stored credentials
          connectToWiFi();
          masterReconnectPending = false;
          masterAPConfigured = false;
        }
      }
    }
    break;

    case ROLE_SLAVE: {
      // Only do this if we do NOT have credentials yet:
      if (!credentialsValid) {
        // Check every 5 seconds
        if (millis() - lastSlaveAttemptTime > slaveRetryInterval) {
          lastSlaveAttemptTime = millis();
          Serial.println("Slave: scanning for available networks...");
          
          int n = WiFi.scanNetworks();
          if (n <= 0) {
            Serial.println("No networks found in SLAVE scan. Will retry...");
          } else {
            Serial.print(n);
            Serial.println(" networks found in SLAVE scan:");
          }
          
          // Check if CONFIGURED_MASTER_SSID is in the scan results
          bool foundConfiguredAP = false;
          for (int i = 0; i < n; i++) {
            String ssidFound = WiFi.SSID(i);
            Serial.print("  Found SSID: ");
            Serial.println(ssidFound);
            if (ssidFound == CONFIGURED_MASTER_SSID) {
              foundConfiguredAP = true;
            }
          }
          
          // Now only attempt to connect if we actually saw the SSID
          if (foundConfiguredAP) {
            Serial.println("Slave: CONFIGURED_MASTER_SSID detected; attempting connection...");
            WiFi.disconnect();
            WiFi.begin(CONFIGURED_MASTER_SSID, CONFIGURED_MASTER_PASSWORD);

            int attempts = 0;
            const int maxAttempts = 20;
            while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
              delay(500);
              attempts++;
              Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
              // Attempt to retrieve credentials from Master
              bool success = retrieveCredentialsFromMaster();
              if (success) {
                connectToWiFi(); // Connect to actual router SSID after retrieval
              } else {
                Serial.println("Slave: Failed to retrieve credentials. Will retry...");
              }
            } else {
              Serial.println("\nSlave: Could not join Master’s AP. Will retry...");
            }
          } else {
            Serial.println("Slave: CONFIGURED_MASTER_SSID not found in scan. Will retry later...");
          }
        }
      } else {
        // If we DO have credentials, make sure we are connected
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("Slave: trying to reconnect to router...");
          connectToWiFi();
        } else {
          // Normal operation once connected: blink the LED
          digitalWrite(LED_BUILTIN, HIGH);
          delay(300);
          digitalWrite(LED_BUILTIN, LOW);
          delay(300);
        }
        // If we just received credentials, we wait 20 seconds, then finalize
        if (slaveCredentialsReceived) {
          if (millis() - slaveCredentialsReceivedTime > slaveCredentialTimeout) {
            Serial.println("All slaves likely have credentials. Final connect...");
            slaveCredentialsReceived = false;
            connectToWiFi();
          }
        }
      }
    }
    break;

    case ROLE_STANDBY: {
      // Check if it's time to scan again
      if (millis() - standbyLastCheckTime >= standbyCheckInterval) {
        standbyLastCheckTime = millis();
        Serial.println("Standby scanning for Configured Master...");

        // Perform the Wi-Fi scan
        int foundNets = WiFi.scanNetworks();
        if (foundNets < 0) {
          // If scanNetworks() returned negative, we found no networks
          Serial.println("No networks found in STANDBY scan (scan failed).");
          foundNets = 0;
        }
        else if (foundNets == 0) {
          Serial.println("No networks found in STANDBY scan.");
        }
        else {
          Serial.print(foundNets);
          Serial.println(" networks found in STANDBY scan:");
        }

        bool foundConfigured = false;

        // Print each SSID found, with trimming and debugging
        for (int i = 0; i < foundNets; i++) {
          String netSSID = WiFi.SSID(i);
          netSSID.trim(); // Remove any leading/trailing whitespace or non-printable chars

          // Debug print: show quotes around the SSID, plus its length
          Serial.print("  Found SSID: '");
          Serial.print(netSSID);
          Serial.print("'   length=");
          Serial.println(netSSID.length());

          // Compare with CONFIGURED_MASTER_SSID after trimming
          if (netSSID == CONFIGURED_MASTER_SSID) {
            foundConfigured = true;
          }
        }

        // If we see the Configured Master SSID, become a Slave
        if (foundConfigured) {
          Serial.println("Found Configured Master. Becoming Slave...");
          currentRole = ROLE_SLAVE;

          if (credentialsValid) {
            // If we already have valid router credentials, connect to the router
            connectToWiFi();
          } else {
            // Otherwise, connect to the Master's AP and try retrieving the new credentials
            WiFi.begin(CONFIGURED_MASTER_SSID, CONFIGURED_MASTER_PASSWORD);
            int attempts = 0;
            const int maxAttempts = 20;
            while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
              delay(500);
              attempts++;
              Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
              bool success = retrieveCredentialsFromMaster();
              if (success) {
                connectToWiFi();
              } else {
                Serial.println("Failed retrieving from Master. Will remain Slave and retry later...");
              }
            } else {
              Serial.println("\nCould not join Master. Staying in Slave mode, will retry later.");
            }
          }
        }
        else {
          // Not found, remain in Standby
          Serial.println("No Configured Master found. Remaining in Standby.");
        }
      }
    } // end ROLE_STANDBY

    break;

    default: {
      // If we somehow end up in an undefined role, just go to Standby
      Serial.println("Undefined role. Entering Standby...");
      currentRole = ROLE_STANDBY;
      enterStandbyMode();
    }
    break;
  }
}

/* -------------------------------------------------------------------------
   parseHttpRequest(String& fullRequest, WiFiClient& client)
   --------------------------------------------------------------------------
   Handles incoming GET requests on the Master. Slaves (or a user’s browser)
   hit these endpoints:
   - /save?ssid=...&pass=...    to store new credentials
   - /get_credentials           to retrieve credentials in JSON format
   - /factory_reset             to wipe EEPROM completely
*/
void parseHttpRequest(String& fullRequest, WiFiClient& client) {
  int getIndex = fullRequest.indexOf("GET ");
  if (getIndex == -1) {
    // We only handle GET in this code
    Serial.println("Only GET requests handled. Serving config page...");
    sendConfigPage(client);
    return;
  }
  int pathEnd = fullRequest.indexOf(" ", getIndex + 4);
  if (pathEnd == -1) pathEnd = fullRequest.length();
  String path = fullRequest.substring(getIndex + 4, pathEnd);
  Serial.print("Parsed path: ");
  Serial.println(path);

  if (path.startsWith("/save")) {
    // When we get new credentials from the web form, we parse and store them
    parseAndSaveCredentials(path, client);
  } 
  else if (path.startsWith("/get_credentials")) {
    // Slaves call this to get the stored SSID/PASS
    sendCredentials(client);
  }
  else if (path.startsWith("/factory_reset")) {
    // Erase everything in EEPROM, then reset the device
    factoryResetEEPROM();
    String resp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html\r\n"
      "Connection: close\r\n\r\n"
      "<html><body><h1>Factory Reset!</h1>"
      "<p>Device will reboot now.</p></body></html>";
    client.print(resp);
    client.flush();
    delay(1000);
    resetDevice();
  }
  else {
    // Any other path, we just serve the config page
    sendConfigPage(client);
  }
}

/* -------------------------------------------------------------------------
   sendConfigPage(WiFiClient& client, const String& errorMsg)
   --------------------------------------------------------------------------
   Serves a simple HTML form to collect SSID and Password from the user.
   If there's an error (like invalid input), we show it in red.
*/
void sendConfigPage(WiFiClient& client, const String& errorMsg) {
  Serial.println("Sending WiFi Configuration Page to client...");
  String page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><title>WiFi Config</title></head><body>"
    "<h1>WiFi Configuration</h1>";

  if (errorMsg.length() > 0) {
    page += "<p style='color:red;'>" + errorMsg + "</p>";
  }

  page +=
    "<form action='/save' method='get'>"
    "SSID: <input type='text' name='ssid' required><br>"
    "Password: <input type='password' name='pass' required><br>"
    "<input type='submit' value='Save'>"
    "</form>"
    "<hr>"
    "<p><a href='/factory_reset'>Factory Reset</a></p>"
    "</body></html>";

  client.print(page);
}

/* -------------------------------------------------------------------------
   sendConfirmationPage(WiFiClient& client, const String& message)
   --------------------------------------------------------------------------
   After the user saves credentials, we show a simple confirmation HTML page.
*/
void sendConfirmationPage(WiFiClient& client, const String& message) {
  String page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><title>Config Confirm</title>"
    "<meta http-equiv='refresh' content='5;url=/'></head><body>"
    "<h1>WiFi Configuration</h1>"
    "<p>" + message + "</p>"
    "</body></html>";
  client.print(page);
}

/* -------------------------------------------------------------------------
   sendCredentials(WiFiClient& client)
   --------------------------------------------------------------------------
   Sends the stored SSID/PASS to a Slave in JSON format, if valid.
*/
void sendCredentials(WiFiClient& client) {
  Serial.println("Sending stored credentials in JSON...");
  // If we marked them as invalid, we can't share them
  if (!credentialsValid) {
    String resp =
      "HTTP/1.1 400 Bad Request\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n\r\n"
      "Invalid or no credentials stored.";
    client.print(resp);
    return;
  }
  StaticJsonDocument<300> doc;
  doc["ssid"] = String(storedSSID);
  doc["pass"] = String(storedPass);
  doc["configured_ssid"] = CONFIGURED_MASTER_SSID;

  String json;
  serializeJson(doc, json);

  String resp =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n\r\n" +
    json;
  client.print(resp);
}

/* -------------------------------------------------------------------------
   parseAndSaveCredentials()
   --------------------------------------------------------------------------
   Called when Master receives "/save?ssid=xyz&pass=abc" from the user’s form.
   We always overwrite stored credentials in EEPROM if they're valid.
   This ensures the old credentials do NOT remain once the user provides new.
*/
void parseAndSaveCredentials(const String& request, WiFiClient& client) {
  Serial.println("Parsing /save request to store new credentials...");

  // Check if there's a query string (?)
  int qStart = request.indexOf("?");
  if (qStart == -1) {
    Serial.println("No query in /save request.");
    sendConfigPage(client, "No data received.");
    return;
  }

  // Extract the query substring
  String query = request.substring(qStart + 1);
  Serial.print("Extracted Query: ");
  Serial.println(query);

  // Look for ssid= and pass=
  int ssidStart = query.indexOf("ssid=");
  int passStart = query.indexOf("pass=");
  if (ssidStart == -1 || passStart == -1) {
    Serial.println("Malformed request, missing SSID or Pass.");
    sendConfigPage(client, "Malformed request, missing SSID or Pass.");
    return;
  }
  ssidStart += 5; // Skip "ssid="
  passStart += 5; // Skip "pass="

  // Determine the end positions (could be an '&' or the end of the string)
  int ssidEnd = query.indexOf('&', ssidStart);
  if (ssidEnd == -1) ssidEnd = query.length();
  int passEnd = query.indexOf('&', passStart);
  if (passEnd == -1) passEnd = query.length();

  // Extract the raw SSID and Password from the query
  String newSSID = query.substring(ssidStart, ssidEnd);
  String newPass = query.substring(passStart, passEnd);

  // URL-decode them (handle %20, etc.) and trim whitespace
  newSSID = urlDecode(newSSID);
  newPass = urlDecode(newPass);
  newSSID.trim();
  newPass.trim();

  Serial.print("Parsed SSID: ");
  Serial.println(newSSID);
  Serial.print("Parsed Password: ");
  Serial.println(newPass);

  // Validate the SSID and Password format
  if (!isValidCredential(newSSID.c_str(), true)) {
    sendConfigPage(client, "Invalid SSID (must be 1..32 printable chars).");
    return;
  }
  if (!isValidCredential(newPass.c_str(), false)) {
    sendConfigPage(client, "Invalid Password (8..64 printable chars).");
    return;
  }

  // -------------------------------
  // ALWAYS OVERWRITE OLD CREDENTIALS
  // -------------------------------
  // Because we validated them, we consider them correct. We store them in EEPROM
  // so next time we start up, we have them as valid.
  newSSID.toCharArray(storedSSID, sizeof(storedSSID));
  newPass.toCharArray(storedPass, sizeof(storedPass));

  // Write them to EEPROM
  EEPROM.put(SSID_ADDR, storedSSID);
  EEPROM.put(PASS_ADDR, storedPass);
  EEPROM.update(VALID_FLAG_ADDR, 1); // Mark as valid
  credentialsValid = true;

  // Now that we have new router credentials, we become a "Configured Master"
  // We kill the old AP, and start a new AP with "ArduinoMasterAP_Configured"
  Serial.println("Closing Unconfigured AP...");
  WiFi.disconnect();
  delay(1000);

  Serial.println("Starting 'Configured Master' AP, for Slaves to retrieve credentials...");
  startAccessPoint(CONFIGURED_MASTER_SSID, CONFIGURED_MASTER_PASSWORD);
  masterAPConfigured = true;
  masterStateChangeTime = millis();

  // In 60s (masterConfiguredAPDuration), we will shut this AP off and connect
  // to the router ourselves
  masterReconnectPending = true;
  masterReconnectTime = masterStateChangeTime + masterConfiguredAPDuration;

  // Finally, send an HTML confirmation to the user
  sendConfirmationPage(client,
    "Credentials saved! Master is now in AP mode for 60s so Slaves can connect. After that, I will join the router.");
}

/* -------------------------------------------------------------------------
   isValidCredential(const char* credential, bool isSSID)
   --------------------------------------------------------------------------
   Checks if the string is within allowed length and printable ASCII range.
   SSID range: 1..32, Pass range: 8..64
*/
bool isValidCredential(const char* credential, bool isSSID) {
  if (!credential) return false;
  size_t len = strlen(credential);

  // Enforce length constraints
  if (isSSID) {
    if (len == 0 || len > 32) return false;
  } else {
    if (len < 8 || len > 64) return false;
  }

  // Enforce that each character is in printable ASCII range 32..126
  for (size_t i = 0; i < len; i++) {
    if (credential[i] < 32 || credential[i] > 126) {
      return false;
    }
  }
  return true;
}

/* -------------------------------------------------------------------------
   urlDecode(String str)
   --------------------------------------------------------------------------
   Replaces %xx and '+' with correct ASCII characters.
   Helps handle typical form inputs with spaces and special chars.
*/
String urlDecode(String str) {
  String decoded;
  decoded.reserve(str.length());
  for (int i = 0; i < (int)str.length(); i++) {
    char c = str[i];
    if (c == '%') {
      if (i + 2 < (int)str.length()) {
        char a = str[++i];
        char b = str[++i];
        if (isxdigit(a) && isxdigit(b)) {
          a = tolower(a);
          b = tolower(b);
          a = (a >= 'a') ? (a - 'a' + 10) : (a - '0');
          b = (b >= 'a') ? (b - 'a' + 10) : (b - '0');
          decoded += char((a << 4) | b);
        } else {
          // If invalid, we skip
        }
      }
    }
    else if (c == '+') {
      decoded += ' ';
    }
    else {
      decoded += c;
    }
  }
  return decoded;
}

/* -------------------------------------------------------------------------
   connectToWiFi()
   --------------------------------------------------------------------------
   Uses the storedSSID/storedPass from EEPROM to connect to the router.
   If unable to connect, it reverts to the unconfigured AP to let user fix it.
*/
void connectToWiFi() {
  if (!credentialsValid) {
    Serial.println("Cannot connectToWiFi: credentials are invalid!");
    return;
  }
  Serial.print("Connecting to router SSID: ");
  Serial.println(storedSSID);
  digitalWrite(LED_BUILTIN, HIGH);

  WiFi.begin(storedSSID, storedPass);
  int attempts = 0;
  const int maxAttempts = 20;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    attempts++;
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Could be Master or Slave connecting to router
    Serial.println("\nMaster (or Slave) connected to router!");
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to router. Reverting to Unconfigured AP...");
    startAccessPoint(MASTER_SSID, MASTER_PASSWORD);
  }
  digitalWrite(LED_BUILTIN, LOW);
}

/* -------------------------------------------------------------------------
   retrieveCredentialsFromMaster()
   --------------------------------------------------------------------------
   Called by a Slave who wants the Master’s saved router credentials.
   This is done over HTTP via GET /get_credentials
*/
bool retrieveCredentialsFromMaster() {
  Serial.println("Attempting to retrieve credentials from Master...");
  // For the typical Arduino AP, the default IP is 192.168.4.1
  String masterIP = "192.168.4.1";

  bool success = sendGetCredentialsRequest(masterIP);
  if (success) {
    Serial.println("Credentials retrieved from Master!");
    // Mark we just got new credentials
    slaveCredentialsReceivedTime = millis();
    slaveCredentialsReceived = true;
  } else {
    Serial.println("Failed to retrieve credentials from Master.");
  }
  return success;
}

/* -------------------------------------------------------------------------
   sendGetCredentialsRequest(const String& masterIP)
   --------------------------------------------------------------------------
   Performs a simple HTTP GET to /get_credentials on the Master
   to retrieve the JSON with SSID/PASS
*/
bool sendGetCredentialsRequest(const String& masterIP) {
  WiFiClient client;
  const int httpPort = 80;

  Serial.print("Connecting to Master at ");
  Serial.print(masterIP);
  Serial.print(":");
  Serial.println(httpPort);

  if (!client.connect(masterIP.c_str(), httpPort)) {
    Serial.println("Connection failed.");
    return false;
  }

  String url = "/get_credentials";
  String req =
    String("GET ") + url + " HTTP/1.1\r\n" +
    "Host: " + masterIP + "\r\n" +
    "Connection: close\r\n\r\n";
  client.print(req);

  String response;
  unsigned long start = millis();
  while ((millis() - start) < 5000) {
    while (client.available()) {
      char c = client.read();
      response += c;
    }
    if (!client.connected()) break;
  }
  client.stop();

  // We look for a JSON object (starts with '{')
  int jsonStart = response.indexOf('{');
  if (jsonStart < 0) {
    Serial.println("No JSON found in response.");
    Serial.println(response);
    return false;
  }

  // Extract the JSON part
  String jsonPart = response.substring(jsonStart);
  Serial.println("Received JSON: ");
  Serial.println(jsonPart);

  StaticJsonDocument<300> doc;
  DeserializationError err = deserializeJson(doc, jsonPart);
  if (err) {
    Serial.print("JSON parse fail: ");
    Serial.println(err.c_str());
    return false;
  }

  // Retrieve SSID, PASS, and the "configured_ssid" from Master
  String newSSID = doc["ssid"] | "";
  String newPass = doc["pass"] | "";
  String configuredSSID = doc["configured_ssid"] | "";

  // If the Master isn’t actually “CONFIGURED_MASTER_SSID”, we treat it as an error
  if (!configuredSSID.equals(CONFIGURED_MASTER_SSID)) {
    Serial.println("Master is not in configured state. Mismatch SSID!");
    return false;
  }

  // Validate
  if (!isValidCredential(newSSID.c_str(), true) ||
      !isValidCredential(newPass.c_str(), false)) {
    Serial.println("Received invalid credentials.");
    return false;
  }

  // Because credentials are valid, we overwrite existing EEPROM:
  newSSID.toCharArray(storedSSID, sizeof(storedSSID));
  newPass.toCharArray(storedPass, sizeof(storedPass));
  EEPROM.put(SSID_ADDR, storedSSID);
  EEPROM.put(PASS_ADDR, storedPass);
  EEPROM.update(VALID_FLAG_ADDR, 1);
  credentialsValid = true;

  Serial.println("Credentials saved. Slave is good to connect!");
  return true;
}

/* -------------------------------------------------------------------------
   testInternetConnection()
   --------------------------------------------------------------------------
   Optional function to test if we can reach an external site (Google).
   This is just for debugging connectivity if desired.
*/
bool testInternetConnection() {
  WiFiClient client;
  const char* host = "www.google.com";
  const uint16_t port = 80;
  if (!client.connect(host, port)) {
    Serial.println("Cannot reach google.com.");
    return false;
  }
  client.print("GET / HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n");

  unsigned long startTime = millis();
  bool ok = false;
  while (client.connected() && millis() - startTime < 5000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      if (line.startsWith("HTTP/1.1 200")) {
        ok = true;
      }
    }
  }
  client.stop();
  return ok;
}

/* -------------------------------------------------------------------------
   startAccessPoint(const char* ssid, const char* password)
   --------------------------------------------------------------------------
   Tells the UNO R4 WiFi to form an AP with the given SSID and Password.
   We then start our server so we can serve the config pages.
*/
void startAccessPoint(const char* ssid, const char* password) {
  Serial.print("Starting AP: ");
  Serial.println(ssid);
  bool ok = WiFi.beginAP(ssid, password);
  if (!ok) {
    Serial.println("beginAP failed!");
    return;
  }
  server.begin();
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.localIP());
  digitalWrite(LED_BUILTIN, HIGH);
}

/* -------------------------------------------------------------------------
   enterStandbyMode()
   --------------------------------------------------------------------------
   ROLE_STANDBY logic. We set a timestamp so every 5s we do a new scan
   to see if a Configured Master shows up.
*/
void enterStandbyMode() {
  standbyLastCheckTime = millis();
  Serial.println("Entered Standby mode. Will scan every 5s for Configured Master.");
}

/* -------------------------------------------------------------------------
   handleFirmwareVersion()
   --------------------------------------------------------------------------
   Checks if the firmware version in EEPROM matches CURRENT_FW_VERSION.
   If not, we wipe the EEPROM to avoid storing potentially incompatible data.
*/
void handleFirmwareVersion() {
  uint8_t storedFwVersion = EEPROM.read(FW_VERSION_ADDR);
  if (storedFwVersion != CURRENT_FW_VERSION) {
    Serial.println("Firmware mismatch, wiping EEPROM...");
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.write(FW_VERSION_ADDR, CURRENT_FW_VERSION);
    Serial.println("EEPROM cleared and version updated. Restarting...");
    delay(2000);
    resetDevice();
  } else {
    Serial.println("Firmware version matches. No action required.");
  }
}

/* -------------------------------------------------------------------------
   resetDevice()
   --------------------------------------------------------------------------
   Performs a system reset on the UNO R4 WiFi via NVIC_SystemReset().
*/
void resetDevice() {
  Serial.println("NVIC_SystemReset() called...");
  NVIC_SystemReset();
}

/* -------------------------------------------------------------------------
   factoryResetEEPROM()
   --------------------------------------------------------------------------
   Overwrites the entire reserved EEPROM area (EEPROM_SIZE bytes) with 0.
   This effectively removes credentials, version flags, etc.
*/
void factoryResetEEPROM() {
  Serial.println("Factory Reset: Wiping entire EEPROM!");
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  Serial.println("EEPROM cleared. Credentials, version, etc. erased.");
}
