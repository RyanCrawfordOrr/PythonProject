#include <WiFiS3.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ctype.h> // For isxdigit

// Constants and Definitions
#define EEPROM_SIZE          96          // Total EEPROM size (Ensure this does not exceed EEPROM.length())
#define SSID_ADDR            0
#define PASS_ADDR            32
#define VALID_FLAG_ADDR      64
#define FW_VERSION_ADDR      65          // Address to store firmware version

// Firmware Version
const uint8_t CURRENT_FW_VERSION = 9;     // Increment this value with each new firmware upload

// Master AP Configuration
#define MASTER_SSID               "ArduinoMasterAP"
#define MASTER_PASSWORD           "masterConfig123"

// Configured Master AP Configuration (Signaling AP)
#define CONFIGURED_MASTER_SSID    "ArduinoMasterAP_Configured"
#define CONFIGURED_MASTER_PASSWORD "masterConfig123" // Same password

// Credentials Storage
char storedSSID[32] = {0};
char storedPass[32] = {0};
bool credentialsValid = false;

// WiFi Server on Port 80 (Used by Master)
WiFiServer server(80);

// Role Determination
enum DeviceRole {
  ROLE_MASTER,
  ROLE_SLAVE,
  ROLE_STANDBY
};

DeviceRole currentRole = ROLE_MASTER; // Default role

// Master Role Tracking
unsigned long masterStateChangeTime = 0;
const unsigned long masterConfiguredAPDuration = 60000; // 60 seconds
bool masterAPConfigured = false;

// Timer for Master to reconnect after AP is configured
bool masterReconnectPending = false;
unsigned long masterReconnectTime = 0;

// Slave Role Tracking
unsigned long slaveCredentialsReceivedTime = 0;
const unsigned long slaveCredentialTimeout = 20000; // 20 seconds
bool slaveCredentialsReceived = false;

// Standby Tracking
unsigned long standbyLastCheckTime = 0;
const unsigned long standbyCheckInterval = 5000; // 5 seconds

// Function Prototypes
void handleFirmwareVersion();
void resetDevice();
void connectToWiFi();
bool retrieveCredentialsFromMaster();
bool sendGetCredentialsRequest(const String& masterIP);
void parseAndSaveCredentials(const String& request, WiFiClient& client);
bool isValidCredential(const char* credential, bool isSSID = true);
void startAccessPoint(const char* ssid, const char* password);
void handleClient(WiFiClient& client);
void sendConfigPage(WiFiClient& client, const String& errorMsg = "");
void sendConfirmationPage(WiFiClient& client, const String& message = "");
void sendCredentials(WiFiClient& client);
bool testInternetConnection();
String urlDecode(String str);
void enterStandbyMode();

