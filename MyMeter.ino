/***************************************************************************
  Gasmeter tracking with Wemos D1 mini, MicroWakeupper battery shield and reed switch or proximity sensor
  (see README.md for setup/installation)

  (c) 2022, 2023 @tstoegi, Tobias Stöger , MIT license
 ***************************************************************************/


#include <Arduino.h>

#include <Credentials.h>  // located (or create a new one) in folder "Arduino/libraries/Credentials/"
/*** example of a Credentials.h file

// your wifi
#define CR_WIFI_SSID "wifi_ssid"
#define CR_WIFI_PASSWORD "wifi_password"

// ota - over the air firmware updates - userid and password of your choise
#define CR_OTA_GASMETER_CLIENT_ID_PREFIX "gasmeter_"
#define CR_OTA_GASMETER_CLIENT_PASSWORD "0123456789"

// your mqtt server cert fingerprint -> use "" if you want to disable cert checking
#define CR_MQTT_BROKER_CERT_FINGERPRINT "AA F0 DE 66 E7 22 98 02 12 1D 59 08 4B 32 23 24 C9 F4 D1 00"

// your mqtt user and password as created on the server side
#define CR_MQTT_BROKER_GASMETER_USER "gasmeter"
#define CR_MQTT_BROKER_GASMETER_PASSWORD "0123456789"

elpmaxe ***/

#define versionString "0.5.20230129.2"

#include <EEPROM.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#include "config.h"  // located in the sketch folder - edit the file and define your settings

const char *pubTopic = CO_MQTT_TOPIC_MAIN_FOLDER_PUB CO_MYMETER_NAME;
const char *subTopic = CO_MQTT_TOPIC_MAIN_FOLDER_PUB CO_MYMETER_NAME CO_MQTT_TOPIC_SUB_FOLDER_SUB;

#if debug == true
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

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
char msg[50];

const long one_turnaround_liter = 10;  // one complete round 10 liter of gas (or 0.01 m3)

struct GasCounter {
  long total_liter;
};
GasCounter gasCounter = { 0 };  // initial value can be set via mqtt retain message - see readme

// we store the new value in EEPROM always at currentEEPROMAddress+1 to spread max write-life-cycles of the whole EEPROM - at the end we start from 0 again
int sizeofGasCounterLong = 10;

int currentEEPROMAddress = 0;  // first address we try to read a valid value
#define MAGIC_BYTE '#'         // start mark of our value in EEPROM "#1234567890" or "#  14567800" (max 10 digits)
#define EEPROM_SIZE_BYTES_MAX 512

long rssi = 0;  // wifi signal strength

bool turningOff = true;
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

bool otaEnabled = false;

bool mqttAvailable = false;

int nextStateDelaySeconds = 0;

unsigned long startTime;

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
  EEPROM.begin(EEPROM_SIZE_BYTES_MAX);  // mandator for esp8266 (because EEPROM is emulated for FLASH)
  microWakeupper.begin();               // For correct initialisation

  delay(100);

  Serial.println("\n*** Gasmeter Monitoring ***");
  Serial.println(versionString);

  nextState = state_startup;
  nextStateDelaySeconds = 0;
}

