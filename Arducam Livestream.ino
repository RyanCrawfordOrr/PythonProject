/******************************************************
 * Arduino Code: UNO R4 WiFi + Arducam + Handshake
 * with Built-In LED Matrix only (3 “lights” or zones)
 ******************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>         // For UNO R4 WiFi
#include <WiFiUdp.h>      // For UDP broadcasting
#include "Arducam_Mega.h" // Arducam Mega library v2.0.6

// For the built-in LED matrix
#include <Arduino_LED_Matrix.h>

/******************************************************
 * WiFi Credentials
 ******************************************************/
const char* ssid     = "NSA";
const char* password = "windytable153";

/******************************************************
 * Camera Pin
 ******************************************************/
#define CAM_CS_PIN 10

/******************************************************
 * Arducam Object
 ******************************************************/
Arducam_Mega camera(CAM_CS_PIN);

/******************************************************
 * MJPEG Web Server
 ******************************************************/
WiFiServer server(80); // HTTP server on port 80

/******************************************************
 * UDP Handshake
 ******************************************************/
WiFiUDP udp;
const unsigned int HANDSHAKE_PORT = 4210; 
unsigned long lastBroadcastMillis = 0;
const unsigned long BROADCAST_INTERVAL = 3000; // 3 seconds

String pcIP = "";

/******************************************************
 * Provide SPI CS Pin HAL for Arducam
 ******************************************************/
extern "C" {
  void arducamCsOutputMode(void) {
    pinMode(CAM_CS_PIN, OUTPUT);
    digitalWrite(CAM_CS_PIN, HIGH);
  }
  void arducamSpiCsPinLow(void) {
    digitalWrite(CAM_CS_PIN, LOW);
  }
  void arducamSpiCsPinHigh(void) {
    digitalWrite(CAM_CS_PIN, HIGH);
  }
}

/******************************************************
 * ArduinoLEDMatrix
 ******************************************************/
ArduinoLEDMatrix matrix;

/******************************************************
 * We'll treat the 12x8 matrix as 96 bits:
 *   - row major: row=0..7, col=0..11
 *   - currentPattern[row*12 + col]
 * '1' => ON, '0' => OFF
 ******************************************************/
static uint8_t currentPattern[96] = {0};

/******************************************************
 * Our 3 "lights" or "zones":
 *  Zone 0 (columns 0..3)  -> Setup
 *  Zone 1 (columns 4..7)  -> WiFi
 *  Zone 2 (columns 8..11) -> Broadcast
 ******************************************************/

// Helper: set a single pixel on/off
void setPixel(int row, int col, bool on)
{
  if(row < 0 || row > 7 || col < 0 || col > 11) return;
  int idx = row * 12 + col;
  currentPattern[idx] = on ? 1 : 0;
}

// Helper: set entire zone to on/off
// zone=0 => columns 0..3, zone=1 => cols 4..7, zone=2 => cols 8..11
void setZone(int zone, bool on)
{
  int colStart = zone * 4;       // 4 columns each
  int colEnd   = zone * 4 + 3;   // inclusive
  for(int row=0; row<8; row++){
    for(int col=colStart; col<=colEnd; col++){
      setPixel(row, col, on);
    }
  }
  // Apply to matrix
  matrix.loadPixels(currentPattern, 96);
}

// Helper: blink a zone certain times
void blinkZone(int zone, int times, int msOnOff)
{
  for(int i=0; i<times; i++){
    setZone(zone, true);
    delay(msOnOff);
    setZone(zone, false);
    delay(msOnOff);
  }
}

/******************************************************
 * Setup states:
 *  - Zone0 => Setup
 *  - Zone1 => WiFi
 *  - Zone2 => Broadcast
 ******************************************************/
#define Z_SETUP     0
#define Z_WIFI      1
#define Z_BROADCAST 2

/******************************************************
 * setup()
 ******************************************************/
