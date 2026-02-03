
# Arduino Energy and Gas Meter Monitoring with a Wemos D1 Mini over MQTT

MyMeter is an ESP8266-based energy and gas meter monitoring system that counts pulses from sensors and reports via MQTT. It uses a MicroWakeupper battery shield for power-efficient operation with deep sleep between measurements.

## Energy Meter
This is how your basic setup should look if you are using an IR Sensor breakout board to count each cycle:

```
RED - VIN - SWITCH_OUT (3V3)
BLACK - GND
ORANGE - SIGNAL - SWITCH_IN
```

![Alt IR Sensor Setup](pics/IRSensor.jpeg "IR Sensor")

## Gas Meter
### Setup A (Recommended for Battery Operation)
- **Reed switch** connected to SWITCH OUT/IN on a MicroWakeupper battery shield (stacked onto a Wemos D1 Mini).

### Setup B (External Power/USB)
- **Inductive proximity sensor LJ12A3-4-Z/BX 5V** connected with the signal line to SWITCH IN (common GND) on a MicroWakeupper battery shield (stacked onto a Wemos D1 Mini). The sensor typically consumes about 50mA (always on).

---

There are different gas meters in use, typically:
a) One with an internal magnet that turns each cycle—allowing the use of a reed switch for counting.
b) One with an internal metal plate that turns each cycle—allowing the use of a proximity sensor LJ12A3-4-Z/BX 5V.

Usually, one full rotation equals 0.01m³ (or 10 liters) of gas—check your gas meter for specifics.

The stacked MicroWakeupper shield turns your Wemos D1 Mini on and off. **Recommended:** Cut the onboard jumper J1.

**TL;DR:** The firmware counts each cycle, writes the total to LittleFS, and sends an MQTT message to your broker with the total amount (total), WiFi RSSI, and battery voltage.

## Setup/Installation

