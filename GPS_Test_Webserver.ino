#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include "page_index.h"
Preferences prefs;


// ====== WiFi settings ======
String wifiSsid = "";
String wifiPass = "";


// Static IP for STA mode (your router network)
IPAddress sta_local_IP(192, 168, 178, 50);
IPAddress sta_gateway(192, 168, 178, 1);
IPAddress sta_subnet(255, 255, 255, 0);
IPAddress sta_primaryDNS(192, 168, 178, 1);

// ====== Fallback AP settings ======
const char* AP_SSID = "ESP32-GPS";
const char* AP_PASS = "";   // min 8 chars for WPA2, or set to "" for open AP

IPAddress ap_local_IP(192, 168, 4, 1);
IPAddress ap_gateway(192, 168, 4, 1);
IPAddress ap_subnet(255, 255, 255, 0);

// ====== GPS / UART settings ======
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);

// GPS TX -> ESP32 RX, GPS RX -> ESP32 TX
static const int GPS_RX_PIN = D3; // This goes to the GPS TX Pin
static const int GPS_TX_PIN = D4; // This goes to the GPS RX Pin

static const uint32_t GPS_BAUD = 9600;


//GLOBALS
int ggaFixQuality = 0;
String lastGGALine = "";
unsigned long lastGGALogTime = 0;

// ====== Web server ======
WebServer server(80);

// Tracks last time we received any GPS byte
unsigned long lastGPSTime = 0;

// Track current WiFi mode for display/debug
enum NetMode { MODE_STA, MODE_AP };
NetMode netMode = MODE_STA;

// Helper: JSON escaping for strings (minimal)
String jsonString(const String& s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  out += "\"";
  return out;
}

String makeJSON() {
  bool gpsConnected = (millis() - lastGPSTime) < 5000;

  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;



bool hasFix =
  gpsConnected &&
(ggaFixQuality > 0);

  double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
  double lon = gps.location.isValid() ? gps.location.lng() : 0.0;
  double spd = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  double alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;

  String status;
  if (!gpsConnected) status = "NO_GPS_DATA";
  else if (!hasFix)  status = "WAITING_FOR_FIX";
  else               status = "FIX_OK";

  String j = "{";
  j += "\"status\":" + jsonString(status) + ",";
  j += "\"gpsConnected\":" + String(gpsConnected ? "true" : "false") + ",";
  j += "\"hasFix\":" + String(hasFix ? "true" : "false") + ",";
  j += "\"satellites\":" + String(sats) + ",";
  j += "\"lat\":" + String(lat, 7) + ",";
  j += "\"lon\":" + String(lon, 7) + ",";
  j += "\"speedKmph\":" + String(spd, 2) + ",";
  j += "\"altMeters\":" + String(alt, 2) + ",";
  j += "\"hdop\":" + String(gps.hdop.isValid() ? gps.hdop.hdop() : 0.0, 2) + ",";
  j += "\"ageMs\":" + String(gps.location.isValid() ? (long)gps.location.age() : -1) + ",";
  j += "\"charsProcessed\":" + String(gps.charsProcessed()) + ",";
  j += "\"netMode\":" + jsonString(netMode == MODE_STA ? "STA" : "AP") + ",";
  j += "\"ip\":" + jsonString(netMode == MODE_STA ? WiFi.localIP().toString(): WiFi.softAPIP().toString())  + ",";
  j += "\"fixQuality\":" + String(ggaFixQuality);

  j += "}";
  return j;
}



void handleRoot() {
  server.send(200, "text/html", PAGE_INDEX);
}
void handleJSON() {
  server.send(200, "application/json", makeJSON());
}
void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void handleWifiSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing ssid");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass"); // can be empty

  ssid.trim();

  if (ssid.length() < 1) {
    server.send(400, "text/plain", "SSID must not be empty");
    return;
  }

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);

  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(500);
  ESP.restart();
}

