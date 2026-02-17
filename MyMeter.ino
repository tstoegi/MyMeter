/***************************************************************************
  MyMeter energy or gas tracking with Wemos D1 mini and a
  MicroWakeupper battery shield with reed switch or proximity sensor
  (see README.md for setup/installation)

  (c) 2022 @tstoegi, Tobias Stöger , MIT license
 ***************************************************************************/


#include <Arduino.h>

#include "config.h"       // located in the sketch folder - edit the file and define your settings
#include "credentials.h"  // copy credentials.h.example to credentials.h and fill in your values


#define VERSION_YEAR 2026
#define VERSION_MONTH 2
#define BUILD_NUMBER 41

// Combine version: YEAR.MONTH.BUILD (e.g., 2026.2.3)
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define versionString TOSTRING(VERSION_YEAR) "." TOSTRING(VERSION_MONTH) "." TOSTRING(BUILD_NUMBER)

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Ticker.h>

// Configuration (loaded from LittleFS)
struct Config {
  // Device
  char deviceName[32] = "";  // Device name (e.g., "MyMeter", "Gasmeter")
  // WiFi
  char wifiSsid[33] = "";
  char wifiPassword[65] = "";
  // MQTT
  char broker[64] = "";
  int port = 1883;
  char user[32] = "";
  char password[64] = "";
  char topic[64] = "";  // Full MQTT topic path, set via portal (e.g., haus/gasmeter)
  bool otaOnBoot = false;
  // OTA
  char otaPassword[65] = "";  // Empty by default - configure via portal
};
Config config;

char pubTopic[128];
char subTopic[160];

#ifdef DEBUG
#define Log(str) \
  Serial.print(__LINE__); \
  Serial.print(' '); \
  Serial.print(millis()); \
  Serial.print(": "); \
  Serial.print(__PRETTY_FUNCTION__); \
  Serial.print(' '); \
  Serial.println(str);
#define Logf(str, param) \
  Serial.print(__LINE__); \
  Serial.print(' '); \
  Serial.print(millis()); \
  Serial.print(": "); \
  Serial.print(__PRETTY_FUNCTION__); \
  Serial.print(' '); \
  Serial.printf(str, param);
#else
#define Log(str)
#define Logf(str, param)
#endif

#include <MicroWakeupper.h>
MicroWakeupper microWakeupper;  //MicroWakeupper instance (only one is supported!)
bool launchedByMicroWakeupperEvent = false;

#ifdef MQTT_TLS
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif
PubSubClient mqttClient(espClient);
char msg[50];

const long one_turnaround = 10;  // For gas: one complete round 10 liter of gas (or 0.01 m3)

struct MyCounter {
  long total_read;
  long total;
};
MyCounter myCounter = { 0 };  // initial value can be set via mqtt retain message - see readme

// Multi-reset config portal feature: reset 6 times within 10 seconds to trigger config portal
#define RESET_PATTERN_THRESHOLD 6
#define RESET_PATTERN_TIMEOUT 10  // seconds

long rssi = 0;  // wifi signal strength

float voltageCalibration = 0.0;

enum State {
  state_startup = 0,
  state_setupWifi = 1,
  state_setupMqtt = 3,
  state_receiveMqtt = 4,
  state_checkSensorData = 5,
  state_sendMqtt = 6,
  state_idle = 7,
  state_turningOff = 8,
  state_turnedOff = 9
} nextState;