// Setup function
void setup() {
  // Initialize Serial Communication
  Serial.begin(115200);
  Serial.println("----- Arduino Uno R4 WiFi Setup Started -----");

  // Initialize Built-in LED for Status Indication
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // Turn off initially
  Serial.println("Built-in LED initialized.");

  // Initialize EEPROM
  if (EEPROM.length() < EEPROM_SIZE) {
    Serial.println("EEPROM size is smaller than expected!");
    // Handle EEPROM size mismatch if necessary
  } else {
    Serial.println("EEPROM initialized.");
  }

  // Check and handle firmware version
  Serial.println("Checking firmware version...");
  handleFirmwareVersion();

  // Read Stored Credentials from EEPROM
  Serial.println("Reading stored WiFi credentials from EEPROM...");
  EEPROM.get(SSID_ADDR, storedSSID);
  EEPROM.get(PASS_ADDR, storedPass);
  credentialsValid = (EEPROM.read(VALID_FLAG_ADDR) == 1);

  // Display Retrieved Credentials Status
  Serial.print("Credentials Valid: ");
  Serial.println(credentialsValid ? "Yes" : "No");

  // Determine Role: Master, Slave, or Standby based on AP SSID
  Serial.println("Determining device role...");
  int n = WiFi.scanNetworks(); // Start scanning (blocking)

  if (n < 0) {
    Serial.println("WiFi scan failed.");
    n = 0;
  }

  bool configuredMasterFound = false;
  bool unconfiguredMasterFound = false;
  for (int i = 0; i < n; ++i) {
    String currentSSID = WiFi.SSID(i);
    Serial.print("Found SSID: ");
    Serial.println(currentSSID);
    if (currentSSID.equals(CONFIGURED_MASTER_SSID)) {
      configuredMasterFound = true;
      break; // Priority to configured AP
    }
    if (currentSSID.equals(MASTER_SSID)) {
      unconfiguredMasterFound = true;
    }
  }

  // Role Determination Logic
  if (configuredMasterFound) {
    // Configured Master AP found; become Slave
    currentRole = ROLE_SLAVE;
    Serial.println("Configured Master AP detected. Becoming Slave...");
    if (credentialsValid) {
      Serial.println("Valid credentials found. Attempting to connect to WiFi...");
      connectToWiFi();
    } else {
      Serial.println("No valid credentials found in EEPROM. Waiting for Master to provide credentials...");
      // As Slave, proceed to retrieve credentials from Master in the loop
    }
  } else if (unconfiguredMasterFound) {
    // Unconfigured Master AP found; enter Standby Mode
    currentRole = ROLE_STANDBY;
    Serial.println("Unconfigured Master AP detected. Entering Standby Mode and waiting for it to become configured...");
    enterStandbyMode();
  } else {
    // No Master AP found; become Master
    currentRole = ROLE_MASTER;
    Serial.println("No existing Master AP found. Becoming Master...");
    startAccessPoint(MASTER_SSID, MASTER_PASSWORD);
  }

  Serial.println("----- Setup Completed -----");
}

// Main loop function
void loop() {
  switch (currentRole) {
    case ROLE_MASTER:
      // Master Role: Handle incoming client requests and manage AP states
      {
        WiFiClient client = server.available(); // Check for available clients
        if (client) { // If a client is connected
          Serial.println("Client connected to Master AP.");
          handleClient(client);
          client.stop();
          Serial.println("Client disconnected from Master AP.");
        }

        if (masterAPConfigured) {
          // Check if it's time to attempt reconnecting to WiFi after 60 seconds
          if (masterReconnectPending && millis() >= masterReconnectTime) {
            Serial.println("60 seconds elapsed. Attempting to connect to WiFi as Master...");
            connectToWiFi();

            if (WiFi.status() == WL_CONNECTED) {
              Serial.println("Master successfully connected to WiFi.");
              // Optionally, perform tasks like logging or data transmission
            } else {
              Serial.println("Master failed to connect to WiFi.");
              // Optionally, restart the AP or notify the user
            }

            // Reset the reconnect flag
            masterReconnectPending = false;
          }
        }
      }
      break;

    case ROLE_SLAVE:
      // Slave Role: Handle standby and credential retrieval
      if (!credentialsValid) {
        // Attempt to retrieve credentials from Master
        Serial.println("Attempting to retrieve credentials from Master...");
        bool success = retrieveCredentialsFromMaster();
        if (success) {
          Serial.println("Credentials retrieved successfully from Master.");
          slaveCredentialsReceivedTime = millis();
          slaveCredentialsReceived = true;
        } else {
          Serial.println("Failed to retrieve credentials from Master. Retrying in 5 seconds...");
          delay(5000); // Wait before retrying
        }
      } else {
        // Credentials are valid; ensure connected to WiFi
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("WiFi not connected. Attempting to reconnect...");
          connectToWiFi();
        } else {
          // Connected to WiFi; perform main tasks
          // Example: Blink LED to indicate normal operation
          digitalWrite(LED_BUILTIN, HIGH);
          delay(500);
          digitalWrite(LED_BUILTIN, LOW);
          delay(500);
        }

        // If credentials were received recently, check for timeout
        if (slaveCredentialsReceived) {
          if (millis() - slaveCredentialsReceivedTime > slaveCredentialTimeout) {
            Serial.println("Assuming all slaves have received credentials. Finalizing connection...");
            slaveCredentialsReceived = false;
            connectToWiFi();
          }
        }
      }
      break;

    case ROLE_STANDBY:
      // Standby Mode: Continuously search for Configured Master AP every 5 seconds
      if (millis() - standbyLastCheckTime >= standbyCheckInterval) {
        standbyLastCheckTime = millis();
        Serial.println("Standby Check: Scanning for Configured Master AP...");
        int n = WiFi.scanNetworks(); // Start scanning (blocking)

        if (n < 0) {
          Serial.println("WiFi scan failed.");
          n = 0;
        }

        bool configuredMasterFound = false;
        for (int i = 0; i < n; ++i) {
          String currentSSID = WiFi.SSID(i);
          Serial.print("Found SSID: ");
          Serial.println(currentSSID);
          if (currentSSID.equals(CONFIGURED_MASTER_SSID)) {
            configuredMasterFound = true;
            break; // Configured AP found
          }
        }

        if (configuredMasterFound) {
          // Configured Master AP found; become Slave
          currentRole = ROLE_SLAVE;
          Serial.println("Configured Master AP detected during Standby. Becoming Slave...");
          if (credentialsValid) {
            Serial.println("Valid credentials found. Attempting to connect to WiFi...");
            connectToWiFi();
          } else {
            Serial.println("No valid credentials found in EEPROM. Waiting for Master to provide credentials...");
            // Proceed to retrieve credentials from Master in the loop
          }
        } else {
          Serial.println("Configured Master AP not found. Continuing in Standby Mode...");
        }
      }
      break;

    default:
      // Undefined Role: Enter Standby Mode as a safe fallback
      Serial.println("Undefined role detected. Entering Standby Mode...");
      currentRole = ROLE_STANDBY;
      enterStandbyMode();
      break;
  }
}

