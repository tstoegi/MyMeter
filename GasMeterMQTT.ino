/***************************************************************************
  Gas Meter Sensor via mqtt (default is variant B!)
  + Wemos D1 Mini
  
  *** Variant A: With inductive proximity sensor LJ12A3-4-Z/BX 5V connected via voltage divider to A0
  How it works: 
  For each sensor signal change, the firmware will sent one mqtt message to your broker/Nodered - you can count each message and/or e.g. forward it to Grafana aso
  + Powered via USB
  + Voltage divider: (Sensor BK)->5V---R10K---A0---R22K---GND
  
  *** Variant B: With reed switch connected on a MicroWakeupper battery shield (stacked to a Wemos D1 mini)
  How it works:
  Your gas meter has an internal magnet that is turing around - connect a reed switch and you are able to "count" one turn (1 = 0,01 m3).
  The MicroWakeupper shield is turing your Wemos on if the reed is switched on (NO switch - you use a NC switched alternatively - change the on board switch apropiate on the shield).
  The firmware will sent one mqtt message to your broker/Nodered - you can count each message and/or e.g. forward it to Grafana aso
  
  (Hardware part)
  + Cut J1 carefully on the backside of your MicroWakeupper shield - this will power on/off the Wemos
  + Connect a reed switch to the MicroWakeupper to SWITCH IN/OUT
  + Stack the MicroWakeupper shield onto your Wemos
  + Connect/power the MicroWakeupper battery shield  with a lipo - recommended: a protected one
  
  (Software part)
  + Install the MicroWakeupper library to your Arduino SDK https://github.com/tstoegi/MicroWakeupper
  + Below: Set USE_MICROWAKEUPPER_SHIELD from false to true 
  + Below: Setup all config data between // $$$config$$$
  + Connected the Wemos via USB and upload the code (without the MicroWakeupper shield stacked or the FLASH button pressed during upload) 
  + Warning: As long as you power the Wemos via USB it will not turn off
  + Warning: OTA will not work on battery with device off
  
  faq:
  Q: Where can I buy the MicroWakeupper battery shield?
  A: My store: https://www.tindie.com/stores/moreiolabs/

  Q: How can I set an initial counter value?
  A: Send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/total_m3" e.g. "202.23" - after receiving there is a response with "0"

  Q: How can I install OTA update (within the Arduino SDK)?
  A: Send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/turningOff" e.g. "false" - after receiving the Wemos will stay online until the next restart or external reset
  
  todo:
  + Store the total amount locally somewhere (e.g. eeprom)
  (c) 2022, 2023 @tstoegi, Tobias Stöger
  
 ***************************************************************************/



#include <Arduino.h>

#ifndef USE_MICROWAKEUPPER_SHIELD
#define USE_MICROWAKEUPPER_SHIELD true  // !!! change this to false if you want Variant A !!!
#endif

#include <Credentials.h>  // located (or create one) in folder "Arduino/libraries/Credentials/"
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


// $$$config$$$
#define CO_MQTT_BROKER_IP "192.168.4.3"
#define CO_MQTT_BROKER_PORT 8883  // 8883 via SSL/TLS, 1883 plain
#define CO_MQTT_GASMETER_TOPIC_PUB "haus/gasmeter"
#define CO_MQTT_GASMETER_TOPIC_SUB "haus/gasmeter/settings"  // we subscribe to + "/#"
#define CO_MQTT_GASMETER_CLIENT_ID_PREFIX "gasmeter_"        // + ip address added by code
// $$$config$$$


#include <EEPROM.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>

#include <ArduinoOTA.h>

#ifdef USE_MICROWAKEUPPER_SHIELD
#include <MicroWakeupper.h>
MicroWakeupper microWakeupper;  //MicroWakeupper instance (only one is supported!)
//microWakeupper.setVoltageDivider(float);    // calibration: A0 read (ADO max 1024) - this will override VOLTAGEDIVIDER_DEFAULT
bool launchedByMicroWakeupperEvent = false;
#endif

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
char msg[50];

const long one_turnaround_liter = 10;  // one complete round 10 liter of gas (or 0.01 m3)

struct GasCounter {
  long total_liter;
};
GasCounter gasCounter = { 0 };  // todo set initial value via mqtt subscribe

// we store the new value in EEPROM always at currentEEPROMAddress+1 to spread max write-life-cycles of the whole EEPROM - at the end we start from 0 again
int sizeofGasCounterLong = 10;

int currentEEPROMAddress = 0;  // first address we try to read a valid value
#define MAGIC_BYTE '#'         // start mark of our value in EEPROM "#1234567890" or "#  14567800" (max 10 digits)
#define EEPROM_SIZE_BYTES_MAX 512