// LED blink ticker for config portal
Ticker ledTicker;
void blinkLED() {
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

const int timeoutOTA = 120;  // Wait for 2 minutes
bool otaEnabled = false;
bool startConfigPortal = false;
unsigned long resetPatternStartTime = 0;

bool mqttAvailable = false;

int nextStateDelaySeconds = 0;

unsigned long startTime;

// No more custom parameters - we use custom HTTP handlers now

bool shouldSaveConfig = false;
bool factoryResetRequested = false;
bool otaOnBootRequested = false;

// Global WiFiManager pointer for custom handlers
WiFiManager* globalWifiManager = nullptr;

// Callback for WiFiManager when WiFi config is saved
// Note: MQTT, OTA, and Device settings are now handled by custom pages
void saveConfigCallback() {
  Log("WiFi config callback triggered");
  shouldSaveConfig = true;
}

// Create default configuration if defines are present
bool createDefaultConfig() {
#if defined(DEFAULT_WIFI_SSID) && defined(DEFAULT_MQTT_BROKER)
  Log("Creating default config from credentials.h");

  strlcpy(config.deviceName, CO_MYMETER_NAME, sizeof(config.deviceName));
  Log("Device name: ");
  Log(config.deviceName);

  strlcpy(config.wifiSsid, DEFAULT_WIFI_SSID, sizeof(config.wifiSsid));
  Log("WiFi SSID: ");
  Log(config.wifiSsid);

  strlcpy(config.wifiPassword, DEFAULT_WIFI_PASSWORD, sizeof(config.wifiPassword));

  strlcpy(config.broker, DEFAULT_MQTT_BROKER, sizeof(config.broker));
  Log("MQTT Broker: ");
  Log(config.broker);

  config.port = DEFAULT_MQTT_PORT;
  Log("MQTT Port: ");
  Log(String(config.port));

  strlcpy(config.user, DEFAULT_MQTT_USER, sizeof(config.user));
  Log("MQTT User: ");
  Log(config.user);

  strlcpy(config.password, DEFAULT_MQTT_PASSWORD, sizeof(config.password));

  strlcpy(config.topic, DEFAULT_MQTT_TOPIC, sizeof(config.topic));
  Log("MQTT Topic: ");
  Log(config.topic);

  strlcpy(config.otaPassword, CR_OTA_MYMETER_CLIENT_PASSWORD, sizeof(config.otaPassword));
  config.otaOnBoot = false;

  // Save to LittleFS
  return saveConfig();
#else
  return false;
#endif
}

// Load configuration from LittleFS
bool loadConfig() {
  Log("Loading config from LittleFS");

  if (!LittleFS.exists("/config.json")) {
    Log("Config file not found");
    // Try to create default config
    if (createDefaultConfig()) {
      Log("Default config created successfully");
      return true;
    }
    return false;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Log("Failed to open config file");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Log("Failed to parse config file");
    return false;
  }

  // Device
  strlcpy(config.deviceName, doc["device_name"] | CO_MYMETER_NAME, sizeof(config.deviceName));
  // WiFi
  strlcpy(config.wifiSsid, doc["wifi_ssid"] | "", sizeof(config.wifiSsid));
  strlcpy(config.wifiPassword, doc["wifi_password"] | "", sizeof(config.wifiPassword));
  // MQTT
  strlcpy(config.broker, doc["mqtt_broker"] | "", sizeof(config.broker));
  config.port = doc["mqtt_port"] | 1883;
  strlcpy(config.user, doc["mqtt_user"] | "", sizeof(config.user));
  strlcpy(config.password, doc["mqtt_password"] | "", sizeof(config.password));
  strlcpy(config.topic, doc["mqtt_topic"] | "haus/mymeter", sizeof(config.topic));
  config.otaOnBoot = doc["ota_on_boot"] | false;
  // OTA
  strlcpy(config.otaPassword, doc["ota_password"] | "", sizeof(config.otaPassword));

  // Set default device name if empty
  if (strlen(config.deviceName) == 0) {
    strlcpy(config.deviceName, CO_MYMETER_NAME, sizeof(config.deviceName));
  }

  Log("Config loaded successfully");
  Log(config.broker);
  return true;
}

// Save configuration to LittleFS
bool saveConfig() {
  Log("Saving config to LittleFS");

  JsonDocument doc;
  // Device
  doc["device_name"] = config.deviceName;
  // WiFi
  doc["wifi_ssid"] = config.wifiSsid;
  doc["wifi_password"] = config.wifiPassword;
  // MQTT
  doc["mqtt_broker"] = config.broker;
  doc["mqtt_port"] = config.port;
  doc["mqtt_user"] = config.user;
  doc["mqtt_password"] = config.password;
  doc["mqtt_topic"] = config.topic;
  doc["ota_on_boot"] = config.otaOnBoot;
  // OTA
  doc["ota_password"] = config.otaPassword;

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Log("Failed to open config file for writing");
    return false;
  }

  serializeJson(doc, configFile);
  configFile.close();

  Log("Config saved successfully");
  return true;
}

// Load counter from LittleFS
long loadCounter() {
  if (!LittleFS.exists("/counter.txt")) {
    Log("Counter file not found, returning 0");
    return 0;
  }

  File f = LittleFS.open("/counter.txt", "r");
  if (!f) {
    Log("Failed to open counter file");
    return 0;
  }

  String content = f.readStringUntil('\n');
  f.close();

  long val = content.toInt();
  Log("Counter loaded: ");
  Log(val);
  return val;
}

// Save counter to LittleFS
bool saveCounter(long total) {
  if (total <= 0) {
    Log("Counter save skipped due to invalid total");
    return false;
  }

  File f = LittleFS.open("/counter.txt", "w");
  if (!f) {
    Log("Failed to open counter file for writing");
    return false;
  }

  f.println(total);
  f.close();

  Log("Counter saved: ");
  Log(total);
  return true;
}