// Function to handle firmware version verification
void handleFirmwareVersion() {
  Serial.println("Handling firmware version verification...");

  // Read stored firmware version
  uint8_t storedFwVersion = EEPROM.read(FW_VERSION_ADDR);
  Serial.print("Stored Firmware Version: ");
  Serial.println(storedFwVersion);
  Serial.print("Current Firmware Version: ");
  Serial.println(CURRENT_FW_VERSION);

  if (storedFwVersion != CURRENT_FW_VERSION) {
    Serial.println("Firmware version mismatch detected.");
    Serial.println("Resetting EEPROM to default values...");

    // Clear EEPROM by writing zeros
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }

    Serial.println("EEPROM successfully cleared.");

    // Update the firmware version in EEPROM
    EEPROM.write(FW_VERSION_ADDR, CURRENT_FW_VERSION);
    Serial.println("Firmware version updated in EEPROM.");

    // Restart the device to apply changes
    Serial.println("Restarting device to apply firmware changes...");
    delay(2000); // Added delay to ensure messages are sent before reset
    resetDevice(); // Reset the device
  } else {
    Serial.println("Firmware version matches. No action required.");
  }
}

// Function to reset the device
void resetDevice() {
  Serial.println("Executing NVIC_SystemReset() to restart the device...");
  NVIC_SystemReset(); // Reset the device
}

// Function to start Access Point mode with specified SSID and password
void startAccessPoint(const char* ssid, const char* password) {
  Serial.print("Initializing Access Point with SSID: ");
  Serial.println(ssid);

  // Explicitly set WiFi mode to AP to avoid conflicts
  bool result = WiFi.beginAP(ssid, password);

  if (result) {
    Serial.println("Access Point started successfully.");
  } else {
    Serial.println("Failed to start Access Point.");
    // Handle AP startup failure if necessary
    return; // Exit the function if AP failed to start
  }

  server.begin();
  Serial.println("WiFi Server started on port 80.");

  Serial.print("AP SSID: ");
  Serial.println(ssid);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.localIP()); // Use WiFi.localIP() instead of WiFi.softAPIP()
  Serial.println("Awaiting client connections for WiFi configuration...");

  digitalWrite(LED_BUILTIN, HIGH); // Indicate AP mode
}

