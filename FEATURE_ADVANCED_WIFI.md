# Feature: Dynamic Static IP and BSSID/Channel Configuration

**Version:** 2026.2.30+
**Status:** Planned
**Priority:** High (Performance Optimization)

## Overview

This feature adds dynamic configuration of Static IP and Static BSSID/Channel settings through the WiFiManager web portal, replacing the hardcoded values in `config.h`. This improves WiFi connection speed by 2-4 seconds and eliminates the need for recompilation when network settings change.

## Problem Statement

### Current Limitations
- Static IP settings are hardcoded in `config.h` (lines 9-21)
- BSSID and channel are hardcoded and commented out
- Changing these settings requires recompilation and reflashing
- Users cannot optimize WiFi connection speed without modifying code

### Performance Impact
- Default DHCP connection: ~5-7 seconds
- With Static IP: ~3-4 seconds (2-3s improvement)
- With Static IP + BSSID/Channel: ~2-3 seconds (3-4s improvement)

## Solution

### Architecture
1. **Config Storage**: Extend `Config` struct with 8 new fields (88 bytes RAM)
2. **Persistence**: Store settings in `config.json` (survives reboots)
3. **Web UI**: New "Advanced WiFi" page in config portal
4. **Application**: Conditional application in `setupWifi()` based on flags

### User Experience

#### Portal Navigation
```
Main Menu
├── 📶 WiFi (existing)
├── 📡 MQTT (existing)
├── ⚙️ Settings (existing)
├── 🔄 Update (existing)
└── 🔧 Advanced WiFi (NEW)
    ├── Static IP Configuration
    │   ├── [x] Enable Static IP
    │   ├── IP Address: 192.168.1.100
    │   ├── Gateway: 192.168.1.1
    │   ├── Subnet: 255.255.255.0
    │   └── DNS: 192.168.1.1
    └── Static BSSID/Channel
        ├── [x] Enable Static BSSID/Channel
        ├── BSSID: AA:BB:CC:DD:EE:FF
        │   (Current: 81:2A:A2:1A:0B:E7)
        └── Channel: 6
            (Current: 6)
```

#### Workflow
1. User triggers config portal (6x reset or first boot)
2. Navigate to "🔧 Advanced WiFi"
3. Enable Static IP checkbox
4. Enter network details (auto-filled suggestions shown)
5. Enable Static BSSID/Channel checkbox
6. Copy current BSSID/Channel (displayed on page)
7. Save → Device reboots
8. Faster WiFi connection applied

## Technical Specifications

### New Config Fields

```cpp
struct Config {
  // ... existing fields ...

  // Static IP Configuration
  bool useStaticIP = false;
  char staticIP[16] = "";          // "192.168.1.100"
  char staticGateway[16] = "";     // "192.168.1.1"
  char staticSubnet[16] = "";      // "255.255.255.0"
  char staticDNS[16] = "";         // "192.168.1.1"

  // Static BSSID/Channel
  bool useStaticWiFi = false;
  char staticBSSID[18] = "";       // "AA:BB:CC:DD:EE:FF"
  int staticChannel = 0;           // 0=auto, 1-14=specific
};
```

### config.json Structure

```json
{
  "device_name": "MyMeter",
  "wifi_ssid": "MyNetwork",
  "wifi_password": "********",
  "mqtt_broker": "192.168.1.10",
  "mqtt_port": 1883,
  "mqtt_user": "mqtt",
  "mqtt_password": "********",
  "mqtt_topic": "haus/gasmeter",
  "ota_on_boot": false,
  "ota_password": "********",

  // NEW FIELDS
  "use_static_ip": true,
  "static_ip": "192.168.1.100",
  "static_gateway": "192.168.1.1",
  "static_subnet": "255.255.255.0",
  "static_dns": "192.168.1.1",
  "use_static_wifi": true,
  "static_bssid": "AA:BB:CC:DD:EE:FF",
  "static_channel": 6
}
```

### WiFi Connection Logic

```cpp
// setupWifi() - Apply Static IP
if (config.useStaticIP && strlen(config.staticIP) > 0) {
  IPAddress ip, gateway, subnet, dns;
  if (ip.fromString(config.staticIP) &&
      gateway.fromString(config.staticGateway) &&
      subnet.fromString(config.staticSubnet) &&
      dns.fromString(config.staticDNS)) {
    WiFi.config(ip, gateway, subnet, dns);
    Log("Static IP configured");
  } else {
    Log("Static IP parsing failed, using DHCP");
  }
}

// setupWifi() - Apply Static BSSID/Channel
if (config.useStaticWiFi && strlen(config.staticBSSID) > 0 && config.staticChannel > 0) {
  byte bssid[6];
  parseBSSID(config.staticBSSID, bssid);
  WiFi.begin(config.wifiSsid, config.wifiPassword, config.staticChannel, bssid);
  Log("Static BSSID/Channel configured");
} else {
  WiFi.begin(config.wifiSsid, config.wifiPassword);
  Log("Using auto BSSID/Channel");
}
```