// Load reset count from LittleFS
int loadResetCount() {
  if (!LittleFS.exists("/reset_count.txt")) {
    return 0;
  }

  File f = LittleFS.open("/reset_count.txt", "r");
  if (!f) {
    return 0;
  }

  int count = f.parseInt();
  f.close();
  return count;
}

// Save reset count to LittleFS
void saveResetCount(int count) {
  File f = LittleFS.open("/reset_count.txt", "w");
  if (!f) {
    return;
  }

  f.println(count);
  f.close();
}

// Clear reset count
void clearResetCount() {
  if (LittleFS.exists("/reset_count.txt")) {
    LittleFS.remove("/reset_count.txt");
  }
}

// Check if config portal flag exists
bool loadPortalFlag() {
  return LittleFS.exists("/portal_flag.txt");
}

// Set config portal flag (survives resets)
void setPortalFlag() {
  File f = LittleFS.open("/portal_flag.txt", "w");
  if (f) {
    f.println("1");
    f.close();
  }
}

// Clear config portal flag
void clearPortalFlag() {
  if (LittleFS.exists("/portal_flag.txt")) {
    LittleFS.remove("/portal_flag.txt");
  }
}

void buildTopics() {
  snprintf(pubTopic, sizeof(pubTopic), "%s", config.topic);
  snprintf(subTopic, sizeof(subTopic), "%s/settings", config.topic);
}

void mqttPublish(const char *mainTopic, const char *subTopic, String msg) {
  String topicString = String(mainTopic) + "/" + String(subTopic);
  mqttClient.publish(topicString.c_str(), msg.c_str(), true);
  Log(">> Published message: ");
  Log(topicString);
  Log(" ");
  Log(msg);
  mqttClient.loop();
}

void setup() {
  startTime = millis();
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, true);  // default off

  delay(100);

  Serial.println("\n*** MyMeter Monitoring ***");
  Serial.println(versionString);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Log("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  // Check if config portal was triggered in previous boot (survives resets)
  if (loadPortalFlag()) {
    Log("Config portal flag found from previous boot");
    startConfigPortal = true;
    clearPortalFlag();  // Clear immediately - if user resets, normal operation resumes
  }

  microWakeupper.begin();

  // Multi-reset detection: count all resets (including MicroWakeupper events)
  int resetCount = loadResetCount();
  resetCount++;
  saveResetCount(resetCount);

  Log("Reset count: ");
  Log(resetCount);

  if (resetCount >= RESET_PATTERN_THRESHOLD) {
    Log("Multi-reset config portal triggered!");
    startConfigPortal = true;
    // No setPortalFlag() - portal opens only once, reset exits to normal mode
    clearResetCount();
  }

  resetPatternStartTime = millis();

  nextState = state_startup;

  nextStateDelaySeconds = 0;
}

void loop() {
  if (otaEnabled) {
    digitalWrite(LED_BUILTIN, false);
    ArduinoOTA.handle();
  }

  // Clear reset counter after timeout (if not in config portal mode)
  static bool resetCounterCleared = false;
  if (!resetCounterCleared && resetPatternStartTime > 0 && millis() - resetPatternStartTime >= RESET_PATTERN_TIMEOUT * 1000) {
    clearResetCount();
    resetCounterCleared = true;
    Log("Reset counter cleared after timeout");
  }

  static unsigned long prevMillis = 0;
  if (millis() - prevMillis >= nextStateDelaySeconds * 1000) {
    prevMillis = millis();
    doNextState(nextState);
  }

  delay(100);
}

void setNextState(State aNextState, int aNextStateDelaySeconds = 0) {
  nextState = aNextState;
  nextStateDelaySeconds = aNextStateDelaySeconds;
}

