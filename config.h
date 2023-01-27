#define CO_MQTT_BROKER_IP "192.168.?.???"  // ip of your mqtt broker/server
#define CO_MQTT_BROKER_PORT ????           // 8883 via SSL/TLS, 1883 plain
#define CO_MQTT_TOPIC_PUB "haus/gasmeter"   // mqtt folder (readonly)
#define CO_MQTT_TOPIC_SUB "haus/gasmeter/settings"  // mqtt folder for settings/config, e.g. waitForOTA, total or voltageCalibration
#define CO_MQTT_CLIENT_ID_PREFIX "gasmeter_"        // mqtt client id (+ip address added at runtime), e.g. "gasmeter_192.168.1.69"