// Function to test internet connection by connecting to a known server
bool testInternetConnection() {
  Serial.println("Testing internet connection by connecting to google.com...");

  WiFiClient client;
  const char* testHost = "www.google.com";
  const uint16_t testPort = 80;
  const unsigned long timeout = 5000; // 5 seconds

  if (!client.connect(testHost, testPort)) {
    Serial.println("Connection to test server failed.");
    return false;
  }

  // Send HTTP GET request
  client.print(String("GET / HTTP/1.1\r\nHost: ") + testHost + "\r\nConnection: close\r\n\r\n");

  // Wait for response
  unsigned long startTime = millis();
  bool success = false;
  while (client.connected() && millis() - startTime < timeout) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      Serial.println(line);
      if (line.startsWith("HTTP/1.1 200")) {
        Serial.println("Internet connection verified successfully.");
        success = true;
        break;
      }
    }
    if (success) break;
  }

  if (!success) {
    Serial.println("No valid response from test server.");
  }

  client.stop();
  return success;
}

// Function to handle incoming client requests (Master only)
void handleClient(WiFiClient& client) {
  Serial.println("Handling incoming client request...");
  bool currentLineIsBlank = true;
  String request = "";
  String errorMsg = "";

  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 10000) { // 10-second timeout
    if (client.available()) {
      char c = client.read();
      Serial.print(c); // Log the request character by character
      request += c;

      // Detect end of HTTP request
      if (c == '\n' && currentLineIsBlank) {
        Serial.println("\nComplete HTTP request received:");
        Serial.println(request);

        // Determine the type of request
        if (request.startsWith("GET /save")) {
          Serial.println("Processing '/save' request to store new credentials.");
          parseAndSaveCredentials(request, client);
          // After parsing and saving, proceed to test internet in the loop
        } else if (request.startsWith("GET /get_credentials")) {
          Serial.println("Processing '/get_credentials' request to send stored credentials.");
          // Respond with stored credentials in JSON format
          sendCredentials(client);
        } else {
          Serial.println("Processing default request to serve configuration page.");
          // Serve the configuration page
          sendConfigPage(client, errorMsg);
        }
        break;
      }

      if (c == '\n') {
        currentLineIsBlank = true;
      } else if (c != '\r') {
        currentLineIsBlank = false;
      }
    }
  }

  if (millis() - timeout >= 10000) {
    Serial.println("Client request timed out.");
    client.stop();
  }
}

// Function to send the WiFi Configuration Page (Master only)
void sendConfigPage(WiFiClient& client, const String& errorMsg) {
  Serial.println("Sending WiFi Configuration Page to client...");

  String page = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html>"
    "<html>"
    "<head><title>WiFi Configuration</title></head>"
    "<body>"
    "<h1>WiFi Configuration</h1>";

  if (errorMsg.length() > 0) {
    page += "<p style=\"color:red;\">" + errorMsg + "</p>";
  }

  page += 
    "<form action=\"/save\" method=\"get\">"
    "SSID: <input type=\"text\" name=\"ssid\" required><br>"
    "Password: <input type=\"password\" name=\"pass\" required><br>"
    "<input type=\"submit\" value=\"Save\">"
    "</form>"
    "</body>"
    "</html>";

  client.print(page);
  Serial.println("Configuration Page successfully sent to client.");
}

// Function to send the Confirmation Page after saving credentials (Master only)
void sendConfirmationPage(WiFiClient& client, const String& message) {
  Serial.println("Sending Confirmation Page to client...");

  String confirmationPage = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>WiFi Configuration</title>"
    "<meta http-equiv=\"refresh\" content=\"5;url=/\">"
    "</head>"
    "<body>"
    "<h1>WiFi Configuration</h1>"
    "<p>" + message + "</p>"
    "</body>"
    "</html>";

  client.print(confirmationPage);
  Serial.println("Confirmation Page successfully sent to client.");
}