void doNextState(State aNewState) {
  Log("___ doNextState() ___: ");
  Log(aNewState);

  switch (aNewState) {
    case state_startup:
      {
        Log("state_startup");

        // Load config from LittleFS
        loadConfig();
        buildTopics();

        // Load counter from LittleFS
        myCounter.total_read = loadCounter();
        myCounter.total = myCounter.total_read;

        if (microWakeupper.resetedBySwitch()) {
          Log("Launched by a MicroWakeupperEvent");
          launchedByMicroWakeupperEvent = true;
          microWakeupper.disable();  // Preventing new triggering/resets
          // Note: Don't clear reset count here - it's cleared in state_turningOff
          // This allows multi-reset detection to work even when connected via serial
        }

        setNextState(state_setupWifi);
        break;
      }
    case state_setupWifi:
      {
        Log("state_setupWifi");
        if (!setupWifi()) {
          setNextState(state_checkSensorData);
          break;
        }
        setNextState(state_setupMqtt);
        break;
      }
    case state_setupMqtt:
      {
        Log("state_setupMqtt");

        // Enable OTA if triggered by multi-reset pattern or config flag
        if ((startConfigPortal || config.otaOnBoot) && !otaEnabled) {
          Log("Enabling OTA");
          otaEnabled = setupOTA();
          // Clear the OTA on boot flag after enabling
          if (config.otaOnBoot) {
            config.otaOnBoot = false;
            saveConfig();
          }
        }

        setupMqtt();
        if (!mqttReconnect()) {
          setNextState(state_checkSensorData);
          break;
        }
        setNextState(state_receiveMqtt);
        break;
      }
    case state_receiveMqtt:
      {
        Log("state_receiveMqtt");
        // we process all retained mqtt messages (in callback)
        for (int i = 0; i < 50; i++) {  // todo Hack - if more incoming messages are queued
          mqttClient.loop();
          delay(10);
          yield();
        }
        setNextState(state_checkSensorData);
        break;
      }
    case state_checkSensorData:
      {
        Log("state_checkSensorData");

        if (launchedByMicroWakeupperEvent) {
          Log("launchedByMicroWakeupperEvent");
          Log(myCounter.total);
          myCounter.total = myCounter.total + one_turnaround;
          Log(one_turnaround);
          Log(myCounter.total);

          if (myCounter.total != myCounter.total_read) {
            saveCounter(myCounter.total);
          } else {
            Log("!!! Counter save skipped due to unchanged total. !!!");
          }
        } else {
          Log("No launchedByMicroWakeupperEvent");
        }

        // Always go through state_sendMqtt to publish version and status
        if (otaEnabled && !launchedByMicroWakeupperEvent) {
          setNextState(state_idle);
        } else {
          setNextState(state_sendMqtt);
        }
        break;
      }
    case state_sendMqtt:
      {
        Log("state_sendMqtt");
        if (mqttAvailable) {
          // float to string
          long total = myCounter.total;
          long ganz = total / 1000;
          long dezimal = (total % 1000) / 10;  // only 2 decimal places
          char buf[16];
          snprintf(buf, sizeof(buf), "%ld.%02ld", ganz, dezimal);
          mqttPublish(pubTopic, "total", String(buf));

          mqttPublish(pubTopic, "wifi_rssi", String(rssi));
          mqttPublish(pubTopic, "batteryVoltage", String(microWakeupper.readVBatt() + voltageCalibration));
#ifdef DEBUG
          mqttPublish(pubTopic, "version", versionString);
          mqttPublish(pubTopic, "localIP", WiFi.localIP().toString());
#endif
        }
        if (otaEnabled) {
          setNextState(state_idle);
        } else {
          setNextState(state_turningOff);
        }
        break;
      }
    case state_idle:  // used for OTA updates - microWakeupper is disabled during idle
      {
        Log("state_idle (until next manual restart or timeout)");
        // Double-blink pattern for OTA mode (distinctive from config portal)
        digitalWrite(LED_BUILTIN, false);  // blink 1
        delay(100);
        digitalWrite(LED_BUILTIN, true);
        delay(150);
        digitalWrite(LED_BUILTIN, false);  // blink 2
        delay(100);
        digitalWrite(LED_BUILTIN, true);
        delay(1000);  // pause

        static unsigned long prevMillis = millis();
        if (millis() - prevMillis >= timeoutOTA * 1000) {
          Log("OTA timed out!");
          otaEnabled = false;  // Clear OTA flag to allow normal shutdown
          Log("OTA flag cleared - device will now turn off normally");
          setNextState(state_turningOff);
        }
        break;
      }
    case state_turningOff:
      {
        Log("state_turningOff");

        // Safety check: Don't turn off if OTA is enabled
        if (otaEnabled) {
          Log("OTA enabled - staying in idle mode instead of turning off");
          setNextState(state_idle);
          break;
        }

        digitalWrite(LED_BUILTIN, true);  // turn led off

        // Clear reset counter before sleep - prevents accumulation from normal wake cycles
        clearResetCount();

        Log("Waiting for turning device off (only if jumper J1 on MicroWakeupperShield is cutted!)");
        microWakeupper.reenable();
        delay(5000);  // time for the MicroWakeupper to power off the wemos

        deepSleep();  // if the MicroWakeupper Switch is still in an ON state, we try to sleep
        delay(1000);
        setNextState(state_turnedOff);
        break;
      }
    case state_turnedOff:
      {
        Log("state_turnedOff");
        delay(1000);
        break;
      }
    default:
      {
        Log("!!! unknown status !!!");
      }
  }

  if (otaEnabled == false && nextState > state_setupMqtt && nextState < state_turningOff) {
    if (!mqttClient.loop()) {  //needs to be called regularly
      mqttReconnect();
    }
  }
}