### Input Validation

#### IP Address Validation
- Format: `XXX.XXX.XXX.XXX`
- Each octet: 0-255
- Empty string allowed (disables feature)

#### BSSID Validation
- Format: `AA:BB:CC:DD:EE:FF`
- 17 characters (6 hex pairs with colons)
- Case insensitive
- Empty string allowed (disables feature)

#### Channel Validation
- Range: 0-14
- 0 = auto-detect
- 1-14 = specific channel

## Use Cases

### Use Case 1: Home User - Faster Boot Time
**Goal:** Reduce boot time for battery-powered gas meter

**Steps:**
1. Flash firmware, configure WiFi/MQTT via portal
2. Notice 5-7 second boot time
3. Re-open portal (6x reset)
4. Navigate to Advanced WiFi
5. Enable Static IP, enter home router settings
6. Save and reboot
7. Verify 3-4 second boot time (40% faster)

**Benefit:** Extended battery life, faster data reporting

### Use Case 2: Multiple Devices - Network Stability
**Goal:** Assign static IPs to multiple MyMeter devices

**Steps:**
1. Configure each device with unique static IP via portal
2. Reserve IPs in router DHCP table (optional)
3. Verify devices always get same IP
4. Use predictable IPs in home automation

**Benefit:** No IP conflicts, stable network addressing

### Use Case 3: Weak WiFi Signal - Connection Reliability
**Goal:** Force connection to specific router in multi-AP setup

**Steps:**
1. Check current BSSID/Channel in portal (displays at bottom)
2. Enable Static BSSID/Channel
3. Enter BSSID of closest access point
4. Save and reboot
5. Verify device always connects to specified AP

**Benefit:** Avoid roaming to distant APs, stable connection

### Use Case 4: Network Change - No Reflash Required
**Goal:** Move device to different network

**Steps:**
1. Trigger config portal (6x reset)
2. Change WiFi credentials on WiFi page
3. Update Static IP on Advanced WiFi page (if used)
4. Save and reboot
5. Device connects to new network

**Benefit:** No need for recompilation or serial access

## Implementation Details

### Memory Impact

#### IRAM (Critical - currently 93%)
- Helper functions: +200 bytes (marked `ICACHE_FLASH_ATTR` → Flash)
- setupWifi() changes: +100 bytes
- **Expected final: 93-94%** (acceptable, but close to limit)

#### RAM (currently 42%)
- Config struct: +88 bytes
- Web UI temporary Strings: +500 bytes (temporary during portal)
- **Expected final: 43%** (acceptable)

#### Flash (currently 37%)
- Web UI code: ~3 KB
- Validation logic: ~1 KB
- WiFi logic: ~0.5 KB
- **Expected final: 38%** (no concern)

### Code Changes

#### Modified Functions
- `loadConfig()` - Add 8 new JSON fields
- `saveConfig()` - Serialize 8 new fields
- `setupWifi()` - Apply static settings conditionally
- `setWebServerCallback()` - Register 2 new handlers

#### New Functions
- `isValidIP()` - Validate IP address format (Flash)
- `isValidBSSID()` - Validate BSSID format (Flash)
- `parseBSSID()` - Convert string to byte array (Flash)
- `handleAdvancedWifiPage()` - Render Advanced WiFi form
- `handleAdvancedWifiSave()` - Process and validate form submission

### Error Handling

#### Invalid IP Address
- Display error: "Invalid IP. "
- Don't save config
- Return to form with current values

#### Invalid BSSID
- Display error: "Invalid BSSID (use AA:BB:CC:DD:EE:FF). "
- Don't save config
- Return to form

#### Invalid Channel
- Display error: "Invalid channel (0-14). "
- Don't save config
- Return to form

#### Parse Failure at Runtime
- Log: "Static IP parsing failed, using DHCP"
- Graceful fallback to DHCP
- Device still connects

#### Connection Failure
- If static settings prevent connection
- Trigger portal via 6x reset
- Disable Advanced WiFi settings
- Save and try again

## Testing Strategy

### Test Matrix

