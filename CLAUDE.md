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

## Configuration Flags

In `config.h`:
- `DEBUG` - Enable serial logging (slower operation)
- `MQTT_TLS` - Enable TLS for MQTT connection
- `STATIC_IP` - Use static IP for faster WiFi connection
- `STATIC_WIFI` - Use static BSSID/channel for faster connection

## Triggering Config Portal

Reset the device 6 times within 10 seconds to force the config portal to open (like Tasmota). This is useful for:
- Changing WiFi networks
- Updating MQTT settings
- Enabling OTA mode (auto-enabled after config portal save)