// Helper function to generate HTML header
String getHTMLHeader(const char* title) {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + String(title) + "</title>";
  html += "<style>body{font-family:sans-serif;margin:20px;background:#f0f0f0}";
  html += ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}";
  html += "h2{color:#1fa3ec;margin-top:0}input,select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px}";
  html += "button,a.button{background:#1fa3ec;color:white;padding:12px;border:none;border-radius:4px;cursor:pointer;width:100%;margin:8px 0;text-decoration:none;display:block;box-sizing:border-box;text-align:center;font-size:16px;font-family:sans-serif;font-weight:normal}";
  html += "button:hover,a.button:hover{background:#1581bd}.back{background:#888}.back:hover{background:#666}";
  html += "label{display:block;margin:10px 0 5px 0;font-weight:bold}</style></head><body><div class='container'>";
  return html;
}

String getHTMLFooter() {
  return "</div></body></html>";
}

// Custom handler for MQTT settings page
void handleMqttPage() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  String html = getHTMLHeader("MQTT Settings");
  html += "<h2>📡 MQTT Configuration</h2>";
  html += "<form method='POST' action='/mqtt_save'>";
  html += "<label>Broker IP</label><input name='mqtt_broker' value='" + String(config.broker) + "'>";
  html += "<label>Port</label><input name='mqtt_port' value='" + String(config.port) + "'>";
  html += "<label>User</label><input name='mqtt_user' value='" + String(config.user) + "'>";
  html += "<label>Password</label><input type='password' name='mqtt_password' value='" + String(config.password) + "'>";
  html += "<label>Topic</label><input name='mqtt_topic' value='" + String(config.topic) + "' placeholder='haus/mymeter'>";
  html += "<button type='submit'>Save</button>";
  html += "</form>";
  html += "<a href='/' class='button back'>Back to Menu</a>";
  html += getHTMLFooter();

  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

// Custom handler for Update/OTA settings page
void handleUpdatePage() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  String html = getHTMLHeader("Update Settings");
  html += "<h2>🔄 Update (OTA)</h2>";
  html += "<form method='POST' action='/update_save'>";
  html += "<label>OTA Password <small>(leave empty to disable)</small></label>";
  html += "<input type='password' name='ota_password' value='" + String(config.otaPassword) + "'>";
  html += "<label><input type='checkbox' name='ota_boot' value='yes'> Enable OTA after reboot</label>";
  html += "<button type='submit'>Save</button>";
  html += "</form>";
  html += "<a href='/' class='button back'>Back to Menu</a>";
  html += getHTMLFooter();

  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

// Custom handler for Device settings page
void handleSettingsPage() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  String html = getHTMLHeader("Device Settings");
  html += "<h2>⚙️ Device Settings</h2>";
  html += "<form method='POST' action='/settings_save'>";
  html += "<label>Device Name</label><input name='device_name' value='" + String(strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME) + "'>";

  char counterTotalStr[16];
  float totalFloat = myCounter.total / 1000.0f;
  snprintf(counterTotalStr, sizeof(counterTotalStr), "%.2f", totalFloat);
  html += "<label>Counter Total (m³ or kWh)</label><input name='counter_total' value='" + String(counterTotalStr) + "'>";

  html += "<hr><label style='color:red'><input type='checkbox' name='factory_reset' value='yes'> Factory Reset (delete all settings)</label>";
  html += "<button type='submit'>Save</button>";
  html += "</form>";
  html += "<a href='/' class='button back'>Back to Menu</a>";
  html += getHTMLFooter();

  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

// Save handlers
void handleMqttSave() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  if (globalWifiManager->server->hasArg("mqtt_broker")) {
    strlcpy(config.broker, globalWifiManager->server->arg("mqtt_broker").c_str(), sizeof(config.broker));
    config.port = globalWifiManager->server->arg("mqtt_port").toInt();
    if (config.port == 0) config.port = 1883;
    strlcpy(config.user, globalWifiManager->server->arg("mqtt_user").c_str(), sizeof(config.user));
    strlcpy(config.password, globalWifiManager->server->arg("mqtt_password").c_str(), sizeof(config.password));
    strlcpy(config.topic, globalWifiManager->server->arg("mqtt_topic").c_str(), sizeof(config.topic));
    saveConfig();
  }

  String html = getHTMLHeader("Saved");
  html += "<h2>✓ Saved!</h2><p>MQTT settings saved successfully.</p>";
  html += "<a href='/mqtt' class='button'>Back to MQTT Settings</a>";
  html += "<a href='/' class='button back'>Main Menu</a>";
  html += getHTMLFooter();
  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