// Function to send stored credentials to Slave (Master only)
void sendCredentials(WiFiClient& client) {
  Serial.println("Sending stored WiFi credentials in JSON format to client...");

  if (credentialsValid) {
    // Create JSON object
    StaticJsonDocument<300> doc; // Increased size to accommodate additional fields if needed
    doc["ssid"] = String(storedSSID);
    doc["pass"] = String(storedPass);
    doc["configured_ssid"] = String(CONFIGURED_MASTER_SSID); // Include configured SSID

    String credentialsJson;
    serializeJson(doc, credentialsJson);

    // Prepare HTTP response
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Connection: close\r\n\r\n";
    response += credentialsJson;

    client.print(response);
    Serial.println("Credentials successfully sent to client.");
  } else {
    // Respond with an error if credentials are invalid
    String response = "HTTP/1.1 400 Bad Request\r\n"
                      "Content-Type: text/plain\r\n"
                      "Connection: close\r\n\r\n"
                      "Invalid or no credentials stored.";

    client.print(response);
    Serial.println("Invalid credentials. Error message sent to client.");
  }
}

// Function to parse and save credentials from '/save' request (Master only)
void parseAndSaveCredentials(const String& request, WiFiClient& client) {
  Serial.println("Parsing WiFi credentials from client request...");
  String errorMsg = "";

  // Extract the query string
  int start = request.indexOf("?");
  if (start == -1) {
    Serial.println("No query string found in request.");
    errorMsg = "No data received. Please provide SSID and Password.";
    sendConfigPage(client, errorMsg);
    return;
  }
  int end = request.indexOf(" ", start);
  if (end == -1) end = request.length();
  String query = request.substring(start + 1, end);
  Serial.print("Extracted Query String: ");
  Serial.println(query);

  // Parse SSID and Password
  int ssidStart = query.indexOf("ssid=");
  int passStart = query.indexOf("pass=");

  if (ssidStart == -1 || passStart == -1) {
    Serial.println("Malformed request. Missing SSID or Password.");
    errorMsg = "Malformed request. Please ensure both SSID and Password are provided.";
    sendConfigPage(client, errorMsg);
    return;
  }

  ssidStart += 5; // Length of "ssid="
  passStart += 5; // Length of "pass="

  int ssidEnd = query.indexOf('&', ssidStart);
  if (ssidEnd == -1) ssidEnd = query.length();
  int passEnd = query.indexOf('&', passStart);
  if (passEnd == -1) passEnd = query.length();

  String newSSID = query.substring(ssidStart, ssidEnd);
  String newPass = query.substring(passStart, passEnd);

  Serial.print("Parsed SSID: ");
  Serial.println(newSSID);
  Serial.print("Parsed Password: ");
  Serial.println(newPass);

  // Decode URL-encoded characters
  newSSID = urlDecode(newSSID);
  newPass = urlDecode(newPass);

  // Trim whitespace
  newSSID.trim();
  newPass.trim();

  // Validate credentials
  if (!isValidCredential(newSSID.c_str(), true)) {
    Serial.println("Invalid SSID provided.");
    errorMsg = "Invalid SSID. Please ensure it is between 1 and 31 characters.";
    sendConfigPage(client, errorMsg);
    return;
  }

  if (!isValidCredential(newPass.c_str(), false)) {
    Serial.println("Invalid Password provided.");
    errorMsg = "Invalid Password. Please ensure it is between 8 and 63 characters.";
    sendConfigPage(client, errorMsg);
    return;
  }

  newSSID.toCharArray(storedSSID, sizeof(storedSSID));
  newPass.toCharArray(storedPass, sizeof(storedPass));

  Serial.print("Decoded SSID: ");
  Serial.println(storedSSID);
  Serial.print("Decoded Password: ");
  Serial.println(storedPass);

  // Save to EEPROM
  Serial.println("Saving new credentials to EEPROM...");
  EEPROM.put(SSID_ADDR, storedSSID);
  EEPROM.put(PASS_ADDR, storedPass);
  EEPROM.update(VALID_FLAG_ADDR, 1);
  // EEPROM.commit(); // Removed as EEPROM.commit() does not exist in standard Arduino EEPROM

  Serial.println("New credentials saved to EEPROM.");
  credentialsValid = true;

  // Close the AP
  Serial.println("Closing Access Point...");
  WiFi.disconnect(); // Disconnect and disable AP
  delay(1000); // Wait for AP to close

  // Attempt to connect to WiFi as station
  Serial.println("Attempting to connect to the provided WiFi network as Master...");
  WiFi.begin(storedSSID, storedPass);

  int attempts = 0;
  const int maxAttempts = 20; // Total wait time: 10 seconds
  bool connected = false;

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print("Attempt ");
    Serial.print(attempts + 1);
    Serial.println(": Connecting...");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi successfully.");

    // Disconnect from WiFi to set up Configured AP
    WiFi.disconnect();
    delay(1000);

    // Start Configured Master AP
    Serial.println("Reconfiguring Access Point to Configured Master AP...");
    startAccessPoint(CONFIGURED_MASTER_SSID, CONFIGURED_MASTER_PASSWORD);
    masterAPConfigured = true;
    masterStateChangeTime = millis();

    // Set timer to connect to WiFi after 60 seconds
    masterReconnectPending = true;
    masterReconnectTime = millis() + masterConfiguredAPDuration;

    Serial.println("Reconfigured AP to Configured Master AP successfully.");
  } else {
    Serial.println("\nFailed to connect to WiFi. Remaining in Unconfigured Master AP mode.");
    // Optionally, you can restart the AP or notify the user
  }

  // Respond with a confirmation page
  sendConfirmationPage(client, "Credentials saved successfully! The device will now attempt to connect to the WiFi network.");
}