void setup() 
{
  Serial.begin(115200);
  while(!Serial){}

  // Initialize matrix
  matrix.begin();
  // All off initially
  memset(currentPattern, 0, 96);
  matrix.loadPixels(currentPattern, 96);

  // Indicate "Setup start" by blinking zone0
  blinkZone(Z_SETUP, 3, 200);

  Serial.println("\n[Arducam + UNO R4 WiFi] Starting...");

  // 1) SPI for Arducam
  SPI.begin();

  // 2) Arducam init
  CamStatus status = camera.begin();
  if(status != CAM_ERR_SUCCESS) {
    Serial.print("Arducam init failed, status=");
    Serial.println(status);
    // Leave Setup zone ON to indicate fail
    setZone(Z_SETUP, true);
  } else {
    Serial.println("Arducam initialized successfully.");

    delay(100);
    Serial.println("Arducam auto-exposure ON");
    Serial.println("Arducam auto-gain ON");
    Serial.println("Arducam Auto-WB ON");
    camera.setAutoExposure(0);      // auto-exposure ON
    camera.setAutoISOSensitive(0);  // auto-gain ON
    camera.setAutoWhiteBalance(0);  // auto-WB ON
    //camera.setEV(CAM_EV_PLUS_2);    // optional "extra bright" compensation
  }

  // 3) WiFi **CHANGE**: Only call WiFi.begin once here
  Serial.print("Connecting to WiFi '");
  Serial.print(ssid);
  Serial.println("'...");
  WiFi.begin(ssid, password);  // **CHANGE** Moved out of loop

  // **CHANGE** Wait for WiFi up to 20s (adjust as desired)
  unsigned long wifiStart = millis();
  const unsigned long wifiTimeout = 20000; // 20 seconds
  while(WiFi.status() != WL_CONNECTED && (millis() - wifiStart < wifiTimeout)) {
    blinkZone(Z_WIFI, 1, 250);
    delay(500);
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connected, IP=");
    Serial.println(WiFi.localIP());
    // Turn WiFi zone on solid
    setZone(Z_WIFI, true);
  } else {
    Serial.println("WiFi connection FAILED. Will keep trying in loop...");
    // Keep zone off to show it’s not connected
    setZone(Z_WIFI, false);
  }

  // 4) Start web server
  server.begin();
  Serial.println("HTTP server started on port 80.");

  // 5) UDP for handshake
  if(udp.begin(HANDSHAKE_PORT)) {
    Serial.print("UDP listening on port ");
    Serial.println(HANDSHAKE_PORT);
  } else {
    Serial.println("UDP begin failed!");
  }

  Serial.println("Setup complete.");
  // Setup zone on solid -> done
  setZone(Z_SETUP, true);
}

/******************************************************
 * loop()
 ******************************************************/
void loop() 
{
  // **ADDED** Re-check WiFi every iteration. If lost, try to reconnect:
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("Lost WiFi or not connected; attempting reconnect...");
    setZone(Z_WIFI, false);

    WiFi.disconnect();
    WiFi.begin(ssid, password);

    // Try 10s this time; if it fails, loop tries again
    unsigned long startAttempt = millis();
    while(WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 10000)) {
      blinkZone(Z_WIFI, 1, 200);
      delay(300);
    }

    if(WiFi.status() == WL_CONNECTED) {
      Serial.println("Reconnected to WiFi.");
      setZone(Z_WIFI, true);
    } else {
      Serial.println("Still not connected after 10s. Will retry again in loop.");
    }
  }

  // Accept new client
  WiFiClient client = server.available();
  if(client) {
    // Indicate HTTP client => blink wifi zone once
    blinkZone(Z_WIFI, 2, 200);
    handleHttpClient(client);
  }

  // broadcast IP every 3s
  if(millis() - lastBroadcastMillis > BROADCAST_INTERVAL) {
    broadcastArduinoIP();
    lastBroadcastMillis = millis();
  }

  // check PC reply
  checkForPCReply();
}

/******************************************************
 * handleHttpClient: MJPEG
 ******************************************************/