void handleUpdateSave() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  if (globalWifiManager->server->hasArg("ota_password")) {
    String otaPwd = globalWifiManager->server->arg("ota_password");
    if (otaPwd.length() > 0) {
      strlcpy(config.otaPassword, otaPwd.c_str(), sizeof(config.otaPassword));
    } else {
      config.otaPassword[0] = '\0';
    }
  }

  config.otaOnBoot = globalWifiManager->server->hasArg("ota_boot");
  saveConfig();

  String html = getHTMLHeader("Saved");
  html += "<h2>✓ Saved!</h2><p>Update settings saved successfully.</p>";
  html += "<a href='/update' class='button'>Back to Update Settings</a>";
  html += "<a href='/' class='button back'>Main Menu</a>";
  html += getHTMLFooter();
  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

void handleSettingsSave() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  // Device name
  if (globalWifiManager->server->hasArg("device_name")) {
    String devName = globalWifiManager->server->arg("device_name");
    if (devName.length() > 0) {
      strlcpy(config.deviceName, devName.c_str(), sizeof(config.deviceName));
    }
  }

  // Counter total
  if (globalWifiManager->server->hasArg("counter_total")) {
    float newTotal = globalWifiManager->server->arg("counter_total").toFloat();
    if (newTotal >= 0 && newTotal < 1e9) {
      myCounter.total = newTotal * 1000.0f;
      saveCounter(myCounter.total);
    }
  }

  // Factory reset
  if (globalWifiManager->server->hasArg("factory_reset")) {
    LittleFS.format();
    WiFi.disconnect(true);

    String html = getHTMLHeader("Factory Reset");
    html += "<h2>🔄 Factory Reset</h2><p>All settings deleted. Device will restart...</p>";
    html += getHTMLFooter();
    globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
    delay(2000);
    ESP.restart();
    return;
  }

  saveConfig();

  String html = getHTMLHeader("Saved");
  html += "<h2>✓ Saved!</h2><p>Device settings saved successfully.</p>";
  html += "<a href='/settings' class='button'>Back to Device Settings</a>";
  html += "<a href='/' class='button back'>Main Menu</a>";
  html += getHTMLFooter();
  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

// Custom handler for main menu (replaces WiFiManager's default root page)
void handleMainMenu() {
  if (!globalWifiManager || !globalWifiManager->server) return;

  const char* devName = strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME;

  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" + String(devName) + "</title>"
    "<style>"
    "body{font-family:sans-serif;background:#f0f0f0;margin:0;padding:20px}"
    ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
    "h1{color:#1fa3ec;text-align:center;margin-top:0}"
    "a{display:block;padding:16px;margin:10px 0;background:#1fa3ec;color:white;text-decoration:none;border-radius:6px;font-size:16px;text-align:center}"
    "a:hover{background:#1581bd}"
    ".exit{background:#888}.exit:hover{background:#666}"
    "</style></head><body><div class='container'>"
    "<h1>" + String(devName) + "</h1>"
    "<a href='/wifi'>📶 WiFi</a>"
    "<a href='/mqtt'>📡 MQTT</a>"
    "<a href='/settings'>⚙️  Settings</a>"
    "<a href='/update'>🔄 Update</a>"
    "<a href='/exit' class='exit'>❌ Exit</a>"
    "<p style='text-align:center;color:#999;font-size:12px;margin-top:20px'>Firmware: " + String(versionString) + "</p>"
    "</div></body></html>";

  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}