void loop() {
  if (otaEnabled) {
    ArduinoOTA.handle();
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
        pinMode(LED_BUILTIN, OUTPUT);
        digitalWrite(LED_BUILTIN, true);  // turn led off

        findLastEEPROMAddress();

        loadFromEEPROM();

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
      }
    case state_checkSensorData:
      {
        Log("state_checkSensorData");

        if (launchedByMicroWakeupperEvent) {
          Log(gasCounter.total_liter);
          gasCounter.total_liter = gasCounter.total_liter + one_turnaround_liter;
          Log(one_turnaround_liter);
          Log(gasCounter.total_liter);
          setNextState(state_sendMqtt);
        } else {
          setNextState(state_turningOff);
        }
      }
    case state_sendMqtt:
      {
        Log("state_sendMqtt");
        if (mqttAvailable) {
          mqttPublish(pubTopic, "total", String(gasCounter.total_liter / 1000.0f));
          mqttPublish(pubTopic, "wifi_rssi", String(rssi));
          mqttPublish(pubTopic, "batteryVoltage", String(microWakeupper.readVBatt() + voltageCalibration));
#if debug == true
          mqttPublish(pubTopic, "version", versionString);
          mqttPublish(pubTopic, "localIP", WiFi.localIP().toString());
#endif
        }
        setNextState(state_turningOff);
        break;
      }
    case state_idle:
      {
        Log("state_idle (until next manual restart or external reset)");  // can be used for OTA updates
        digitalWrite(LED_BUILTIN, false);
        delay(10);
        digitalWrite(LED_BUILTIN, true);
        delay(1000);

        static unsigned long prevMillis = millis();
        if (millis() - prevMillis >= 60 * 1000) {
          Log("OTA timed out!");
          turningOff = true;
          setNextState(state_turningOff);
        }
        break;
      }
    case state_turningOff:
      {
        if (!turningOff) {
          setNextState(state_idle);
          break;
        }
        Log("state_turningOff");

        digitalWrite(LED_BUILTIN, true);  // turn led off
        increaseEEPROMAddress();
        storeToEEPROM();
        EEPROM.end();

        Serial.println();
        Serial.println(millis() - startTime);
        Serial.println();

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

  if (nextState > state_setupMqtt && nextState < state_turningOff) {
    if (!mqttClient.loop()) {  //needs to be called regularly
      mqttReconnect();
    }
  }
}

// int32_t getWiFiChannel(const char* ssid) {
//   if (int32_t n = WiFi.scanNetworks()) {
//     for (uint8_t i = 0; i < n; i++) {
//       if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
//         return WiFi.channel(i);
//       }
//     }
//   }
//   return 0;
// }

bool setupWifi() {
  WiFi.forceSleepWake();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);  // <<< Station
  //WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  //WiFi.setOutputPower(20.5); // max
  delay(100);

#ifdef STATIC_IP
  WiFi.config(ip, gateway, subnet, dns);
#endif

  // Wait for connection
  int retries = 2;
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
#ifdef STATIC_WIFI
    Log("Trying WiFi channel id: ");
    Log(channel);
    Log("Trying WiFi BSSID: ");
    Log(bssid);
    WiFi.begin(CR_WIFI_SSID, CR_WIFI_PASSWORD, channel, bssid);
#else
    WiFi.begin(CR_WIFI_SSID, CR_WIFI_PASSWORD);
#endif
    Log(retries);
    int secondsTimeout = 10;
    while (WiFi.status() != WL_CONNECTED && secondsTimeout > 0) {
      yield();
      delay(1000);
      Log(".");
      secondsTimeout--;
    }
    retries--;
    Log();
  }

  if (retries == 0) {
    Log(CR_WIFI_SSID);
    Log("Connection failed!");
    return false;
  }

  Log("");
  Log("Connected to ");
  Log(CR_WIFI_SSID);
  Log("IP address: ");
  Log(WiFi.localIP());


  Log("BSSID: ");
  Log(WiFi.BSSIDstr());

  rssi = WiFi.RSSI();
  return true;
}

void setupOTA() {
  String localIPWithoutDots = WiFi.localIP().toString();
  localIPWithoutDots.replace(".", "_");
  String ota_client_id = CR_OTA_GASMETER_CLIENT_ID_PREFIX + localIPWithoutDots;
  Log("OTA_CLIENT_ID: ");
  Log(ota_client_id);

  ArduinoOTA.setHostname(ota_client_id.c_str());

  ArduinoOTA.setPassword(CR_OTA_GASMETER_CLIENT_PASSWORD);

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
}

void setupMqtt() {
  if (strlen(CR_MQTT_BROKER_CERT_FINGERPRINT) > 0) {
    Log("Setting mqtt server fingerprint");
    espClient.setFingerprint(CR_MQTT_BROKER_CERT_FINGERPRINT);
  } else {
    Log("No fingerprint verification for mqtt");
    espClient.setInsecure();
  }
  mqttClient.setBufferSize(512);
  mqttClient.setServer(CO_MQTT_BROKER_IP, CO_MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttCallback);
}

