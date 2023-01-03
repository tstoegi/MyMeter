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
  A: Send/publish a mqtt message (with retain!) to "haus/gasmeter/settings/total_m3" e.g. "202.23" (just once) - a empty value or 0.00 will be ignored
  
  todo:
  + Store the total amount locally somewhere (e.g. eeprom)
  (c) 2022, 2023 @tstoegi, Tobias Stöger
  
 ***************************************************************************/

#include <Arduino.h>

#include <EEPROM.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>

#include <ArduinoOTA.h>

#ifndef USE_MICROWAKEUPPER_SHIELD
#define USE_MICROWAKEUPPER_SHIELD true  // !!! change this to false if you want Variant A !!!
#endif

#ifdef USE_MICROWAKEUPPER_SHIELD
#include <MicroWakeupper.h>
MicroWakeupper microWakeupper;  //MicroWakeupper instance (only one is supported!)
bool launchedByMicroWakeupperEvent = false;
#endif

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
char msg[50];

#include <Credentials.h>  // located (or create one) in folder "Arduino/libraries/Credentials/"
/* example

// your wifi
#define CR_WIFI_SSID        "wifi_ssid"
#define CR_WIFI_PASSWORD    "wifi_password"

// ota - over the air firmware updates - userid and password of your choise
#define CR_OTA_GASMETER_CLIENT_ID_PREFIX       "gasmeter_"
#define CR_OTA_GASMETER_CLIENT_PASSWORD        "0123456789"

// your mqtt server cert fingerprint -> use "" if you want to disable cert checking
#define CR_MQTT_BROKER_CERT_FINGERPRINT "AA F0 DE 66 E7 22 98 02 12 1D 59 08 4B 32 23 24 C9 F4 D1 00"

// your mqtt user and password as created on the server side
#define CR_MQTT_BROKER_GASMETER_USER "gasmeter"
#define CR_MQTT_BROKER_GASMETER_PASSWORD "0123456789"

*/


// $$$config$$$
#define CO_MQTT_BROKER_IP "192.168.4.3"
#define CO_MQTT_BROKER_PORT 8883  // 8883 via SSL/TLS, 1883 plain
#define CO_MQTT_GASMETER_TOPIC_PUB "haus/gasmeter"
#define CO_MQTT_GASMETER_TOPIC_SUB "haus/gasmeter/settings/#"
#define CO_MQTT_GASMETER_CLIENT_ID_PREFIX "gasmeter_"  // + ip address added by code
// $$$config$$$

const float oneround_m3 = 0.01f;  // one complete round 0.01m3 (or 10 liter of gas)

struct GasCounter {
  float total_m3;
};
GasCounter gasCounter = { 0.00f };  // todo set initial value via mqtt subscribe

// we store the new value in EEPROM always at currentEEPROMAddress+1 to spread max write-life-cycles of the whole EEPROM - at the end we start from 0 again
int sizeofStructGasCounter = 10;  // do not use sizeof!!! it will not work on structs

int currentEEPROMAddress = 0;
#define MAGIC_BYTE '#'
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