bool setupWifi() {
  WiFi.forceSleepWake();
  WiFi.persistent(false);  // We handle credentials ourselves in LittleFS
  WiFi.mode(WIFI_STA);
  delay(100);

#ifdef STATIC_IP
  IPAddress staticIP(STATIC_IP_ADDR);
  IPAddress staticGateway(STATIC_GATEWAY);
  IPAddress staticSubnet(STATIC_SUBNET);
  IPAddress staticDNS(STATIC_DNS);
  Log("Configuring static IP:");
  Log(staticIP.toString());
  Log("Gateway: ");
  Log(staticGateway.toString());
  Log("Subnet: ");
  Log(staticSubnet.toString());
  Log("DNS: ");
  Log(staticDNS.toString());
  WiFi.config(staticIP, staticGateway, staticSubnet, staticDNS);
#endif

  bool connected = false;

  // First try saved credentials from LittleFS (if not forcing config portal)
  if (!startConfigPortal && strlen(config.wifiSsid) > 0) {
    Log("Connecting with saved WiFi credentials...");
    Log(config.wifiSsid);

    WiFi.begin(config.wifiSsid, config.wifiPassword);

    int timeout = 20;  // 10 seconds
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
      delay(500);
      Log(".");
      timeout--;
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Log("Connected with saved credentials!");
    } else {
      Log("Saved credentials failed, starting portal...");
    }
  }

  // If not connected, use WiFiManager
  if (!connected) {
    WiFiManager wifiManager;
    globalWifiManager = &wifiManager;  // Store pointer for custom handlers

    wifiManager.setConfigPortalTimeout(300);
    wifiManager.setTitle("MyMeter");

    // Let WiFiManager handle only the wifi configuration page
    // We'll override the root page with our custom menu
    const char* menu[] = {"wifi"};
    wifiManager.setMenu(menu, 1);

    // Hide "No AP set" message on WiFi config page
    String customCSS = "<style>.msg{display:none !important;}</style>";
    wifiManager.setCustomHeadElement(customCSS.c_str());

    // Set callback to register custom handlers after WiFiManager sets up the server
    wifiManager.setWebServerCallback([]() {
      if (globalWifiManager && globalWifiManager->server) {
        Log("Registering custom HTTP handlers via callback");
        // Custom main menu at root (override WiFiManager's root)
        globalWifiManager->server->on("/", handleMainMenu);
        // Custom configuration pages
        globalWifiManager->server->on("/mqtt", handleMqttPage);
        globalWifiManager->server->on("/mqtt_save", HTTP_POST, handleMqttSave);
        globalWifiManager->server->on("/update", handleUpdatePage);
        globalWifiManager->server->on("/update_save", HTTP_POST, handleUpdateSave);
        globalWifiManager->server->on("/settings", handleSettingsPage);
        globalWifiManager->server->on("/settings_save", HTTP_POST, handleSettingsSave);
        Log("Custom handlers registered");
      }
    });

    // Create AP name with last 4 hex digits of MAC address for unique identification
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macSuffix[5];
    sprintf(macSuffix, "%02X%02X", mac[4], mac[5]);
    const char* devName = strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME;
    String apName = String(devName) + "-" + macSuffix + "-Setup";

    // Start fast LED blinking during config portal
    ledTicker.attach(0.2, blinkLED);

    if (startConfigPortal) {
      Log("Starting config portal (multi-reset triggered)");
      connected = wifiManager.startConfigPortal(apName.c_str());
    } else {
      Log("Starting config portal (no saved credentials)");
      connected = wifiManager.startConfigPortal(apName.c_str());
    }

    // Stop LED blinking
    ledTicker.detach();
    digitalWrite(LED_BUILTIN, true);  // LED off

    // Clear portal flag now that portal has finished
    clearPortalFlag();

    // If connected via portal, save WiFi credentials to our config.json
    if (connected) {
      String currentSsid = WiFi.SSID();
      if (currentSsid.length() > 0 && strcmp(config.wifiSsid, currentSsid.c_str()) != 0) {
        Log("Saving new WiFi credentials...");
        Log(currentSsid);
        strlcpy(config.wifiSsid, currentSsid.c_str(), sizeof(config.wifiSsid));
        strlcpy(config.wifiPassword, WiFi.psk().c_str(), sizeof(config.wifiPassword));
        saveConfig();
        Log("WiFi credentials saved!");
      }
    }

    if (!connected) {
      Log("WiFi connection failed!");
      globalWifiManager = nullptr;
      return false;
    }

    globalWifiManager = nullptr;  // Clear pointer after portal closes
  }

  Log("");
  Log("Connected to WiFi");
  Log("IP address: ");
  Log(WiFi.localIP());
  Log("BSSID: ");
  Log(WiFi.BSSIDstr());

  rssi = WiFi.RSSI();
  return true;
}