void handleHttpClient(WiFiClient &client)
{
  while (client.available()) {
    client.read();
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Connection: close");
  client.println();

  while(client.connected()) {
    unsigned long now = millis();
    if(now - lastBroadcastMillis > BROADCAST_INTERVAL) {
      broadcastArduinoIP();
      lastBroadcastMillis = now;
    }
    if(!captureAndSendFrame(client)) {
      Serial.println("Capture failed or client disconnected.");
      break;
    }
    // delay takes milliseconds as an integer. Using a float causes the delay
    // to truncate to zero. Use a small integer delay instead.
    delay(30);
  }
  
  // Once we exit the loop, either the client disconnected or error occurred
  delay(10);
  client.flush();
  delay(50);
  client.stop();
  Serial.println("HTTP client disconnected.");

  //restart server if client ends upbruptly
  //server.end();
  //delay(50);
  //server.begin();
  
}

/******************************************************
 * captureAndSendFrame
 ******************************************************/
bool captureAndSendFrame(WiFiClient &client)
{
  CamStatus stat = camera.takePicture(CAM_IMAGE_MODE_QVGA, CAM_IMAGE_PIX_FMT_JPG);
  if(stat != CAM_ERR_SUCCESS) {
    Serial.print("takePicture failed, status=");
    Serial.println(stat);
    // Attempt re-init
    CamStatus reStat = camera.begin();
    Serial.print("camera has failed to take picture => status=");
    Serial.println(reStat);
    delay(400);

    stat = camera.takePicture(CAM_IMAGE_MODE_QVGA, CAM_IMAGE_PIX_FMT_JPG);
    if(stat != CAM_ERR_SUCCESS) {
      Serial.println("Still failing after re-init. Giving up.");
      return false;
    }
  }
  uint32_t length = camera.getTotalLength();
  if(length == 0) {
    Serial.println("Error: Picture length=0.");
    return false;
  }

  client.println("--frame");
  client.println("Content-Type: image/jpeg");
  client.print("Content-Length: ");
  client.println(length);
  client.println();

  const uint16_t chunkSize = 128;
  uint8_t buffer[chunkSize];
  uint32_t bytesRemaining = length;

  while(bytesRemaining > 0) {
    uint16_t toRead = (bytesRemaining < chunkSize) ? bytesRemaining : chunkSize;
    uint8_t bytesRead = camera.readBuff(buffer, (uint8_t)toRead);
    if(bytesRead == 0) {
      Serial.println("readBuff returned 0. Breaking...");
      break;
    }
    client.write(buffer, bytesRead);
    bytesRemaining -= bytesRead;
  }
  client.println();
  client.flush();
  return true;
}

/******************************************************
 * broadcastArduinoIP
 ******************************************************/
void broadcastArduinoIP()
{
  // blink broadcast zone quickly
  blinkZone(Z_BROADCAST, 1, 150);

  IPAddress ardIP = WiFi.localIP();
  String msg = "UNO_R4 IP:" + ardIP.toString();

  IPAddress broadcast(255,255,255,255);
  udp.beginPacket(broadcast, HANDSHAKE_PORT);
  udp.write(msg.c_str());
  udp.endPacket();

  Serial.print("[Broadcast] ");
  Serial.println(msg);
}

/******************************************************
 * checkForPCReply
 ******************************************************/
void checkForPCReply()
{
  int packetSize = udp.parsePacket();
  if(packetSize > 0) {
    char buffer[128];
    int len = udp.read(buffer, sizeof(buffer)-1);
    if(len>0) {
      buffer[len] = '\0';
    }
    String message = String(buffer);

    if(message.startsWith("PC IP:")) {
      String discoveredPcIP = message.substring(6);
      discoveredPcIP.trim();
      if(discoveredPcIP.length()>0) {
        pcIP = discoveredPcIP;
        Serial.print("[Handshake] Discovered PC IP: ");
        Serial.println(pcIP);
      }
    }
  }
}