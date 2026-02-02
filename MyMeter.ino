/***************************************************************************
  MyMeter energy or gas tracking with Wemos D1 mini and a
  MicroWakeupper battery shield with reed switch or proximity sensor
  (see README.md for setup/installation)

  (c) 2022-2025 @tstoegi, Tobias Stöger , MIT license
 ***************************************************************************/


#include <Arduino.h>

#include "config.h"       // located in the sketch folder - edit the file and define your settings
#include "credentials.h"  // copy credentials.h.example to credentials.h and fill in your values


#define versionString "2.0.20260130.1"

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

// WiFiManager custom parameters
WiFiManagerParameter* customMqttBroker;
WiFiManagerParameter* customMqttPort;
WiFiManagerParameter* customMqttUser;
WiFiManagerParameter* customMqttPassword;
WiFiManagerParameter* customMqttTopic;
WiFiManagerParameter* customOtaCheckbox;
WiFiManagerParameter* customSavedWifiInfo;
WiFiManagerParameter* customFactoryReset;

bool shouldSaveConfig = false;
bool factoryResetRequested = false;
bool otaOnBootRequested = false;

// Callback for WiFiManager when config is saved
void saveConfigCallback() {
  Log("Config should be saved");
  shouldSaveConfig = true;
}

// Load configuration from LittleFS
bool loadConfig() {
  Log("Loading config from LittleFS");

  if (!LittleFS.exists("/config.json")) {
    Log("Config file not found");
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

  Log("Config loaded successfully");
  Log(config.broker);
  return true;
}

// Save configuration to LittleFS
bool saveConfig() {
  Log("Saving config to LittleFS");

  JsonDocument doc;
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
    clearPortalFlag();
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
    setPortalFlag();  // Save flag so it survives additional resets
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

        if (microWakeupper.resetedBySwitch() && microWakeupper.isActive()) {
          Log("Launched by a MicroWakeupperEvent");
          launchedByMicroWakeupperEvent = true;
          microWakeupper.disable();  // Preventing new triggering/resets
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

          setNextState(state_sendMqtt);
        } else {
          Log("No launchedByMicroWakeupperEvent");
          if (otaEnabled) {
            setNextState(state_idle);
          } else {
            setNextState(state_turningOff);
          }
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
        digitalWrite(LED_BUILTIN, false);
        delay(100);
        digitalWrite(LED_BUILTIN, true);
        delay(1000);

        static unsigned long prevMillis = millis();
        if (millis() - prevMillis >= timeoutOTA * 1000) {
          Log("OTA timed out!");
          setNextState(state_turningOff);
        }
        break;
      }
    case state_turningOff:
      {
        Log("state_turningOff");

        digitalWrite(LED_BUILTIN, true);  // turn led off

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

bool setupWifi() {
  WiFi.forceSleepWake();
  WiFi.persistent(false);  // We handle credentials ourselves in LittleFS
  WiFi.mode(WIFI_STA);
  delay(100);

#ifdef STATIC_IP
  WiFi.config(ip, gateway, subnet, dns);
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
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setConfigPortalTimeout(300);

    // Create custom parameters for MQTT configuration
    char portStr[6];
    snprintf(portStr, sizeof(portStr), "%d", config.port);

    customMqttBroker = new WiFiManagerParameter("mqtt_broker", "MQTT Broker IP", config.broker, 63);
    customMqttPort = new WiFiManagerParameter("mqtt_port", "MQTT Port", portStr, 5);
    customMqttUser = new WiFiManagerParameter("mqtt_user", "MQTT User", config.user, 31);

    const char* pwCustomHtml = "type='password'";
    customMqttPassword = new WiFiManagerParameter("mqtt_password", "MQTT Password", config.password, 63, pwCustomHtml);

    const char* showPwHtml =
      "<label style='display:block;margin-top:5px'>"
      "<input type='checkbox' onclick=\"var p=document.getElementById('mqtt_password');"
      "p.type=this.checked?'text':'password';\"> Show Password</label>";
    static WiFiManagerParameter showPwCheckbox(showPwHtml);

    customMqttTopic = new WiFiManagerParameter("mqtt_topic", "MQTT Topic (e.g. haus/mymeter)", config.topic, 63);

    // OTA checkbox with hidden field for WiFiManager to read
    const char* otaCheckboxHtml =
      "<br><label><input type='checkbox' id='ota_cb' onchange=\"document.getElementById('ota_boot').value=this.checked?'yes':'no'\"> "
      "Enable OTA after reboot</label>";
    customOtaCheckbox = new WiFiManagerParameter(otaCheckboxHtml);
    static WiFiManagerParameter otaBootValue("ota_boot", "", "no", 4, "type='hidden'");

    // Show saved WiFi info
    String savedWifiHtml = "<p style='color:#666;margin:10px 0'><b>Saved WiFi:</b> ";
    savedWifiHtml += strlen(config.wifiSsid) > 0 ? config.wifiSsid : "(none)";
    savedWifiHtml += "</p>";
    customSavedWifiInfo = new WiFiManagerParameter(savedWifiHtml.c_str());

    // Factory reset checkbox with hidden field for WiFiManager to read
    const char* factoryResetHtml =
      "<br><hr><label style='color:red'><input type='checkbox' id='fr_cb' onchange=\"document.getElementById('factory_reset').value=this.checked?'yes':'no'\"> "
      "Factory Reset (delete all settings)</label>";
    customFactoryReset = new WiFiManagerParameter(factoryResetHtml);

    // Hidden field that WiFiManager can read
    static WiFiManagerParameter factoryResetValue("factory_reset", "", "no", 4, "type='hidden'");

    wifiManager.addParameter(customSavedWifiInfo);
    wifiManager.addParameter(customMqttBroker);
    wifiManager.addParameter(customMqttPort);
    wifiManager.addParameter(customMqttUser);
    wifiManager.addParameter(customMqttPassword);
    wifiManager.addParameter(&showPwCheckbox);
    wifiManager.addParameter(customMqttTopic);
    wifiManager.addParameter(customOtaCheckbox);
    wifiManager.addParameter(&otaBootValue);
    wifiManager.addParameter(customFactoryReset);
    wifiManager.addParameter(&factoryResetValue);

    String apName = String(CO_MYMETER_NAME) + "-Setup";

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

    if (!connected) {
      Log("WiFi connection failed!");
      delete customMqttBroker;
      delete customMqttPort;
      delete customMqttUser;
      delete customMqttPassword;
      delete customMqttTopic;
      delete customOtaCheckbox;
      delete customSavedWifiInfo;
      delete customFactoryReset;
      return false;
    }

    // Check for factory reset
    if (strcmp(factoryResetValue.getValue(), "yes") == 0) {
      Log("Factory reset requested!");
      LittleFS.format();
      WiFi.disconnect(true);  // Clear WiFi credentials
      delay(1000);
      ESP.restart();
    }

    // Save WiFi and MQTT config
    if (shouldSaveConfig || connected) {
      Log("Saving config...");

      // Save WiFi credentials from connected network
      strlcpy(config.wifiSsid, WiFi.SSID().c_str(), sizeof(config.wifiSsid));
      strlcpy(config.wifiPassword, WiFi.psk().c_str(), sizeof(config.wifiPassword));

      // Save MQTT parameters
      strlcpy(config.broker, customMqttBroker->getValue(), sizeof(config.broker));
      config.port = atoi(customMqttPort->getValue());
      if (config.port == 0) config.port = 1883;
      strlcpy(config.user, customMqttUser->getValue(), sizeof(config.user));
      strlcpy(config.password, customMqttPassword->getValue(), sizeof(config.password));
      strlcpy(config.topic, customMqttTopic->getValue(), sizeof(config.topic));

      // Check OTA checkbox
      if (strcmp(otaBootValue.getValue(), "yes") == 0) {
        config.otaOnBoot = true;
      }

      saveConfig();
      buildTopics();

      Log("Config saved, restarting...");
      delay(1000);
      ESP.restart();
    }

    delete customMqttBroker;
    delete customMqttPort;
    delete customMqttUser;
    delete customMqttPassword;
    delete customMqttTopic;
    delete customOtaCheckbox;
    delete customSavedWifiInfo;
    delete customFactoryReset;
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
  String ota_client_id = String(CO_MYMETER_NAME) + localIPWithoutDots;
  Log("OTA_CLIENT_ID: ");
  Log(ota_client_id);

  ArduinoOTA.setHostname(ota_client_id.c_str());

  ArduinoOTA.setPassword(CR_OTA_MYMETER_CLIENT_PASSWORD);

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
    String clientId = String(CO_MYMETER_NAME) + WiFi.localIP().toString();
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
