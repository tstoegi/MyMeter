# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MyMeter is an ESP8266-based (Wemos D1 Mini) energy and gas meter monitoring system that counts pulses from sensors and reports via MQTT. It uses a MicroWakeupper battery shield for power-efficient operation with deep sleep between measurements.

## Build and Upload

This is an Arduino project. Use Arduino IDE or Arduino CLI.

**Required libraries:**
- MicroWakeupper (https://github.com/tstoegi/MicroWakeupper)
- PubSubClient (MQTT)
- WiFiManager (by tzapu)
- ArduinoJson
- ESP8266WiFi, ESP8266mDNS, ArduinoOTA, LittleFS (ESP8266 core)

**First-time setup:**
1. Flash the firmware
2. Connect to the "gasmeter-Setup" WiFi AP
3. Configure WiFi and MQTT settings through the web portal
4. Device reboots and connects to your network

**OTA Update:**
```bash
# Method 1: Enable OTA via MQTT: publish "true" to <topic>/settings/waitForOTA
# Method 2: Reset device 3 times quickly (within 10 seconds)
./ota.sh -ip <ESP_IP> -f build/esp8266.esp8266.d1_mini/MyMeter.ino.bin -pw <OTA_PASSWORD>
```

**Serial Debug:**

Use in separate terminal (not in Claude Code):
```bash
picocom -b 115200 /dev/cu.usbserial-132410
# Exit: Ctrl+A, Ctrl+X
```

## Architecture

**State Machine:** The firmware runs a sequential state machine:
1. `state_startup` - Load config and counter from LittleFS
2. `state_setupWifi` - Connect to WiFi via WiFiManager (auto-connect or config portal)
3. `state_setupMqtt` - Connect to MQTT broker (optional TLS)
4. `state_receiveMqtt` - Process retained settings messages
5. `state_checkSensorData` - If triggered by MicroWakeupper, increment counter and store to LittleFS
6. `state_sendMqtt` - Publish total, battery voltage, and RSSI
7. `state_turningOff` - Re-enable MicroWakeupper and enter deep sleep

**Storage (LittleFS):**
- `/config.json` - MQTT settings (broker, port, user, password, topic)
- `/counter.txt` - Current counter value
- `/reset_count.txt` - Multi-reset detection counter

**Configuration layers:**
- `config.h` - Device name, static IP options, debug flags
- `credentials.h` - OTA password, TLS fingerprint
- Web portal - WiFi and MQTT credentials (stored in LittleFS)

**MQTT Topics:**
- Publish: `<main>/<name>/total`, `wifi_rssi`, `batteryVoltage`
- Subscribe: `<main>/<name>/settings/total` (set counter), `waitForOTA`, `voltageCalibration`

## Network / Infrastructure

- **Home MQTT broker:** `192.168.4.79` (Tobias' Heimnetz)
- **Heiko's MQTT broker:** `192.168.4.27` (Heiko's Heimnetz), user: `heiko`, password: `heikog`
- **Heiko's WiFi:** `WLAN-074425`
- **Heiko's device (new Wemos):** Static IP `192.168.4.87`, topic `haus/gasmeter`
- **Test device (old Wemos, MAC 4c:75:25:19:1c:2d):** Static IP `192.168.4.88`, topic `haus/gasmeter_test`

## Configuration Flags

In `config.h`:
- `DEBUG` - Enable serial logging (slower operation)
- `MQTT_TLS` - Enable TLS for MQTT connection
- `STATIC_IP` - Use static IP for faster WiFi connection
- `STATIC_WIFI` - Use static BSSID/channel for faster connection

## Version and Build Number

**IMPORTANT:** Before each compile, increment `BUILD_NUMBER` in MyMeter.ino:
```cpp
#define VERSION_YEAR 2026
#define VERSION_MONTH 2
#define BUILD_NUMBER 3  // <-- INCREMENT THIS ON EACH COMPILE
```

The version string is automatically combined: `YEAR.MONTH.BUILD` (e.g., `2026.2.3`)

## Triggering Config Portal

Reset the device 6 times within 10 seconds to force the config portal to open (like Tasmota). This is useful for:
- Changing WiFi networks
- Updating MQTT settings
- Enabling OTA mode (auto-enabled after config portal save)

## Performance Optimization TODO

**Current Issues (as of v2026.2.17):**
- IRAM usage: 93% (61100/65536 bytes) - **CRITICAL!** Too close to limit
- RAM usage: 42% (34384/80192 bytes) - Acceptable
- Flash usage: 37% (395808/1048576 bytes) - Good

**Priority Tasks:**
1. **IRAM Optimization (CRITICAL):**
   - Move non-critical functions from IRAM to Flash
   - Replace String operations with char arrays to reduce IRAM
   - Consider loading WiFiManager dynamically only when needed
   - Profile which functions consume most IRAM
   - WiFiManager is known to be IRAM-heavy

2. **WiFi Connection Speed:**
   - Enable STATIC_IP for production builds (faster by ~2-3 seconds)
   - Enable STATIC_WIFI (BSSID/channel) for even faster connection
   - Reduce WiFi connection timeout values

3. **Code Optimization:**
   - Eliminate String concatenation (causes heap fragmentation)
   - Use snprintf() instead of String operations
   - Pre-allocate buffers where possible
   - Optimize JSON document sizes

4. **Timing Analysis:**
   - Measure boot-to-sleep cycle time
   - Profile MQTT connection time
   - Identify bottlenecks in state machine
   - Add timing measurements (only in DEBUG mode)

**Target Goals:**
- IRAM usage < 85% (safety margin for stability)
- Boot-to-sleep cycle < 5 seconds (for battery life)
- WiFi connection < 2 seconds
- MQTT connection < 1 second
