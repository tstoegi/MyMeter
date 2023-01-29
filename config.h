#define CO_MQTT_BROKER_IP "192.168.?.??"            // ip of your mqtt broker/server
#define CO_MQTT_BROKER_PORT 8883                    // 8883 via SSL/TLS, 1883 plain
#define CO_MQTT_TOPIC_PUB "haus/gasmeter"           // mqtt folder (readonly)
#define CO_MQTT_TOPIC_SUB "haus/gasmeter/settings"  // mqtt folder for settings/config, e.g. waitForOTA, total or voltageCalibration
#define CO_MQTT_CLIENT_ID_PREFIX "gasmeter_"        // mqtt client id (+ip address added at runtime), e.g. "gasmeter_192.168.1.69"

// #define STATIC_IP  // uncomment this line, if you want to use a static IP (for faster WiFi connection)
#ifdef STATIC_IP
IPAddress ip(192, 168, 4, 88);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 4, 1);
#endif

// #define STATIC_WIFI  // uncomment this line, if you want to use a static WIFI (dedicated BSSID and Channel ID)
#ifdef STATIC_WIFI
byte bssid[] = { 0x81, 0x2A, 0xA2, 0x1A, 0x0B, 0xE7 };  // the bssid can be your routers mac address - just "can"! - see debug logging "BSSID: "
int channel = 1;                                        // try to find out your channel id with getWiFiChannel(CR_WIFI_SSID);
#endif