bool setupOTA() {
  String localIPWithoutDots = WiFi.localIP().toString();
  localIPWithoutDots.replace(".", "_");
  const char* devName = strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME;
  String ota_client_id = String(devName) + localIPWithoutDots;
  Log("OTA_CLIENT_ID: ");
  Log(ota_client_id);

  ArduinoOTA.setHostname(ota_client_id.c_str());

  // Use configured OTA password, disable auth if empty
  if (strlen(config.otaPassword) > 0) {
    ArduinoOTA.setPassword(config.otaPassword);
    Log("OTA password authentication enabled");
  } else {
    Log("OTA authentication disabled (no password set)");
  }

  ArduinoOTA.onStart([]() {
    Log("Start");
  });
  ArduinoOTA.onEnd([]() {
    Log("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Logf("Progress: %u%%\r", (progress / (total / 100)))
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Logf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Log("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Log("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Log("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Log("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Log("End Failed");
    }
  });

  ArduinoOTA.begin();
  Log("OTA ready");
  return true;
}

void setupMqtt() {
  // Check if MQTT broker is configured
  if (strlen(config.broker) == 0) {
    Log("MQTT broker not configured!");
    return;
  }

#ifdef MQTT_TLS
  if (strlen(CR_MQTT_BROKER_CERT_FINGERPRINT) > 0) {
    Log("Setting mqtt server fingerprint");
    espClient.setFingerprint(CR_MQTT_BROKER_CERT_FINGERPRINT);
  } else {
    Log("No fingerprint verification for mqtt");
    espClient.setInsecure();
  }
#endif

  mqttClient.setBufferSize(512);
  mqttClient.setServer(config.broker, config.port);
  mqttClient.setCallback(mqttCallback);
}

bool mqttReconnect() {
  // Check if MQTT broker is configured
  if (strlen(config.broker) == 0) {
    Log("MQTT broker not configured, skipping connection");
    mqttAvailable = false;
    return false;
  }

  // Loop until we're reconnected
  int maxRetries = 2;
  int retries = 0;
  while (!mqttClient.connected() && retries < maxRetries) {
    Log("Attempting MQTT connection...");

    // Create a random and unique client ID for mqtt
    const char* devName = strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME;
    String clientId = String(devName) + WiFi.localIP().toString();
    Log("MQTT_CLIENT_ID: ");
    Log(clientId);

    // Attempt to connect
    bool connectResult;
    if (strlen(config.user) > 0) {
      connectResult = mqttClient.connect(clientId.c_str(), config.user, config.password);
    } else {
      connectResult = mqttClient.connect(clientId.c_str());
    }

    if (connectResult) {
      Log("connected");

      String subTopicStr = String(subTopic) + "/#";
      mqttClient.subscribe(subTopicStr.c_str());
      Log("mqtt subscription topic: ");
      Log(subTopicStr.c_str());

      mqttClient.loop();
      mqttAvailable = true;
      return true;
    } else {
      Log("failed, rc=");
      Log(mqttClient.state());
      Log(" try again in 2 seconds");
      // Wait 2 seconds before retrying
      delay(2000);
      retries++;
    }
  }
  mqttAvailable = false;
  return false;
}


void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Log();
  Log("<< Received message: ");
  Log(topic);
  Log(" ");

  char buff_p[length + 1];
  for (unsigned int i = 0; i < length; i++) {
    buff_p[i] = (char)payload[i];
  }
  buff_p[length] = '\0';
  String str_payload = String(buff_p);
  Log(str_payload);

  String subTopicKey = "total";
  if (String(topic).endsWith(subTopicKey)) {
    float newValue = str_payload.toFloat();
    if (!isnan(newValue) && newValue > 0 && newValue < 1e9) {  // Reasonable max value
      Log("Override value total: ");
      Log(newValue);
      myCounter.total = newValue * 1000.0f;

      saveCounter(myCounter.total);

      Log(">>>");
      Log(String(myCounter.total / 1000.0f));
      mqttPublish(pubTopic, subTopicKey.c_str(), String(myCounter.total / 1000.0f));
      mqttPublish(subTopic, subTopicKey.c_str(), "");  // delete the retained mqtt message
    } else {
      Log("Invalid MQTT total ignored:");
      Log(str_payload);
    }
    return;
  }

  subTopicKey = "waitForOTA";
  if (String(topic).endsWith(subTopicKey)) {
    if (str_payload.startsWith("true")
        || str_payload.startsWith("yes")) {
      Log("Disabling temporarly turningOff/deepSleep");
      otaEnabled = setupOTA();
      mqttPublish(subTopic, subTopicKey.c_str(), "");  // delete the retained mqtt message
    }
    return;
  }

  subTopicKey = "voltageCalibration";
  if (String(topic).endsWith(subTopicKey)) {
    float newValue = str_payload.toFloat();
    Log("Calibrating voltage: ");
    Log(newValue);
    voltageCalibration = newValue;
    if (newValue == 0.0) {
      mqttPublish(subTopic, subTopicKey.c_str(), "");  // delete the retained mqtt message
    }
    return;
  }
}

void deepSleep() {
  mqttClient.disconnect();

  delay(100);

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(1);
  WiFi.forceSleepBegin();

  ESP.deepSleep(0, RF_DISABLED);  // sleep "forever"
  delay(100);                     // Seems recommended after calling deepSleep
}
