#define CO_MYMETER_NAME "mymeter"  // unique name of your device, e.g. gasmeter (used for mqtt client id and OTA network name)

#define CO_MQTT_BROKER_IP "192.168.?.?"          // ip of your mqtt broker/server
#define CO_MQTT_BROKER_PORT 8883                  // 8883 via SSL/TLS, 1883 plain
#define CO_MQTT_TOPIC_MAIN_FOLDER_PUB "home/"     // mqtt main folder for e.g. home/mymeter
#define CO_MQTT_TOPIC_SUB_FOLDER_SUB "/settings"  // mqtt sub folder for home/mymeter/settings/config, e.g. waitForOTA, total or voltageCalibration

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

#define debug true  // true = enable debug messages (and much slower processing), or false = disable debug messages