| Test Case | Static IP | Static BSSID | Expected Result |
|-----------|-----------|--------------|-----------------|
| 1. Fresh Install | Disabled | Disabled | DHCP, auto BSSID (~5-7s) |
| 2. Static IP Only | Enabled | Disabled | Static IP, auto BSSID (~3-4s) |
| 3. Static BSSID Only | Disabled | Enabled | DHCP, static BSSID (~4-5s) |
| 4. Both Enabled | Enabled | Enabled | Static IP + BSSID (~2-3s) |
| 5. Invalid IP | Invalid | - | Error message, no save |
| 6. Invalid BSSID | - | Invalid | Error message, no save |
| 7. Disable After Enable | Disabled | Disabled | Fallback to DHCP/auto |
| 8. Upgrade from v2026.2.29 | Default: Off | Default: Off | Existing config preserved |

### Validation Tests

#### Input Validation
- Valid IP: `192.168.1.100` → Accept
- Invalid IP: `256.1.1.1` → Reject
- Invalid IP: `192.168.1` → Reject
- Empty IP: `` → Accept (disables feature)
- Valid BSSID: `AA:BB:CC:DD:EE:FF` → Accept
- Valid BSSID: `aa:bb:cc:dd:ee:ff` → Accept
- Invalid BSSID: `AA-BB-CC-DD-EE-FF` → Reject
- Invalid BSSID: `AA:BB:CC:DD:EE` → Reject
- Valid Channel: `6` → Accept
- Valid Channel: `0` → Accept (auto)
- Invalid Channel: `15` → Reject
- Invalid Channel: `-1` → Reject

#### Performance Tests
- Measure WiFi connection time with DEBUG mode
- Compare DHCP vs Static IP vs Static IP+BSSID
- Verify 2-4 second improvement

#### Persistence Tests
- Configure settings → Save → Reboot
- Verify config.json contains new fields
- Verify settings applied on next boot
- Factory reset → Verify defaults restored

### Debug Logging

```cpp
#ifdef DEBUG
Log("Static IP configured");
Log(config.staticIP);
Log("Static BSSID/Channel configured");
Log(config.staticBSSID);
Log("Using auto BSSID/Channel");
Logf("WiFi connect time: %lu ms\n", wifiEnd - wifiStart);
#endif
```

## Rollback Strategy

### If Feature Causes Issues

1. **Via Portal:**
   - Trigger portal (6x reset)
   - Navigate to Advanced WiFi
   - Uncheck both checkboxes
   - Save and reboot

2. **Via Factory Reset:**
   - Trigger portal (6x reset)
   - Navigate to Settings
   - Check "Factory Reset"
   - All settings deleted, fresh start

3. **Via Re-flash:**
   - Flash previous firmware (v2026.2.29)
   - Old firmware ignores new JSON fields
   - Device continues working with DHCP

### Compatibility

- **Forward compatible:** New firmware reads old config.json (missing fields default to false/empty)
- **Backward compatible:** Old firmware ignores new fields in config.json
- **Data loss:** None - all existing settings preserved during upgrade

## Success Criteria

- ✅ Advanced WiFi page accessible and functional
- ✅ All fields save/load correctly from config.json
- ✅ Input validation prevents invalid configurations
- ✅ Static IP applies correctly (verified via router)
- ✅ Static BSSID/Channel works (faster connection)
- ✅ Graceful fallback to DHCP if disabled or parsing fails
- ✅ IRAM usage ≤ 94% (monitor closely)
- ✅ No WiFi connection regressions
- ✅ WiFi connection time improves by 2-4 seconds
- ✅ Existing configs work after upgrade (no breakage)

## Future Enhancements

### Phase 2 (Optional)
- Auto-fill current network settings (detect IP, Gateway, DNS)
- "Use Current" button to copy active BSSID/Channel
- Connection time statistics display in portal
- IP conflict detection

### Phase 3 (Optional)
- Multiple WiFi network profiles
- Automatic fallback between networks
- WiFi signal strength indicator
- Connection diagnostics page

## Documentation Updates Required

### README.md
- Add section on Advanced WiFi configuration
- Explain Static IP benefits
- Show example network settings
- Add troubleshooting for connection issues

### CLAUDE.md
- Update "Configuration Flags" section
- Mark `STATIC_IP` and `STATIC_WIFI` as deprecated
- Add "Advanced WiFi Portal Settings" section
- Update performance optimization notes

## Related Files

- `MyMeter.ino` - Main implementation (all changes)
- `config.h` - Deprecation notice for old defines
- `config.json` - 8 new JSON fields
- `CLAUDE.md` - Documentation updates
- `README.md` - User documentation updates

## Version History

- **v2026.2.30** - Initial implementation (planned)
- **v2026.2.29** - Current version (no dynamic WiFi config)

## References

- WiFiManager documentation: https://github.com/tzapu/WiFiManager
- ESP8266 WiFi API: https://arduino-esp8266.readthedocs.io/
- Arduino IPAddress class: https://www.arduino.cc/reference/en/language/functions/communication/ip/
- LittleFS documentation: https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html
