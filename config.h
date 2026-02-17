// Device name - used for WiFi AP name, MQTT client ID, OTA hostname, and MQTT topic
// Examples: "gasmeter", "stromzaehler", "wasserzaehler"
#define CO_MYMETER_NAME "MyMeter"

// NOTE: MQTT broker settings are now configured via the web portal.
// On first boot (or 3x reset), connect to the "<name>-Setup" WiFi AP
// and configure your MQTT broker, port, user, password, and topic.

// #define STATIC_IP  // uncomment this line, if you want to use a static IP (for faster WiFi connection)
#ifdef STATIC_IP
#define STATIC_IP_ADDR 192, 168, 4, 87  // device static IP (comma-separated octets)
#define STATIC_GATEWAY 192, 168, 4, 1
#define STATIC_SUBNET 255, 255, 255, 0
#define STATIC_DNS 192, 168, 4, 1
#endif

// #define STATIC_WIFI  // uncomment this line, if you want to use a static WIFI (dedicated BSSID and Channel ID)
#ifdef STATIC_WIFI
byte bssid[] = { 0x81, 0x2A, 0xA2, 0x1A, 0x0B, 0xE7 };  // the bssid can be your routers mac address - just "can"! - see debug logging "BSSID: "
int channel = 1;                                        // try to find out your channel id with getWiFiChannel(CR_WIFI_SSID);
#endif

// uncomment for enabling serial debug messages (and much slower processing)
// #define DEBUG  // uncomment for enabling serial debug messages (and much slower processing)

// uncomment for enabling mqtt tls support
//#define MQTT_TLS