// Function to validate that a credential is non-empty and meets length requirements
bool isValidCredential(const char* credential, bool isSSID) {
  if (credential == nullptr) return false;
  size_t len = strlen(credential);
  
  if (isSSID) {
    if (len == 0 || len > 31) return false; // SSID max length is 32 bytes including null terminator
  } else {
    if (len < 8 || len > 63) return false; // Password length between 8 and 63 characters
  }
  
  // Check for invalid characters (ASCII 32-126)
  for (size_t i = 0; i < len; i++) {
    if (credential[i] < 32 || credential[i] > 126) {
      return false;
    }
  }
  
  return true;
}

// Function to decode URL-encoded strings
String urlDecode(String str) {
  String decoded = "";
  char a, b;
  int len = str.length();
  for (int i = 0; i < len; i++) {
    if (str[i] == '%') {
      if (i + 2 < len) {
        a = str[i + 1];
        b = str[i + 2];
        if (isxdigit(a) && isxdigit(b)) {
          a = tolower(a);
          b = tolower(b);
          a = (a >= 'a') ? (a - 'a' + 10) : (a - '0');
          b = (b >= 'a') ? (b - 'a' + 10) : (b - '0');
          decoded += (char)((a << 4) | b);
          i += 2;
        } else {
          // Invalid encoding, skip
          decoded += str[i];
        }
      } else {
        // Not enough characters for encoding, skip
        decoded += str[i];
      }
    } else if (str[i] == '+') {
      decoded += ' ';
    } else {
      decoded += str[i];
    }
  }
  return decoded;
}

// Function to connect to WiFi using stored credentials (Slave or Master Role)
void connectToWiFi() {
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(storedSSID);
  digitalWrite(LED_BUILTIN, HIGH); // Indicate connection attempt

  WiFi.begin(storedSSID, storedPass);
  Serial.println("WiFi connection initiated.");

  int attempts = 0;
  const int maxAttempts = 20; // Total wait time: 10 seconds
  bool connected = false;

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print("Attempt ");
    Serial.print(attempts + 1);
    Serial.println(": Connecting...");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nSuccessfully connected to WiFi!");
    Serial.print("Assigned IP Address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_BUILTIN, LOW); // Connection successful
    // Start server or perform main tasks if needed
  } else {
    Serial.println("\nFailed to connect to WiFi.");
    digitalWrite(LED_BUILTIN, LOW); // Connection failed
    // Optionally, attempt to start AP mode if WiFi connection fails
    Serial.println("Attempting to start Access Point mode...");
    startAccessPoint(MASTER_SSID, MASTER_PASSWORD);
  }
}