### Required Libraries
Install these libraries in your Arduino IDE:
- [MicroWakeupper](https://github.com/tstoegi/MicroWakeupper)
- PubSubClient (MQTT)
- WiFiManager (by tzapu)
- ArduinoJson
- ESP8266WiFi, ESP8266mDNS, ArduinoOTA, LittleFS (ESP8266 core)

### Configuration Files
1. **config.h** - Device name, static IP options, debug flags
   - Set your device name (default: "MyMeter")
   - Enable/disable `DEBUG` for serial logging (disable for production)
   - Optional: Configure `STATIC_IP` and `STATIC_WIFI` for faster connection

2. **credentials.h** - Copy from `credentials.h.example`
   - OTA password
   - Optional: Set MQTT TLS fingerprint
   - Optional: Pre-configure WiFi and MQTT defaults (see below)

### Setup Methods

MyMeter supports **two configuration approaches**:

#### Option A: Runtime Configuration (Default - Recommended for most users)
Use the web portal to configure WiFi and MQTT settings. This is the easiest method and allows changing settings without reflashing.

See **"First-Time Setup"** section below.

#### Option B: Compile-Time Pre-Configuration (Advanced)
Pre-configure devices with WiFi and MQTT settings at compile time. Useful for:
- Flashing multiple identical devices
- Avoiding manual configuration steps
- Corporate/controlled deployments

**How to use:**
1. Copy `credentials.h.example` to `credentials.h`
2. Uncomment and fill in the `DEFAULT_*` defines:
```cpp
#define DEFAULT_WIFI_SSID "YourWiFiSSID"
#define DEFAULT_WIFI_PASSWORD "YourWiFiPassword"
#define DEFAULT_MQTT_BROKER "192.168.1.100"
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_USER "mqtt_user"
#define DEFAULT_MQTT_PASSWORD "mqtt_password"
#define DEFAULT_MQTT_TOPIC "home/mymeter"
```
3. Compile and flash
4. Device starts immediately with these settings (no config portal needed)

**Note:** If `config.json` already exists in LittleFS, these defaults are ignored. To force new defaults, either:
- Perform a factory reset via config portal (6x reset → Factory Reset button)
- Erase flash before uploading: `esptool.py erase_flash`

You can still open the config portal later (6x reset) to modify settings.

### First-Time Setup (Runtime Configuration)
1. Flash the firmware to your Wemos D1 Mini
2. The device creates a WiFi access point named `MyMeter-XXXX-Setup` (XXXX = last 4 digits of MAC)
3. Connect to this AP and navigate to `192.168.4.1`
4. Configure through the web portal:
   - **WiFi**: Select network and enter password
   - **MQTT**: Broker IP, port, credentials, topic
   - **OTA**: Set password for secure OTA updates (leave empty to disable)
   - **Device Settings**:
     - Change device name (e.g., "Gasmeter", "Stromzaehler")
     - Set initial counter value
5. Click "Save" - device reboots and connects to your network

### Accessing Configuration Portal Later
Reset the device **6 times within 10 seconds** to force the config portal to open (Tasmota-style). This is useful for:
- Changing WiFi networks
- Updating MQTT settings
- Modifying device name or counter value

### OTA Updates

**Method 1: Via MQTT**
- Publish `true` to `<topic>/settings/waitForOTA`
- OTA will be available for 2 minutes
- Find device as `[DeviceName]_[IP_address]` in Arduino IDE

**Method 2: Via Multi-Reset**
- Reset device 3 times quickly (within 10 seconds)
- OTA enabled automatically

**Method 3: Via ota.sh Script**
```bash
./ota.sh -ip <ESP_IP> -f build/MyMeter.ino.bin -pw <OTA_PASSWORD>
```

**Warning:** As long as you power the Wemos via USB (or external VIN), the MicroWakeupper shield cannot turn it off.

All MQTT messages from the client (Wemos) are sent with the "retain" flag—so you see the last messages even if the device is off.

## Web Portal Features

The configuration portal provides an organized interface with the following sections:

### MQTT Configuration
- Broker IP address
- Port (default: 1883)
- Username and password (with show/hide toggle)
- MQTT topic path (e.g., `haus/gasmeter`)

### OTA Configuration
- OTA password (with show/hide toggle)
  - Leave empty to disable authentication
  - Set password for secure OTA updates
- Enable OTA after reboot checkbox

### Device Settings
- Device name (changes AP name, OTA hostname, MQTT client ID)
- Current WiFi connection info
- Counter total value (editable for initial setup or corrections)
- Factory reset option

## MQTT Topics

**Published by device:**
- `<topic>/total` - Current counter value (m³ or kWh)
- `<topic>/wifi_rssi` - WiFi signal strength
- `<topic>/batteryVoltage` - Current battery voltage
- `<topic>/version` - Firmware version (only in DEBUG mode)
- `<topic>/localIP` - Device IP address (only in DEBUG mode)

**Subscribed by device:**
- `<topic>/settings/total` - Set counter value (e.g., `"202.23"`)
- `<topic>/settings/waitForOTA` - Enable OTA mode (`"true"`)
- `<topic>/settings/voltageCalibration` - Adjust battery reading (e.g., `"+0.3"`)

## FAQ

**Q: Where can I buy the MicroWakeupper battery shield?**
A: [My store](https://www.tindie.com/stores/moreiolabs/)

**Q: How can I set an initial counter value?**
A: **Option 1 (Recommended):** Open the config portal (6x reset) and edit "Counter Total" field directly.
**Option 2:** Send/publish an MQTT message (with retain!) to `<topic>/settings/total`, e.g., `"202.23"`. After receiving that message, it is removed, and you should see the new value.

**Q: How can I enable OTA updates?**
A: **Option 1:** Reset device 3 times within 10 seconds.
**Option 2:** Send/publish an MQTT message (with retain!) to `<topic>/settings/waitForOTA` with `"true"`.
**Option 3:** Enable "OTA after reboot" in the config portal.

**Q: How can I adjust (calibrate) the battery voltage value?**
A: Send/publish an MQTT message (with retain!) to `<topic>/settings/voltageCalibration`, e.g., `"+0.3"` or `"-0.5"` volts. The message has to stay there!

**Q: How do I change the device name?**
A: Open the config portal (6x reset) and edit the "Device Name" field in Device Settings. This changes the WiFi AP name, OTA hostname, and MQTT client ID.

**Q: I forgot my OTA password, how can I reset it?**
A: Open the config portal (6x reset) and set a new OTA password, or leave it empty to disable authentication.

## Build Configuration

### Debug vs. Release Mode
Edit `config.h` to toggle debug mode:

**Debug Mode (Development):**
```cpp
#define DEBUG  // Enable serial logging
```
- Detailed serial output at 115200 baud
- Version and IP published via MQTT
- Slower execution
- Uses ~2KB more RAM

**Release Mode (Production):**
```cpp
//#define DEBUG  // Disabled for release
```
- No serial logging
- Faster execution
- Lower power consumption
- Recommended for battery operation

### Optional Features
```cpp
#define STATIC_IP      // Use static IP for faster WiFi connection
#define STATIC_WIFI    // Use static BSSID/channel for faster connection
#define MQTT_TLS       // Enable TLS for MQTT (requires fingerprint in credentials.h)
```

---

(c) 2022-2026 @tstoegi, Tobias Stöger, MIT License