bool mqttReconnect() {
  // Loop until we're reconnected
  int maxRetries = 2;
  int retries = 0;
  while (!mqttClient.connected() && retries < maxRetries) {
    Log("Attempting MQTT connection...");

    // Create a random and unique client ID for mqtt
    String clientId = CO_MYMETER_NAME + WiFi.localIP().toString();
    Log("MQTT_CLIENT_ID: ");
    Log(clientId);

    // Attempt to connect
    if (mqttClient.connect(clientId.c_str(), CR_MQTT_BROKER_GASMETER_USER, CR_MQTT_BROKER_GASMETER_PASSWORD)) {
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

  char buff_p[length];
  for (int i = 0; i < length; i++) {
    buff_p[i] = (char)payload[i];
  }
  buff_p[length] = '\0';
  String str_payload = String(buff_p);
  Log(str_payload);

  String subTopicKey = "total";
  if (String(topic).endsWith(subTopicKey)) {
    float newValue = str_payload.toFloat();
    if (newValue > 0) {
      Log("Override value total: ");
      Log(newValue);
      gasCounter.total_liter = newValue * 1000.0f;

      formatEEPROM();
      currentEEPROMAddress = 0;
      storeToEEPROM();

      Log(">>>");
      Log(String(gasCounter.total_liter / 1000.0f));
      mqttPublish(pubTopic, subTopicKey.c_str(), String(gasCounter.total_liter / 1000.0f));
      mqttPublish(subTopic, subTopicKey.c_str(), "");  // delete the retained mqtt message
    }
    return;
  }

  subTopicKey = "waitForOTA";
  if (String(topic).endsWith(subTopicKey)) {
    if (str_payload.startsWith("true")
        || str_payload.startsWith("yes")) {
      Log("Disabling temporarly turningOff/deepSleep");
      turningOff = false;
      setupOTA();
      otaEnabled = true;
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

void formatEEPROM() {
  Log("formatEEPROM");
  for (int i = 0; i < EEPROM_SIZE_BYTES_MAX; i++) {
    EEPROM.write(i, 255);
    char value = EEPROM.read(i);
    if (value != 255) {
      Log("EEPROM CHECK FAILED @");
      Log(i);
    }
  }
}

void findLastEEPROMAddress() {
  Log("### EEPROM DUMP ###");
  for (; currentEEPROMAddress < EEPROM_SIZE_BYTES_MAX; currentEEPROMAddress++) {
    // read a byte from the current address of the EEPROM
#if debug == true
    char value = EEPROM[currentEEPROMAddress];
    Log(currentEEPROMAddress);
    Log("\t");
    Log(value);
    Log();
#endif
    // read until we find something different as MAGIC_BYTE
    // e.g. ########47110815#####...
    //              ^
    if (EEPROM[currentEEPROMAddress] != MAGIC_BYTE) {
      currentEEPROMAddress--;  // skip one byte back for start write offset
      break;
    }
  }

  if (currentEEPROMAddress < 0 || currentEEPROMAddress >= EEPROM_SIZE_BYTES_MAX) {  // empty/new EEPROM
    currentEEPROMAddress = -1;                                                      // will be set to 0 in increaseEEPROMAddress
  }

  Log("Current EEPROM address (-1 -> nothing found): ");
  Log(currentEEPROMAddress);
}

void increaseEEPROMAddress() {
  currentEEPROMAddress++;
  if (currentEEPROMAddress >= EEPROM_SIZE_BYTES_MAX - sizeofGasCounterLong) {
    // we start from the beginning
    currentEEPROMAddress = 0;
  }
  Log("New EEPROM address: ");
  Log(currentEEPROMAddress);
}

void storeToEEPROM() {
  Log("storeToEEPROM GasCounter: ");
  Log(gasCounter.total_liter);
  EEPROM.put(currentEEPROMAddress, MAGIC_BYTE);

  char buffer[10];
  sprintf(buffer, "%10lu", gasCounter.total_liter);
  EEPROM.put(currentEEPROMAddress + 1, buffer);

  EEPROM.commit();
}

void loadFromEEPROM() {
  Log("Read EEPROM address: ");
  Log(currentEEPROMAddress);
  char magicByteRead = EEPROM.read(currentEEPROMAddress);
  Log(magicByteRead);
  if (magicByteRead != MAGIC_BYTE) {
    Log("No MAGIC_BYTE found at ");
    Log(currentEEPROMAddress);
  } else {
    char buffer[10];
    EEPROM.get(currentEEPROMAddress + 1, buffer);
    gasCounter.total_liter = String(buffer).toInt();  // returns long
    Log("loadFromEEPROM GasCounter: ");
    Log(gasCounter.total_liter);
  }

  if (gasCounter.total_liter < 0 || isnan(gasCounter.total_liter)) {
    Log("!!! Resetting gasCounter.total_liter to 0");
    gasCounter.total_liter = 0;
  }
}