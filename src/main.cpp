/*
 * Smart Poultry Monitoring and Control System firmware.
 * Target: ESP32 DevKit v1, Arduino framework, PlatformIO.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <esp_system.h>
#include "webpage.h"

// ── Pin definitions ──
static const uint8_t PIN_DHT     = 4;
static const uint8_t PIN_TRIG    = 5;
static const uint8_t PIN_ECHO    = 18; // HC-SR04 ECHO needs a 5 V-to-3.3 V divider.
static const uint8_t PIN_SERVO   = 13; // Use a separate regulated 5 V supply; share GND.
static const uint8_t PIN_FAN     = 19;  // Relay / LED
static const uint8_t PIN_HEAT    = 21;  // Relay / LED
static const uint8_t PIN_BUZZER  = 22;

// ── Sensor type differs between physical hardware and Wokwi. ──
#ifdef ENV_WOKWI
static const uint8_t DHT_TYPE = DHT22;
#else
static const uint8_t DHT_TYPE = DHT11;
#endif

// ── Default thresholds ──
static const float    DEF_FAN_ON    = 31.0f;
static const float    DEF_HEAT_ON   = 27.0f;
static const float    DEF_HYSTER    = 0.5f;
static const uint8_t  DEF_LOW_PCT   = 15;
static const uint32_t DEF_AUTO_MIN  = 240;
static const float    DEF_DIST_EMPTY= 20.0f;
static const float    DEF_DIST_FULL = 3.0f;

// ── Timing constants ──
static const uint32_t DHT_INTERVAL   = 2000;
static const uint32_t SENSOR_TIMEOUT = 10000;
static const uint32_t BUZZ_INTERVAL  = 400;
static const uint32_t ULTRA_SAMPLES  = 5;
static const uint32_t SERVO_HOLD_MS  = 600;

// ── Wi-Fi Credentials ──
static const char* SETUP_AP_SSID = "SmartPoultry_Setup";
static const char* WIFI_NAMESPACE = "poultry";
static const char* WIFI_SSID_KEY = "wifiSsid";
static const char* WIFI_PASS_KEY = "wifiPass";

// ── Objects & Globals ──
DHT        dht(PIN_DHT, DHT_TYPE);
Servo      servo;
WebServer  server(80);
Preferences prefs;
DNSServer  dnsServer;

float    g_temp       = NAN;
float    g_hum        = NAN;
float    g_feedPct    = 0.0f;
bool     g_fanOn      = false;
bool     g_heatOn     = false;
bool     g_buzzerOn   = false;
bool     g_feedValid  = false;
bool     g_wifiProvisioning = false;
bool     g_hasWifiCredentials = false;
bool     g_buzzerTestActive = false;
bool     g_stationConnected = false;
bool     g_mdnsStarted = false;
bool     g_wifiRebootPending = false;
uint8_t  buzzerMode = 0;
uint32_t t_buzzerTestStart = 0;
uint32_t t_wifiReconnect   = 0;
uint32_t t_wifiReboot      = 0;
char     serialLine[64] = {};
uint8_t  serialLineLength = 0;
bool     serialLineOverflow = false;

float    cfg_fanOn    = DEF_FAN_ON;
float    cfg_heatOn   = DEF_HEAT_ON;
uint8_t  cfg_lowPct   = DEF_LOW_PCT;
uint32_t cfg_autoMin  = DEF_AUTO_MIN;
float    cfg_distEmpty= DEF_DIST_EMPTY;
float    cfg_distFull = DEF_DIST_FULL;

uint32_t t_dhtLast    = 0;
uint32_t t_statusLast = 0;
uint32_t t_dhtOkLast  = 0;
uint32_t t_feedOkLast = 0;
uint32_t t_buzzLast   = 0;
uint32_t t_autoFeed   = 0;
uint32_t t_servoStart = 0;

enum ServoState { SERVO_IDLE, SERVO_OPEN, SERVO_CLOSE };
ServoState servoState = SERVO_IDLE;

inline void relaySet(uint8_t pin, bool on) {
#ifdef ENV_HARDWARE
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

void loadPrefs() {
  if (!prefs.begin("poultry", true)) return;
  cfg_fanOn    = prefs.getFloat("fanOn",    DEF_FAN_ON);
  cfg_heatOn   = prefs.getFloat("heatOn",   DEF_HEAT_ON);
  cfg_lowPct   = prefs.getUChar("lowPct",   DEF_LOW_PCT);
  cfg_autoMin  = prefs.getUInt ("autoMin",  DEF_AUTO_MIN);
  cfg_distEmpty= prefs.getFloat("distEmpty",DEF_DIST_EMPTY);
  cfg_distFull = prefs.getFloat("distFull", DEF_DIST_FULL);
  prefs.end();

  if (!isfinite(cfg_fanOn) || cfg_fanOn < -20.0f || cfg_fanOn > 80.0f ||
      !isfinite(cfg_heatOn) || cfg_heatOn < -20.0f || cfg_heatOn > 80.0f ||
      cfg_fanOn <= cfg_heatOn + 1.0f) {
    cfg_fanOn = DEF_FAN_ON;
    cfg_heatOn = DEF_HEAT_ON;
  }
  if (cfg_lowPct > 100) cfg_lowPct = DEF_LOW_PCT;
  if (cfg_autoMin > 1440) cfg_autoMin = DEF_AUTO_MIN;
  if (!isfinite(cfg_distEmpty) || !isfinite(cfg_distFull) ||
      cfg_distFull < 2.0f || cfg_distFull > 400.0f ||
      cfg_distEmpty > 400.0f ||
      cfg_distEmpty <= cfg_distFull + 0.5f) {
    cfg_distEmpty = DEF_DIST_EMPTY;
    cfg_distFull = DEF_DIST_FULL;
  }
}

bool savePrefs() {
  if (!prefs.begin("poultry", false)) return false;
  bool saved = true;
  saved = (prefs.putFloat("fanOn", cfg_fanOn) == sizeof(float)) && saved;
  saved = (prefs.putFloat("heatOn", cfg_heatOn) == sizeof(float)) && saved;
  saved = (prefs.putUChar("lowPct", cfg_lowPct) == sizeof(cfg_lowPct)) && saved;
  saved = (prefs.putUInt("autoMin", cfg_autoMin) == sizeof(cfg_autoMin)) && saved;
  saved = (prefs.putFloat("distEmpty", cfg_distEmpty) == sizeof(float)) && saved;
  saved = (prefs.putFloat("distFull", cfg_distFull) == sizeof(float)) && saved;
  prefs.end();
  return saved;
}

bool startServo() {
  if (servoState != SERVO_IDLE) return false;
  Serial.println("[ACTION] Dispensing feed...");
  servo.write(90);
  servoState   = SERVO_OPEN;
  t_servoStart = millis();
  return true;
}

float measureDistanceCm(float tempC) {
  if (!isfinite(tempC)) tempC = 25.0f;
  float total = 0.0f;
  uint8_t valid = 0;
  float soundCm = (331.4f + 0.606f * tempC) * 100.0f / 1e6f;
  for (uint8_t i = 0; i < ULTRA_SAMPLES; i++) {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long dur = pulseIn(PIN_ECHO, HIGH, 30000UL);
    if (dur > 0) {
      total += (dur * soundCm) / 2.0f;
      valid++;
    }
    delay(10);
  }
  return (valid > 0) ? (total / valid) : -1.0f;
}

float calcFeedPct(float dist) {
  if (dist < 0) return g_feedPct;
  float range = cfg_distEmpty - cfg_distFull;
  if (range < 0.5f) return 0.0f;
  float pct = (cfg_distEmpty - dist) / range * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

void updateDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isfinite(t) && isfinite(h)) {
    g_temp = t;
    g_hum  = h;
    t_dhtOkLast = millis();
  }
}

void applyClimate() {
  uint32_t now = millis();
  bool sensorOk = isfinite(g_temp) && (now - t_dhtOkLast < SENSOR_TIMEOUT);
  if (!sensorOk) {
    g_fanOn  = false;
    g_heatOn = false;
  } else {
    if (!g_fanOn  && g_temp > cfg_fanOn)               g_fanOn  = true;
    if ( g_fanOn  && g_temp < cfg_fanOn - DEF_HYSTER)  g_fanOn  = false;
    if (!g_heatOn && g_temp < cfg_heatOn)              g_heatOn = true;
    if ( g_heatOn && g_temp > cfg_heatOn + DEF_HYSTER) g_heatOn = false;
  }
  relaySet(PIN_FAN,  g_fanOn);
  relaySet(PIN_HEAT, g_heatOn);
}

void printStatus() {
  char temp[16];
  char humidity[16];
  char feed[16];
  uint32_t now = millis();
  bool temperatureFresh = isfinite(g_temp) && now - t_dhtOkLast < SENSOR_TIMEOUT;
  bool feedFresh = g_feedValid && now - t_feedOkLast < SENSOR_TIMEOUT;
  if (temperatureFresh) snprintf(temp, sizeof(temp), "%.1f", g_temp);
  else snprintf(temp, sizeof(temp), "N/A");
  if (temperatureFresh && isfinite(g_hum)) snprintf(humidity, sizeof(humidity), "%.1f", g_hum);
  else snprintf(humidity, sizeof(humidity), "N/A");
  if (feedFresh) snprintf(feed, sizeof(feed), "%.0f", g_feedPct);
  else snprintf(feed, sizeof(feed), "N/A");

  const char* servoName = "IDLE";
  if (servoState == SERVO_OPEN) servoName = "OPEN";
  else if (servoState == SERVO_CLOSE) servoName = "CLOSE";
  Serial.printf("[STATUS] Temp: %s C | Hum: %s %% | Feed: %s %% | Fan: %s | Heat: %s | Buzz: %s | Servo: %s\n",
                temp, humidity, feed,
                g_fanOn ? "ON" : "OFF",
                g_heatOn ? "ON" : "OFF",
                g_buzzerOn ? "ON" : "OFF",
                servoName);
}

void updateBuzzer(uint32_t now) {
  bool testActive = g_buzzerTestActive &&
                    now - t_buzzerTestStart < 2000UL;
  if (g_buzzerTestActive && !testActive) g_buzzerTestActive = false;
  bool feedAlarm = g_feedValid && now - t_feedOkLast < SENSOR_TIMEOUT &&
                   g_feedPct < (float)cfg_lowPct;
  uint8_t requestedMode = testActive ? 2 : (feedAlarm ? 1 : 0);

  if (requestedMode != buzzerMode) {
    buzzerMode = requestedMode;
    g_buzzerOn = buzzerMode != 0;
    t_buzzLast = now;
  } else if (buzzerMode != 0 && now - t_buzzLast >= BUZZ_INTERVAL) {
    g_buzzerOn = !g_buzzerOn;
    t_buzzLast = now;
  } else if (buzzerMode == 0) {
    g_buzzerOn = false;
  }
  digitalWrite(PIN_BUZZER, g_buzzerOn ? HIGH : LOW);
}

void printDiagnosticHelp() {
  Serial.println("Commands: test servo | test buzzer | test fan | test heat | reset wifi | status | help");
}

void handleSerialCommand(char* command) {
  while (*command == ' ' || *command == '\t') command++;
  size_t length = strlen(command);
  while (length > 0 && (command[length - 1] == ' ' || command[length - 1] == '\t')) {
    command[--length] = '\0';
  }
  for (size_t i = 0; i < length; i++) command[i] = (char)tolower((unsigned char)command[i]);

  if (strcmp(command, "test servo") == 0) {
    if (!startServo()) Serial.println("[DIAG] Servo is busy.");
  } else if (strcmp(command, "test buzzer") == 0) {
    g_buzzerTestActive = true;
    t_buzzerTestStart = millis();
    updateBuzzer(t_buzzerTestStart);
    Serial.println("[DIAG] Buzzer test started for 2 seconds.");
  } else if (strcmp(command, "test fan") == 0) {
    g_fanOn = !g_fanOn;
    relaySet(PIN_FAN, g_fanOn);
    Serial.printf("[DIAG] Fan manually set %s; climate control resumes at next sensor update.\n",
                  g_fanOn ? "ON" : "OFF");
  } else if (strcmp(command, "test heat") == 0) {
    g_heatOn = !g_heatOn;
    relaySet(PIN_HEAT, g_heatOn);
    Serial.printf("[DIAG] Heater manually set %s; climate control resumes at next sensor update.\n",
                  g_heatOn ? "ON" : "OFF");
  } else if (strcmp(command, "reset wifi") == 0) {
    if (!prefs.begin(WIFI_NAMESPACE, false)) {
      Serial.println("[DIAG] ERROR: Could not open Wi-Fi settings in flash.");
      return;
    }
    bool removed = true;
    if (prefs.isKey(WIFI_SSID_KEY)) removed = prefs.remove(WIFI_SSID_KEY) && removed;
    if (prefs.isKey(WIFI_PASS_KEY)) removed = prefs.remove(WIFI_PASS_KEY) && removed;
    prefs.end();
    if (!removed) {
      Serial.println("[DIAG] ERROR: Could not clear Wi-Fi credentials.");
      return;
    }
    Serial.println("[DIAG] Wi-Fi credentials cleared; rebooting into setup mode.");
    Serial.flush();
    ESP.restart();
  } else if (strcmp(command, "status") == 0) {
    printStatus();
  } else if (strcmp(command, "help") == 0) {
    printDiagnosticHelp();
  } else if (*command != '\0') {
    Serial.println("[DIAG] Unknown command. Type help.");
  }
}

void pollSerialCommands() {
  uint8_t bytesRead = 0;
  while (Serial.available() > 0 && bytesRead < 32) {
    bytesRead++;
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialLineOverflow) {
        Serial.println("[DIAG] ERROR: Command too long.");
      } else {
        serialLine[serialLineLength] = '\0';
        handleSerialCommand(serialLine);
      }
      serialLineLength = 0;
      serialLineOverflow = false;
    } else if (!serialLineOverflow) {
      if (serialLineLength < sizeof(serialLine) - 1) {
        serialLine[serialLineLength++] = c;
      } else {
        serialLineOverflow = true;
      }
    }
  }
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  if (g_wifiProvisioning) {
    static const char WIFI_SETUP_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Poultry Wi-Fi Setup</title><style>
body{font:16px system-ui;max-width:28rem;margin:3rem auto;padding:1rem;background:#10131a;color:#eee}
label{display:block;margin-top:1rem}input{box-sizing:border-box;width:100%;padding:.7rem;margin-top:.3rem}
button{margin-top:1rem;padding:.7rem 1rem;background:#f59e0b;border:0;border-radius:.4rem;font-weight:bold}
</style><h1>Connect Smart Poultry to Wi-Fi</h1>
<p>Enter the Wi-Fi network credentials. The device will restart and keep its setup access point if it cannot connect.</p>
<form method="post" action="/wifi">
<label>Network name (SSID)<input name="ssid" maxlength="32" required autocomplete="off"></label>
<label>Wi-Fi password<input name="password" type="password" maxlength="63" autocomplete="new-password"></label>
<button type="submit">Save and connect</button></form></html>
)rawliteral";
    server.send_P(200, "text/html", WIFI_SETUP_HTML);
  } else {
    server.send_P(200, "text/html", INDEX_HTML);
  }
}

bool parseFloatArg(const char* name, float& value) {
  if (!server.hasArg(name)) return false;
  String raw = server.arg(name);
  raw.trim();
  char* end = nullptr;
  float parsed = strtof(raw.c_str(), &end);
  if (raw.isEmpty() || end == raw.c_str() || *end != '\0' || !isfinite(parsed)) return false;
  value = parsed;
  return true;
}

bool parseIntegerArg(const char* name, long minValue, long maxValue, long& value) {
  if (!server.hasArg(name)) return false;
  String raw = server.arg(name);
  raw.trim();
  char* end = nullptr;
  long parsed = strtol(raw.c_str(), &end, 10);
  if (raw.isEmpty() || end == raw.c_str() || *end != '\0' ||
      parsed < minValue || parsed > maxValue) return false;
  value = parsed;
  return true;
}

void handleApiData() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  char temp[16];
  char humidity[16];
  char feed[16];
  uint32_t now = millis();
  bool temperatureFresh = isfinite(g_temp) && now - t_dhtOkLast < SENSOR_TIMEOUT;
  bool feedFresh = g_feedValid && now - t_feedOkLast < SENSOR_TIMEOUT;
  if (temperatureFresh) snprintf(temp, sizeof(temp), "%.1f", g_temp);
  else snprintf(temp, sizeof(temp), "null");
  if (temperatureFresh && isfinite(g_hum)) snprintf(humidity, sizeof(humidity), "%.1f", g_hum);
  else snprintf(humidity, sizeof(humidity), "null");
  if (feedFresh)
    snprintf(feed, sizeof(feed), "%.0f", g_feedPct);
  else snprintf(feed, sizeof(feed), "null");
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%s,\"hum\":%s,\"feed\":%s,\"fan\":%s,\"heat\":%s,\"buzzer\":%s}",
    temp, humidity, feed,
    g_fanOn    ? "true" : "false",
    g_heatOn   ? "true" : "false",
    g_buzzerOn ? "true" : "false"
  );
  server.send(200, "application/json", buf);
}

void handleApiFeed() {
  if (!startServo()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"Feeder is busy\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiConfig() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  if (server.method() == HTTP_POST) {
    float fan = cfg_fanOn;
    float heat = cfg_heatOn;
    long low = cfg_lowPct;
    long autoMin = cfg_autoMin;
    bool valid = true;

    if (server.hasArg("fan")) {
      valid = parseFloatArg("fan", fan) && fan >= -20.0f && fan <= 80.0f;
    }
    if (server.hasArg("heat")) {
      valid = parseFloatArg("heat", heat) && heat >= -20.0f && heat <= 80.0f && valid;
    }
    if ((server.hasArg("fan") || server.hasArg("heat")) && fan <= heat + 1.0f) {
      valid = false;
    }
    if (server.hasArg("low")) {
      valid = parseIntegerArg("low", 0, 100, low) && valid;
    }
    if (server.hasArg("auto")) {
      valid = parseIntegerArg("auto", 0, 1440, autoMin) && valid;
    }
    if (!valid) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Check ranges and keep fan threshold more than 1 C above heat threshold\"}");
      return;
    }

    bool changed = fan != cfg_fanOn || heat != cfg_heatOn ||
                   low != cfg_lowPct || autoMin != cfg_autoMin;
    if (changed) {
      float oldFan = cfg_fanOn;
      float oldHeat = cfg_heatOn;
      uint8_t oldLow = cfg_lowPct;
      uint32_t oldAutoMin = cfg_autoMin;
      cfg_fanOn = fan;
      cfg_heatOn = heat;
      cfg_lowPct = (uint8_t)low;
      cfg_autoMin = (uint32_t)autoMin;

      if (!savePrefs()) {
        cfg_fanOn = oldFan;
        cfg_heatOn = oldHeat;
        cfg_lowPct = oldLow;
        cfg_autoMin = oldAutoMin;
        server.send(500, "application/json",
                    "{\"ok\":false,\"error\":\"Could not save settings to flash\"}");
        return;
      }
    }
  }

  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"fan\":%.1f,\"heat\":%.1f,\"low\":%u,\"auto\":%u}",
    cfg_fanOn, cfg_heatOn, (unsigned)cfg_lowPct, (unsigned)cfg_autoMin);
  server.send(200, "application/json", buf);
}

void handleApiCalibrate() {
  String p = server.arg("p");
  if (p != "full" && p != "empty") {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  float dist = measureDistanceCm(isnan(g_temp) ? 25.0f : g_temp);
  if (dist < 2.0f || dist > 400.0f || !isfinite(dist) ||
      (p == "full" && dist >= cfg_distEmpty - 0.5f) ||
      (p == "empty" && dist <= cfg_distFull + 0.5f)) {
    server.send(503, "application/json", "{\"ok\":false}");
    return;
  }
  float oldDistFull = cfg_distFull;
  float oldDistEmpty = cfg_distEmpty;
  if (p == "full") cfg_distFull = dist;
  else cfg_distEmpty = dist;
  if (!savePrefs()) {
    cfg_distFull = oldDistFull;
    cfg_distEmpty = oldDistEmpty;
    server.send(500, "application/json", "{\"ok\":false}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiConfigPost() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (!password.isEmpty() && (password.length() < 8 || password.length() > 63))) {
    server.send(400, "text/plain",
                "Invalid network name or password length. SSID must be 1-32 characters; password must be empty or 8-63 characters.");
    return;
  }
  if (!prefs.begin(WIFI_NAMESPACE, false)) {
    server.send(500, "text/plain", "Could not open Wi-Fi settings in flash.");
    return;
  }
  size_t ssidWritten = prefs.putString(WIFI_SSID_KEY, ssid);
  size_t passwordWritten = prefs.putString(WIFI_PASS_KEY, password);
  prefs.end();
  if (ssidWritten != ssid.length() ||
      (!password.isEmpty() && passwordWritten != password.length())) {
    server.send(500, "text/plain", "Could not save Wi-Fi settings to flash.");
    return;
  }

  server.send(200, "text/html",
              "<!doctype html><html><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>Wi-Fi saved</title><body><h1>Wi-Fi settings saved</h1>"
              "<p>The device is restarting and will try to connect. If it cannot connect, "
              "the SmartPoultry_Setup network will remain available.</p></body></html>");
  Serial.println("[WIFI] Credentials saved. Restarting to connect...");
  g_wifiRebootPending = true;
  t_wifiReboot = millis();
}

void handleNotFound() {
  if (g_wifiProvisioning) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

void setupServer() {
  server.on("/",              HTTP_GET,  handleRoot);
  server.on("/wifi",          HTTP_GET,  handleRoot);
  server.on("/wifi",          HTTP_POST, handleWifiConfigPost);
  server.on("/api/data",      HTTP_GET,  handleApiData);
  server.on("/api/feed",      HTTP_POST, handleApiFeed);
  server.on("/api/config",    HTTP_GET,  handleApiConfig);
  server.on("/api/config",    HTTP_POST, handleApiConfig);
  server.on("/api/calibrate", HTTP_POST, handleApiCalibrate);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Web server started on port 80.");
}

void printStationConnection() {
  Serial.print("[WIFI] Connected to configured network. Station IP: http://");
  Serial.println(WiFi.localIP());
  if (g_mdnsStarted || MDNS.begin("poultry")) {
    g_mdnsStarted = true;
    Serial.println("[WIFI] Dashboard: http://poultry.local/");
  } else {
    Serial.println("[WIFI] ERROR: Could not start mDNS; use the station IP above.");
  }
}

bool loadWifiCredentials(String& ssid, String& password) {
  if (!prefs.begin(WIFI_NAMESPACE, true)) return false;
  ssid = prefs.getString(WIFI_SSID_KEY, "");
  password = prefs.getString(WIFI_PASS_KEY, "");
  prefs.end();
  g_hasWifiCredentials = !ssid.isEmpty();
  return g_hasWifiCredentials;
}

void startSetupAccessPoint() {
  if (g_wifiProvisioning) return;

  static const char PASSWORD_CHARS[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  char password[13];
  for (size_t i = 0; i < sizeof(password) - 1; i++) {
    password[i] = PASSWORD_CHARS[esp_random() % (sizeof(PASSWORD_CHARS) - 1)];
  }
  password[sizeof(password) - 1] = '\0';

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(SETUP_AP_SSID, password)) {
    Serial.println("[WIFI] ERROR: Could not start setup access point.");
    return;
  }
  g_wifiProvisioning = true;
  if (!dnsServer.start(53, "*", WiFi.softAPIP())) {
    Serial.println("[WIFI] ERROR: Could not start captive DNS; open http://192.168.4.1/ manually.");
  }
  Serial.printf("[WIFI] Setup network: %s\n", SETUP_AP_SSID);
  Serial.printf("[WIFI] Setup password: %s\n", password);
  Serial.print("[WIFI] Setup portal: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("[WIFI] Enter your local Wi-Fi details on the setup page.");
}

void setupWiFi() {
  String ssid;
  String password;
  if (!loadWifiCredentials(ssid, password)) {
    Serial.println("[WIFI] No saved network. Starting setup access point.");
    startSetupAccessPoint();
    return;
  }

  Serial.printf("[WIFI] Connecting to saved network '%s' (8-second timeout)...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 8000UL) {
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_stationConnected = true;
    printStationConnection();
  } else {
    Serial.println("[WIFI] Station connection failed. Starting setup access point.");
    startSetupAccessPoint();
  }
}

void maintainWiFi(uint32_t now) {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !g_stationConnected) {
    g_stationConnected = true;
    if (g_wifiProvisioning) {
      dnsServer.stop();
      g_wifiProvisioning = false;
      Serial.println("[WIFI] Provisioned station connected; dashboard is now available.");
    }
    printStationConnection();
  } else if (!connected && g_stationConnected) {
    g_stationConnected = false;
    t_wifiReconnect = now;
    Serial.println("[WIFI] Station connection lost.");
  }
  if (!connected && g_hasWifiCredentials && now - t_wifiReconnect >= 30000UL) {
    t_wifiReconnect = now;
    Serial.println("[WIFI] Reconnecting to saved network...");
    WiFi.reconnect();
    if (!g_wifiProvisioning) {
      Serial.println("[WIFI] Starting setup access point while reconnecting.");
      startSetupAccessPoint();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- SMART POULTRY SYSTEM BOOTING ---");

  loadPrefs();

  relaySet(PIN_FAN, false);
  relaySet(PIN_HEAT, false);
  pinMode(PIN_FAN,    OUTPUT);
  pinMode(PIN_HEAT,   OUTPUT);
  relaySet(PIN_FAN, false);
  relaySet(PIN_HEAT, false);
  pinMode(PIN_BUZZER, OUTPUT);
  relaySet(PIN_FAN,  false);
  relaySet(PIN_HEAT, false);
  digitalWrite(PIN_BUZZER, LOW);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  dht.begin();
  servo.attach(PIN_SERVO);
  servo.write(0);

  setupWiFi();
  setupServer();

  t_dhtOkLast = millis();
  t_autoFeed  = millis();
  Serial.println("[SYSTEM] Ready and operational.\n");
}

void loop() {
  uint32_t now = millis();
  pollSerialCommands();
  maintainWiFi(now);
  if (g_wifiProvisioning) dnsServer.processNextRequest();
  server.handleClient();
  if (g_wifiRebootPending && now - t_wifiReboot >= 500UL) {
    Serial.flush();
    ESP.restart();
  }

  if (now - t_dhtLast >= DHT_INTERVAL) {
    t_dhtLast = now;
    updateDHT();
    applyClimate();

    float dist = measureDistanceCm(isfinite(g_temp) ? g_temp : 25.0f);
    if (dist >= 2.0f && dist <= 400.0f) {
      g_feedPct = calcFeedPct(dist);
      g_feedValid = true;
      t_feedOkLast = millis();
    }

  }

  updateBuzzer(now);

  if (servoState == SERVO_OPEN && now - t_servoStart >= SERVO_HOLD_MS) {
    servo.write(0);
    servoState   = SERVO_CLOSE;
    t_servoStart = now;
  } else if (servoState == SERVO_CLOSE && now - t_servoStart >= SERVO_HOLD_MS) {
    servoState = SERVO_IDLE;
  }

  if (cfg_autoMin > 0 && now - t_autoFeed >= cfg_autoMin * 60000UL) {
    t_autoFeed = now;
    startServo();
  }

  if (now - t_statusLast >= DHT_INTERVAL) {
    t_statusLast = now;
    printStatus();
  }
}