#ifdef USE_MICROWAKEUPPER_SHIELD
// nothing to see here
#else
// inductive proximity sensor LJ12A3-4-Z/BX 5V connected via voltage divider to A0
#define SIGNAL_SENSOR A0
#define SIGNAL_HYST_MIN 450
#define SIGNAL_HYST_MAX 550
#define SIGNAL_CHECK_EVERY_SEC 1

#endif

long rssi = 0;  // wifi signal strength

bool turningOff = true;

enum State {
  state_startup = 0,
  state_setupWifi = 1,
  state_setupOTA = 2,
  state_setupMqtt = 3,
  state_checkSensorData = 4,
  state_sendMqtt = 5,
  state_idle = 6,
  state_turningOff = 7,
  state_turnedOff = 8
} nextState;

int nextStateDelaySeconds = 0;

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE_BYTES_MAX);  // mandator for esp8266 (because EEPROM is emulated for FLASH)

  delay(100);
  findLastEEPROMAddress();

  Serial.println("\n Gas Meter Monitoring");

  nextState = state_startup;
  nextStateDelaySeconds = 0;
}

void loop() {
  if (nextState > state_setupOTA) {
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
  Serial.print("___ doNextState() ___: ");
  Serial.println(aNewState);

  switch (aNewState) {
    case state_startup:
      {
        Serial.println("state_startup");
        pinMode(LED_BUILTIN, OUTPUT);
        Serial.println(gasCounter.total_liter);
        loadFromEEPROM();
        Serial.println(gasCounter.total_liter);

#ifdef USE_MICROWAKEUPPER_SHIELD
        Serial.println("USE_MICROWAKEUPPER_SHIELD true");
        microWakeupper.begin();  // For correct initialisation
        if (microWakeupper.resetedBySwitch() && microWakeupper.isActive()) {
          Serial.println("Launched by a MicroWakeupperEvent");
          launchedByMicroWakeupperEvent = true;
          microWakeupper.disable();  // Preventing new triggering/resets
        }
#endif

        setNextState(state_setupWifi);
        break;
      }
    case state_setupWifi:
      {
        Serial.println("state_setupWifi");
        setupWifi();
        setNextState(state_setupOTA);
        break;
      }
    case state_setupOTA:
      {
        Serial.println("state_setupOTA");
        setupOTA();
        setNextState(state_setupMqtt);
        break;
      }
    case state_setupMqtt:
      {
        Serial.println("state_setupMqtt");
        setupMqtt();
        mqttReconnect();
        setNextState(state_checkSensorData);
        break;
      }
    case state_checkSensorData:
      {
        Serial.println("state_checkSensorData");

#ifdef USE_MICROWAKEUPPER_SHIELD

        // one start means one turn around gasmeter
        Serial.println(gasCounter.total_liter);
        gasCounter.total_liter = gasCounter.total_liter + one_turnaround_liter;
        Serial.println(one_turnaround_liter);
        Serial.println(gasCounter.total_liter);
        if (launchedByMicroWakeupperEvent) {
          setNextState(state_sendMqtt);
        } else {
          setNextState(state_turningOff);
        }
#else  // we use a sensor with active-low and inactive-high signal (reverse)
        int adc = analogRead(SIGNAL_SENSOR);
        Serial.println(adc);

        digitalWrite(LED_BUILTIN, false);
        delay(50);
        digitalWrite(LED_BUILTIN, true);

        static bool active = false;
        if (!active
            && adc < SIGNAL_HYST_MIN) {
          active = true;
        }
        Serial.println(active ? "Sensor is on" : "Sensor is off");
        if (active && adc > SIGNAL_HYST_MAX) {
          active = false;
          Serial.println(active ? "Sensor is on" : "Sensor is off");
          setNextState(state_sendMqtt);
          break;  // done
        }
        digitalWrite(LED_BUILTIN, !active);
        setNextState(state_checkSensorData, SIGNAL_CHECK_EVERY_SEC);
        break;
#endif
      }
    case state_sendMqtt:
      {
        Serial.println("state_sendMqtt");

        mqttPublish(CO_MQTT_GASMETER_TOPIC_PUB, "total_m3", String(gasCounter.total_liter / 1000.0f));
        mqttPublish(CO_MQTT_GASMETER_TOPIC_PUB, "wifi_rssi", String(rssi));

#ifdef USE_MICROWAKEUPPER_SHIELD
        mqttPublish(CO_MQTT_GASMETER_TOPIC_PUB, "batteryVoltage", String(microWakeupper.readVBatt()));

        setNextState(state_turningOff);
#else
        setNextState(state_checkSensorData);
#endif
        break;
      }
    case state_idle:
      {
        Serial.println("state_idle (until next manual restart or external reset)");  // can be used for OTA updates
        digitalWrite(LED_BUILTIN, false);
        delay(200);
        digitalWrite(LED_BUILTIN, true);
        delay(1000);
        break;
      }
    case state_turningOff:
      {
        if (!turningOff) {
          setNextState(state_idle);
          break;
        }

        digitalWrite(LED_BUILTIN, true);  // turn led off
        increaseEEPROMAddress();
        storeToEEPROM();
        EEPROM.end();

#ifdef USE_MICROWAKEUPPER_SHIELD
        Serial.println("Waiting for turning device off (J1 on MicroWakeupperShield cutted!)");
        microWakeupper.reenable();
        delay(5000);  // time for the MicroWakeupper to power off the wemos

        deepSleep();  // if the MicroWakeupper Switch is still in an ON state, we try to sleep
        delay(1000);
        setNextState(state_turnedOff);
#else
        Serial.println("!!! Unsupported state !!!");
        dealy(1000);
#endif
        break;
      }
    case state_turnedOff:
      {
        Serial.println("state_turnedOff");
        delay(1000);
        break;
      }
    default:
      {
        Serial.println("!!! unknown status !!!");
      }
  }

  if (aNewState > state_setupMqtt && aNewState < state_turningOff) {
    if (!mqttClient.loop()) {  //needs to be called regularly
      mqttReconnect();
    }

    for (int i = 0; i < 100; i++) {  // todo Hack - if more incoming messages are queued
      mqttClient.loop();
    }
  }

  delay(100);
}

void setupWifi() {
  WiFi.forceSleepWake();
  WiFi.mode(WIFI_STA);  // <<< Station
  delay(300);
  // Wait for connection
  int retries = 5;
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
    WiFi.begin(CR_WIFI_SSID, CR_WIFI_PASSWORD);
    Serial.print(retries);
    delay(1000);
    int secondsTimeout = 10;
    while (WiFi.status() != WL_CONNECTED && secondsTimeout > 0) {
      delay(1000);
      Serial.print(".");
      secondsTimeout--;
    }
    retries--;
    Serial.println();
  }

  if (retries == 0) {
    Serial.println(CR_WIFI_SSID);
    Serial.println("Connection failed!");
    return;
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(CR_WIFI_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  rssi = WiFi.RSSI();
}

void setupOTA() {
  String ota_client_id = CR_OTA_GASMETER_CLIENT_ID_PREFIX + WiFi.localIP().toString();
  Serial.print("OTA_CLIENT_ID: ");
  Serial.println(ota_client_id);

  ArduinoOTA.setHostname(ota_client_id.c_str());

  ArduinoOTA.setPassword(CR_OTA_GASMETER_CLIENT_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

void setupMqtt() {
  if (strlen(CR_MQTT_BROKER_CERT_FINGERPRINT) > 0) {
    Serial.println("Setting mqtt server fingerprint");
    espClient.setFingerprint(CR_MQTT_BROKER_CERT_FINGERPRINT);
  } else {
    Serial.println("No fingerprint verification for mqtt");
    espClient.setInsecure();
  }
  mqttClient.setBufferSize(512);
  mqttClient.setServer(CO_MQTT_BROKER_IP, CO_MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttCallback);
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");

    // Create a random and unique client ID for mqtt
    String clientId = CO_MQTT_GASMETER_CLIENT_ID_PREFIX + WiFi.localIP().toString();
    Serial.print("MQTT_CLIENT_ID: ");
    Serial.println(clientId);

    // Attempt to connect
    if (mqttClient.connect(clientId.c_str(), CR_MQTT_BROKER_GASMETER_USER, CR_MQTT_BROKER_GASMETER_PASSWORD)) {
      Serial.println("connected");

      String subTopic = String(CO_MQTT_GASMETER_TOPIC_SUB) + "/#";
      mqttClient.subscribe(subTopic.c_str());
      Serial.print("mqtt subscription topic: ");
      Serial.println(subTopic.c_str());

      mqttClient.loop();
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void mqttPublish(const char* mainTopic, const char* subTopic, String msg) {
  String topicString = String(mainTopic) + "/" + String(subTopic);
  mqttClient.publish(topicString.c_str(), msg.c_str(), true);  //We send with "retain"
  Serial.print(">> Published message: ");
  Serial.print(topicString);
  Serial.print(" ");
  Serial.println(msg);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println();
  Serial.print("<< Received message: ");
  Serial.print(topic);
  Serial.print(" ");

  char buff_p[length];
  for (int i = 0; i < length; i++) {
    buff_p[i] = (char)payload[i];
  }
  buff_p[length] = '\0';
  String str_payload = String(buff_p);
  Serial.println(str_payload);

  String subTopic = "total_m3";
  if (String(topic).endsWith(subTopic)) {
    float newValue = str_payload.toFloat();
    if (newValue > 0) {
      Serial.print("Override value total_m3: ");
      Serial.println(newValue);
      gasCounter.total_liter = newValue * 1000.0f;

      formatEEPROM();
      currentEEPROMAddress = 0;
      storeToEEPROM();

      mqttPublish(CO_MQTT_GASMETER_TOPIC_SUB, subTopic.c_str(), "0");  // override retained mqtt message with 0 to prevent retriggering on next device restart
      mqttClient.loop();
      Serial.print(">>>");
      Serial.println(String(gasCounter.total_liter / 1000.0f));
      mqttPublish(CO_MQTT_GASMETER_TOPIC_PUB, subTopic.c_str(), String(gasCounter.total_liter / 1000.0f));
      mqttClient.loop();
    }
  }

  subTopic = "turningOff";
  if (String(topic).endsWith(subTopic)) {
    if (str_payload.startsWith("false")
        || str_payload.startsWith("no")) {
      Serial.println("Disabling temporarly turningOff/deepSleep");
      turningOff = false;
      mqttPublish(CO_MQTT_GASMETER_TOPIC_SUB, subTopic.c_str(), "true");
      mqttClient.loop();
    }
  }
}

void deepSleep() {
  mqttClient.disconnect();

  delay(100);

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(1);
  WiFi.forceSleepBegin();

  Serial.println("Going into deepSleep now");
  //ESP.deepSleep(1000); // todo does not work at the moment - is setting pin7 to high sadly :-(
  //ESP.deepSleepMax();  // around 71 minutes
  delay(200);  // Seems recommended after calling deepSleep
}

void formatEEPROM() {
  Serial.println("formatEEPROM");
  for (int i = 0; i < EEPROM_SIZE_BYTES_MAX; i++) {
    EEPROM.write(i, 255);
    char value = EEPROM.read(i);
    if (value != 255) {
      Serial.print("EEPROM CHECK FAILED @");
      Serial.println(i);
    }
  }
}

void findLastEEPROMAddress() {
  Serial.println("### EEPROM DUMP ###");
  for (; currentEEPROMAddress < EEPROM_SIZE_BYTES_MAX; currentEEPROMAddress++) {
    // read a byte from the current address of the EEPROM
    char value = EEPROM.read(currentEEPROMAddress);
    Serial.print(currentEEPROMAddress);
    Serial.print("\t");
    Serial.print(value);
    Serial.println();
    // read until we find something different as MAGIC_BYTE
    // e.g. ########47110815#####...
    //              ^
    if (value != MAGIC_BYTE) {
      currentEEPROMAddress--;  // skip one byte back for start write offset
      break;
    }
  }

  if (currentEEPROMAddress < 0 || currentEEPROMAddress >= EEPROM_SIZE_BYTES_MAX) {  // empty/new EEPROM
    currentEEPROMAddress = -1;                                                      // will be set to 0 in increaseEEPROMAddress
  }

  Serial.print("Current EEPROM address (-1 -> nothing found): ");
  Serial.println(currentEEPROMAddress);
}

void increaseEEPROMAddress() {
  currentEEPROMAddress++;
  if (currentEEPROMAddress > EEPROM_SIZE_BYTES_MAX - sizeofGasCounterLong) {
    // we start from the beginning
    currentEEPROMAddress = 0;
  }
  Serial.print("New EEPROM address: ");
  Serial.println(currentEEPROMAddress);
}

void storeToEEPROM() {
  Serial.print("storeToEEPROM GasCounter: ");
  Serial.println(gasCounter.total_liter);
  EEPROM.put(currentEEPROMAddress, MAGIC_BYTE);

  char buffer[10];
  sprintf(buffer, "%10lu", gasCounter.total_liter);
  for (int i = 0; i < sizeofGasCounterLong; i++) {
    EEPROM.put(currentEEPROMAddress + 1 + i, buffer[i]);
  }

  EEPROM.commit();
}

void loadFromEEPROM() {
  Serial.print("Read EEPROM address: ");
  Serial.print(currentEEPROMAddress);
  char magicByteRead = EEPROM.read(currentEEPROMAddress);
  Serial.println(magicByteRead);
  if (magicByteRead != MAGIC_BYTE) {
    Serial.print("No MAGIC_BYTE found at ");
    Serial.print(currentEEPROMAddress);
  } else {
    String valueAsString = "";
    for (int i = 0; i < sizeofGasCounterLong; i++) {
      char c = EEPROM.read(currentEEPROMAddress + 1 + i);
      valueAsString += c;
    }
    gasCounter.total_liter = valueAsString.toInt();  // returns long
    Serial.print("loadFromEEPROM GasCounter: ");
    Serial.println(gasCounter.total_liter);
  }

  if (gasCounter.total_liter < 0 || isnan(gasCounter.total_liter)) {
    Serial.println("!!! Resetting gasCounter.total_liter to 0");
    gasCounter.total_liter = 0;
  }
}