void updateNMEASniffer(char c) {
  static char line[120];
  static uint8_t idx = 0;

  if (c == '\n') {
    line[idx] = 0;
    idx = 0;

    // Look specifically for GGA sentences
    if (strstr(line, "GGA,")) {
      lastGGALine = String(line);
    }
    return;
  }

  if (c != '\r' && idx < sizeof(line) - 1) {
    line[idx++] = c;
  }
}
void updateFixQualityFromNMEA(char c) {
  static char line[120];
  static uint8_t idx = 0;

  if (c == '\n') {
    line[idx] = 0;
    idx = 0;

    if (strstr(line, "GGA,")) {
      int field = 0;
      char *p = line;
      while (*p) {
        if (*p == ',') field++;
        if (field == 6) {      // Fix quality field
          ggaFixQuality = atoi(p + 1);
          break;
        }
        p++;
      }
    }
    return;
  }

  if (c != '\r' && idx < sizeof(line) - 1) {
    line[idx++] = c;
  }
}



void readGPS() {
  unsigned long start = millis();
  while (millis() - start < 10) {   // read for up to 10 ms
    while (GPSSerial.available()) {
      char c = GPSSerial.read();
      updateFixQualityFromNMEA(c);
      updateNMEASniffer(c);
      gps.encode(c);
      lastGPSTime = millis();
    }
  }
    // Print raw GGA once per second
  if (millis() - lastGGALogTime > 1000 && lastGGALine.length() > 0) {
    lastGGALogTime = millis();
    Serial.print("GGA: ");
    Serial.println(lastGGALine);
  }
}





void debugGPSOncePerSecond() {
  static unsigned long last = 0;
  static uint32_t lastChars = 0;

  if (millis() - last >= 1000) {
    last = millis();
    uint32_t chars = gps.charsProcessed();
    Serial.print("bytes/s=");
    Serial.print(chars - lastChars);
    Serial.print(" age(ms)=");
    Serial.print(millis() - lastGPSTime);
    Serial.print(" locValid=");
    Serial.print(gps.location.isValid());
    Serial.print(" sats=");
    Serial.print(gps.satellites.isValid() ? gps.satellites.value() : -1);
    Serial.print(" hdop=");
    Serial.println(gps.hdop.isValid() ? gps.hdop.hdop() : -1);
    lastChars = chars;
  }
}



bool connectSTAWithTimeout(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);

  if (!WiFi.config(sta_local_IP, sta_gateway, sta_subnet, sta_primaryDNS)) {
    Serial.println("STA Failed to configure static IP");
  }

if(wifiSsid.length() == 0){
  return false;
}
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  Serial.print("Connecting to WiFi");

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  return WiFi.status() == WL_CONNECTED;
}

void startFallbackAP() {
  netMode = MODE_AP;

  // Switch to AP mode and set fixed IP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_local_IP, ap_gateway, ap_subnet);

  bool ok;
  if (strlen(AP_PASS) >= 8) ok = WiFi.softAP(AP_SSID, AP_PASS);
  else ok = WiFi.softAP(AP_SSID); // open AP if pass too short/empty

  Serial.print("AP start: ");
  Serial.println(ok ? "OK" : "FAILED");

  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  prefs.begin("wifi", false);
wifiSsid = prefs.getString("ssid", "");
wifiPass = prefs.getString("pass", "");


  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // Start GPS UART
  GPSSerial.setRxBufferSize(2048); // helps with bursts
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Try STA, else fallback to AP
  if (connectSTAWithTimeout(20000)) {
    netMode = MODE_STA;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Open: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi NOT connected. Starting fallback AP...");
    startFallbackAP();
    Serial.print("Open: http://");
    Serial.println(WiFi.softAPIP());
  }

  // Start server
  server.on("/", handleRoot);
  server.on("/json", handleJSON);
  server.on("/wifi", HTTP_POST, handleWifiSave);

  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("HTTP server started");
}

void loop() {
  readGPS();
  server.handleClient();
//debugGPSOncePerSecond();
  // LED status
 bool gpsConnected = (millis() - lastGPSTime) < 5000;

bool hasFix =
  gpsConnected &&
  gps.location.isValid() &&
  gps.location.age() < 5000;

  if (!gpsConnected) {
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_BUILTIN, LOW);
  } else if (!hasFix) {
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, LOW);
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_BUILTIN, HIGH);
  }
}