// Function to retrieve credentials from Master (Slave Role)
bool retrieveCredentialsFromMaster() {
  Serial.println("Attempting to retrieve credentials from Master...");

  // Define the master's IP address
  String masterIP = "192.168.4.1"; // Default AP IP for Arduino Uno R4 WiFi

  // Send HTTP GET request to /get_credentials
  bool success = sendGetCredentialsRequest(masterIP);

  if (success) {
    Serial.println("Credentials retrieved successfully from Master.");
    // Attempt to connect to the retrieved WiFi network
    connectToWiFi();
    return true;
  } else {
    Serial.println("Failed to retrieve credentials from Master.");
    // Continue in Standby Mode and retry in the loop
    Serial.println("Will continue standby mode and retry in next cycle.");
    return false; // Return false instead of recursive call
  }
}

// Function to send GET request to Master and retrieve credentials (Slave Role)
bool sendGetCredentialsRequest(const String& masterIP) {
  WiFiClient client;
  const int httpPort = 80;

  Serial.print("Connecting to Master at ");
  Serial.print(masterIP);
  Serial.print(":");
  Serial.println(httpPort);

  if (!client.connect(masterIP.c_str(), httpPort)) {
    Serial.println("Connection to Master failed.");
    return false;
  }

  // Send HTTP GET request
  String url = "/get_credentials";
  Serial.print("Sending HTTP GET request to ");
  Serial.println(url);
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + masterIP + "\r\n" + // Corrected Host header
               "Connection: close\r\n\r\n");

  // Wait for response
  unsigned long timeout = millis();
  bool credentialsReceived = false;

  while (client.connected() && millis() - timeout < 5000) { // 5-second timeout
    if (client.available()) {
      String line = client.readStringUntil('\n');
      Serial.println("Received from Master AP:");
      Serial.println(line);
      // Check for JSON payload
      if (line.startsWith("{")) {
        // Parse JSON
        StaticJsonDocument<300> doc; // Increased size to accommodate additional fields if needed
        DeserializationError error = deserializeJson(doc, line);
        if (error) {
          Serial.print("deserializeJson() failed: ");
          Serial.println(error.c_str());
          return false;
        }

        String newSSID = doc["ssid"];
        String newPass = doc["pass"];
        String configuredSSID = doc["configured_ssid"]; // To verify configuration
        // uint8_t masterFWVersion = doc["fw_version"] | 0; // Optional: Include if needed

        Serial.print("Received SSID: ");
        Serial.println(newSSID);
        Serial.print("Received Password: ");
        Serial.println(newPass);
        Serial.print("Received Configured SSID: ");
        Serial.println(configuredSSID);

        // Verify that the Master AP is in the configured state by checking SSID
        if (!configuredSSID.equals(CONFIGURED_MASTER_SSID)) {
          Serial.println("Master AP is not in the configured state.");
          return false;
        }

        // Validate credentials
        if (isValidCredential(newSSID.c_str(), true) && isValidCredential(newPass.c_str(), false)) {
          // Save to EEPROM
          Serial.println("Saving new credentials to EEPROM...");
          newSSID.toCharArray(storedSSID, sizeof(storedSSID));
          newPass.toCharArray(storedPass, sizeof(storedPass));
          EEPROM.put(SSID_ADDR, storedSSID);
          EEPROM.put(PASS_ADDR, storedPass);
          EEPROM.update(VALID_FLAG_ADDR, 1);
          // EEPROM.commit(); // Removed as EEPROM.commit() does not exist in standard Arduino EEPROM
          Serial.println("Credentials saved to EEPROM.");
          credentialsValid = true;
          credentialsReceived = true;
          break;
        } else {
          Serial.println("Received invalid credentials.");
          return false;
        }
      }
    }
    delay(100);
  }

  if (credentialsReceived) {
    Serial.println("WiFi credentials retrieved and stored successfully.");
    client.stop();
    return true;
  } else {
    Serial.println("No response received from Master within timeout.");
    client.stop();
    return false;
  }
}

// Function to enter Standby Mode
void enterStandbyMode() {
  Serial.println("Entering Standby Mode. Will periodically check for Configured Master AP.");
  standbyLastCheckTime = millis();
  // Optionally, turn off AP or keep it active depending on requirements
  // For example, turn off AP:
  // WiFi.disconnect(true); // Disconnect and disable AP
  // WiFi.mode(WIFI_STA); // Set to Station mode if needed
}
