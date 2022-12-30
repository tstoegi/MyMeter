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
  + Connected the Wemos via USB and upload the code (without the MicroWakeupper shield stacked or the FLASH button pressed during upload) 
  + Warning: As long as you power the Wemos via USB it will not turn off
  + Warning: OTA will not work on battery with device off
  
  faq:
  Q: Where can I buy the MicroWakeupper battery shield?
  A: My store: https://www.tindie.com/stores/moreiolabs/
  
  todo:
  + Store the total amount locally somewhere (e.g. eeprom)

  (c) 2022, 2023 @tstoegi, Tobias Stöger
  
 ***************************************************************************/

#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>

#include <ArduinoOTA.h>
#define OTA_CLIENT_PASSWORD "your ota password"

#ifndef USE_MICROWAKEUPPER_SHIELD
#define USE_MICROWAKEUPPER_SHIELD true // !!! change this to false if you want Variant A !!!
#endif

#ifdef USE_MICROWAKEUPPER_SHIELD
#include <MicroWakeupper.h>
MicroWakeupper microWakeupper;  //MicroWakeupper instance (only one is supported!)
bool launchedByMicroWakeupperEvent = false; 
#endif


WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
char msg[50];

#include <Credentials.h>
/* Credentials.h should look like
  #define mySSID "yourSSID"
  #define myPASSWORD "1234567890"

  #define below value "..." aso...
*/

//mqtt_server
const char* mqtt_server = myMQTTBroker_Server;
const unsigned int mqtt_port = myMQTTBroker_Port;

const char* mqtt_fprint = myMQTTBroker_Cert_FPrint;

//mqtt_user
const char* mqtt_user = myMQTTBroker_User_ESP_gasmeter;
const char* mqtt_pass = myMQTTBroker_Pass_ESP_gasmeter;

//mqtt_topic
String mqttTopic_out = "haus/gasmeter";
String clientId = "gasmeter_";

const float m3_oneround = 0.01;  // one complete round 0.01m3 (or 10 liter of gas)

// todo store locally somewhere
// float m3_total = 0.0;            // total overall

#ifdef USE_MICROWAKEUPPER_SHIELD
  // nothing to see here
#else
  // inductive proximity sensor LJ12A3-4-Z/BX 5V connected via voltage divider to A0
#define SIGNAL_SENSOR A0
#define SIGNAL_HYST_MIN 450
#define SIGNAL_HYST_MAX 550
#define SIGNAL_CHECK_EVERY_SEC 1

#endif


enum State {
  state_startup = 0,
  state_setupWifi = 1,
  state_setupOTA = 2,
  state_setupMqtt = 3,
  state_checkSensorData = 4,
  state_sendMqtt = 5,
  state_turingOff = 6
} nextState;

int nextStateDelaySeconds = 0;

void setup() {
  Serial.begin(115200);
  delay(100);
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
        setNextState(state_checkSensorData);
        break;
      }
    case state_checkSensorData:
      {
        Serial.println("state_checkSensorData");

#ifdef USE_MICROWAKEUPPER_SHIELD
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
        setNextState(state_checkSensorData, SIGNAL_CHECK_EVERY_SEC);
        digitalWrite(LED_BUILTIN, !active);
        break;
#endif
      }
    case state_sendMqtt:
      {
        Serial.println("state_sendMqtt");
        mqttPublish(String(m3_oneround), "m3used");

        // todo store locally somewhere
        // m3_total += m3_oneround;
        // mqttPublish(String(m3_total), "m3total");

        static unsigned long lastMillis = millis();
        unsigned long duration = millis() - lastMillis;
        lastMillis = millis();
        mqttPublish(String(duration), "durationInMillis");

#ifdef USE_MICROWAKEUPPER_SHIELD
        setNextState(state_turingOff);
#else
        setNextState(state_checkSensorData);
#endif
        break;
      }
    case state_turingOff:
      {
#ifdef USE_MICROWAKEUPPER_SHIELD
        Serial.println("Waiting for turning device off (J1 on MicroWakeupperShield cutted!)");
        microWakeupper.reenable();
        delay(1000);
#else
        Serial.println("!!! Unsupported state !!!");
        dealy(1000);
#endif
        break;
      }
    default:
      {
        Serial.println("!!! unknown status !!!");
      }
  }

  if (aNewState > state_setupMqtt) {
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
  Serial.print(">> Published message: ");
  String topicString = mqttTopic_out + "/" + subTopic;
  Serial.print(topicString);
  Serial.print(" ");
  Serial.println(msg);
  mqttClient.publish(topicString.c_str(), msg.c_str(), true);  //We send with "retain"
}

void setupWifi() {
  WiFi.forceSleepWake();
  WiFi.mode(WIFI_STA);  // <<< Station
  delay(300);
  // Wait for connection
  int retries = 5;
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
    WiFi.begin(mySSID, myPASSWORD);
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
    Serial.println(mySSID);
    Serial.println("Connection failed!");
    return;
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(mySSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setupOTA() {
  String clientID = clientId + WiFi.localIP().toString();
  Serial.println(clientID);
  ArduinoOTA.setHostname(clientID.c_str());

  ArduinoOTA.setPassword(OTA_CLIENT_PASSWORD);

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
  if (strlen(mqtt_fprint) > 0) {
    Serial.println("Setting mqtt server fingerprint");
    espClient.setFingerprint(mqtt_fprint);
  } else {
    Serial.println("No fingerprint verification for mqtt");
    espClient.setInsecure();
  }
  mqttClient.setServer(mqtt_server, mqtt_port);
  // mqttClient.setCallback(mqttCallback);
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");

    // Create a random client ID
    clientId += WiFi.localIP().toString();  // mqtt clientId must be unique !!!

    // Attempt to connect
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("connected");

      // mqttClient.subscribe(mqttTopicOut.c_str());
      // Serial.println(mqttTopicOut.c_str());

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