enum State {
  state_startup = 0,
  state_setupWifi = 1,
  state_setupOTA = 2,
  state_setupMqtt = 3,
  state_checkSensorData = 4,
  state_sendMqtt = 5,
  state_turingOff = 6,
  state_turnedOff = 7
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
  while (millis() - prevMillis >= nextStateDelaySeconds * 1000) {
    prevMillis = millis();
    doNextState(nextState);
  }
  delay(1000);
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
        Serial.println(gasCounter.total_m3);
        loadFromEEPROM(gasCounter);
        Serial.println(gasCounter.total_m3);

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
        Serial.println(gasCounter.total_m3);
        gasCounter.total_m3 = gasCounter.total_m3 + oneround_m3;
        Serial.println(oneround_m3);
        Serial.println(gasCounter.total_m3);
        if (launchedByMicroWakeupperEvent) {
          setNextState(state_sendMqtt);
        } else {
          setNextState(state_turingOff);
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

        mqttPublish(String(gasCounter.total_m3), "total_m3");
        mqttPublish(String(rssi), "wifi_rssi");

#ifdef USE_MICROWAKEUPPER_SHIELD
        mqttPublish(String(microWakeupper.readVBatt()), "batteryVoltage");

        setNextState(state_turingOff);
#else
        setNextState(state_checkSensorData);
#endif
        break;
      }
    case state_turingOff:
      {
        Serial.println("state_turingOff");
        digitalWrite(LED_BUILTIN, true);  // turn led off
        increaseEEPROMAddress();
        storeToEEPROM(gasCounter);
        EEPROM.end();

#ifdef USE_MICROWAKEUPPER_SHIELD
        Serial.println("Waiting for turning device off (J1 on MicroWakeupperShield cutted!)");
        microWakeupper.reenable();
        deepSleep();
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

  if (aNewState > state_setupMqtt && aNewState < state_turingOff) {
    if (!mqttClient.loop()) {  //needs to be called regularly
      mqttReconnect();
    }

    for (int i = 0; i < 100; i++) {  // todo Hack - if more incoming messages are queued
      mqttClient.loop();
    }
  }

  delay(100);
}
void mqttPublish(String msg, const char* subTopic) {
  String topicString = String(CO_MQTT_GASMETER_TOPIC_PUB) + "/" + subTopic;
  mqttClient.publish(topicString.c_str(), msg.c_str(), true);  //We send with "retain"
  Serial.print(">> Published message: ");
  Serial.print(topicString);
  Serial.print(" ");
  Serial.println(msg);
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

      mqttClient.subscribe(CO_MQTT_GASMETER_TOPIC_SUB);
      Serial.print("mqtt subscription topic: ");
      Serial.println(CO_MQTT_GASMETER_TOPIC_SUB);

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

  if (String(topic).endsWith("total_m3")) {
    float newValue = str_payload.toFloat();
    if (newValue > 0.00) {
      Serial.print("Override value total_m3: ");
      Serial.println(newValue);
      gasCounter.total_m3 = newValue;
    }
  }

  // republish changed values
  setNextState(state_sendMqtt);
}

void deepSleep() {
  mqttClient.disconnect();

  delay(100);

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(1);
  WiFi.forceSleepBegin();

  Serial.println("Going into deepSleep now");
  ESP.deepSleepMax();  // around 71 minutes
  delay(200);          // Seems recommended after calling deepSleep
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

  if (currentEEPROMAddress < 0) {  // empty/new EEPROM
    currentEEPROMAddress = -1;     // will be set to 0 in
  }

  Serial.print("Found EEPROM address: ");
  Serial.println(currentEEPROMAddress);
}

void increaseEEPROMAddress() {
  currentEEPROMAddress++;
  if (currentEEPROMAddress > EEPROM_SIZE_BYTES_MAX - sizeofStructGasCounter) {
    // we start from the beginning
    currentEEPROMAddress = 0;
  }
  Serial.print("New EEPROM address: ");
  Serial.println(currentEEPROMAddress);
}

void storeToEEPROM(GasCounter gasCounter) {
  Serial.print("storeToEEPROM GasCounter: ");
  Serial.println(gasCounter.total_m3);
  EEPROM.write(currentEEPROMAddress, MAGIC_BYTE);
  EEPROM.put(currentEEPROMAddress + 1, gasCounter);
  EEPROM.commit();
}

void loadFromEEPROM(GasCounter& gasCounter) {
  Serial.print("Read EEPROM address: ");
  Serial.print(currentEEPROMAddress);
  char magicByteRead = EEPROM.read(currentEEPROMAddress);
  Serial.println(magicByteRead);
  if (magicByteRead != MAGIC_BYTE) {
    Serial.print("No MAGIC_BYTE found at ");
    Serial.print(currentEEPROMAddress);
  } else {
    EEPROM.get(currentEEPROMAddress + 1, gasCounter);
    Serial.print("loadFromEEPROM GasCounter: ");
  }
  Serial.println(gasCounter.total_m3);

  if (gasCounter.total_m3 < 0.0 || isnan(gasCounter.total_m3)) {
    Serial.println("!!! Resetting gasCounter.total_m3 to 0.00");
    gasCounter.total_m3 = 0.00f;
  }
}