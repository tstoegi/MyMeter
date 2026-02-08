# Feature: Memory Optimization for ESP8266

**Version:** 2026.2.30+
**Status:** Planned
**Priority:** Critical (IRAM at 93%)

## Overview

Comprehensive memory optimization to address critical IRAM shortage (93% usage) that risks compilation failures, runtime crashes, and OTA update issues. Three-phase approach targeting 10-18 KB IRAM reduction through code relocation, String elimination, and heap optimization.

## Problem Statement

### Current Memory Usage (v2026.2.29)
- **IRAM:** 61100 / 65536 bytes (**93%**) - **CRITICAL!**
- **RAM:** 34384 / 80192 bytes (42%) - Acceptable
- **Flash:** 395808 / 1048576 bytes (37%) - OK

### Risks
- Compilation may fail if IRAM exceeds 65536 bytes
- Runtime crashes due to IRAM overflow
- OTA updates may fail (insufficient memory for temporary buffers)
- No headroom for future features or bug fixes

### Target
- **IRAM:** <85% (55500 bytes) for safety margin
- **RAM:** Reduce heap fragmentation
- **Flash:** No constraints

## Root Causes

### 1. Missing ICACHE_FLASH_ATTR (4-6 KB waste)
- 8 web handler functions unnecessarily in IRAM
- HTTP handlers don't need IRAM - ESP8266WebServer supports Flash callbacks
- Easy fix: Add one keyword per function

### 2. Excessive String Concatenation (3-4 KB waste)
- 20+ instances of `String + String` operations
- Each `+=` triggers heap reallocation
- Large HTML strings (1KB+) built entirely on heap
- Causes fragmentation and IRAM bloat

### 3. No PROGMEM Usage
- Static HTML strings stored in RAM
- Could be in Flash with PROGMEM or snprintf

### 4. DEBUG Flag Enabled (1-2 KB waste)
- Serial logging macros compiled into production builds
- Adds IRAM code for `Serial.print()` calls
- Not needed in battery-powered production device

## Solution: Three-Phase Optimization

### Phase 1: Quick Wins (Mandatory)

**Effort:** 30-45 minutes
**Savings:** 5-8 KB IRAM
**Risk:** Very Low
**Result:** 93% → 80-85% IRAM

#### 1.1 Mark Web Handlers with ICACHE_FLASH_ATTR

**Affected Functions:** MyMeter.ino (lines 610, 627, 647, 665, 688, 709, 732, 777)

Mark these 8 functions:
```cpp
String ICACHE_FLASH_ATTR getHTMLHeader(const char* title) { ... }
void ICACHE_FLASH_ATTR handleMqttPage() { ... }
void ICACHE_FLASH_ATTR handleUpdatePage() { ... }
void ICACHE_FLASH_ATTR handleSettingsPage() { ... }
void ICACHE_FLASH_ATTR handleMqttSave() { ... }
void ICACHE_FLASH_ATTR handleUpdateSave() { ... }
void ICACHE_FLASH_ATTR handleSettingsSave() { ... }
void ICACHE_FLASH_ATTR handleMainMenu() { ... }
```

**Why Safe:**
- HTTP request handlers, not ISRs (Interrupt Service Routines)
- Called from ESP8266WebServer which tolerates Flash-resident code
- Zero functional change - pure memory relocation
- `ICACHE_FLASH_ATTR` moves code from IRAM (fast, scarce) to Flash (slow, abundant)

**Savings:** 4-6 KB IRAM per analysis

#### 1.2 Disable DEBUG Flag in Production

**File:** config.h (line 24)

```cpp
// #define DEBUG  // Disabled for production - saves 1-2 KB IRAM
```

**Impact:**
- Removes all `Serial.print()` calls via Log macros
- Faster boot time (no serial overhead)
- Battery life improvement
- Re-enable anytime for debugging

**Savings:** 1-2 KB IRAM

---

### Phase 2: String Optimization (Recommended)

**Effort:** 2-3 hours
**Savings:** 3-4 KB IRAM
**Risk:** Low-Medium
**Result:** 80% → 75-80% IRAM

#### 2.1 Refactor handleMainMenu() String Building

**File:** MyMeter.ino (lines 782-801)
**Savings:** 1.5-2 KB IRAM

**Problem:** 12+ String concatenations for 1.2KB HTML page

**Before:**
```cpp
String html = "<!DOCTYPE html>..." + String(devName) + "..."
    + String(devName) + "..." + String(versionString) + "...";
globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
```

**After:**
```cpp
void ICACHE_FLASH_ATTR handleMainMenu() {
  const char* devName = strlen(config.deviceName) > 0 ? config.deviceName : CO_MYMETER_NAME;

  char html[1024];  // Stack-allocated, auto-freed
  snprintf(html, sizeof(html),
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>%s</title>"
    "<style>"
    "body{font-family:sans-serif;background:#f0f0f0;margin:0;padding:20px}"
    ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
    "h1{color:#1fa3ec;text-align:center;margin-top:0}"
    "a{display:block;padding:16px;margin:10px 0;background:#1fa3ec;color:white;text-decoration:none;border-radius:6px;font-size:16px;text-align:center}"
    "a:hover{background:#1581bd}"
    ".exit{background:#888}.exit:hover{background:#666}"
    "</style></head><body><div class='container'>"
    "<h1>%s</h1>"
    "<a href='/wifi'>📶 WiFi</a>"
    "<a href='/mqtt'>📡 MQTT</a>"
    "<a href='/settings'>⚙️  Settings</a>"
    "<a href='/update'>🔄 Update</a>"
    "<a href='/exit' class='exit'>❌ Exit</a>"
    "<p style='text-align:center;color:#999;font-size:12px;margin-top:20px'>Firmware: %s</p>"
    "</div></body></html>",
    devName, devName, versionString);

  globalWifiManager->server->send(200, "text/html; charset=UTF-8", html);
}
```

**Benefits:**
- Single snprintf() call vs 12+ heap operations
- No heap allocations - stack buffer auto-freed
- Eliminates String object overhead
- No heap fragmentation

#### 2.2 Refactor getHTMLHeader() with snprintf

**File:** MyMeter.ino (lines 610-620)
**Savings:** 1-2 KB IRAM

**Problem:** 8+ String concatenations for header template

**Before:**
```cpp
String getHTMLHeader(const char* title) {
  String html = "<!DOCTYPE html>...";
  html += "<title>" + String(title) + "</title>";
  html += "<style>...";
  // 6 more += operations
  return html;
}
```

**After:**
```cpp
String ICACHE_FLASH_ATTR getHTMLHeader(const char* title) {
  char buffer[800];
  snprintf(buffer, sizeof(buffer),
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>%s</title>"
    "<style>body{font-family:sans-serif;margin:20px;background:#f0f0f0}"
    ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
    "h2{color:#1fa3ec;margin-top:0}input,select{width:100%%;padding:8px;margin:8px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px}"
    "button,a.button{background:#1fa3ec;color:white;padding:12px;border:none;border-radius:4px;cursor:pointer;width:100%%;margin:8px 0;text-decoration:none;display:block;box-sizing:border-box;text-align:center;font-size:16px;font-family:sans-serif;font-weight:normal}"
    "button:hover,a.button:hover{background:#1581bd}.back{background:#888}.back:hover{background:#666}"
    "label{display:block;margin:10px 0 5px 0;font-weight:bold}</style></head><body><div class='container'>",
    title);
  return String(buffer);  // Backward compatible with existing callers
}
```

**Note:** Return type stays `String` for compatibility - still saves IRAM

#### 2.3 Convert MQTT Topic Building to snprintf

**Files:** MyMeter.ino (lines 338, 1018, 1033)
**Savings:** 400-600 bytes IRAM

**Line 338 (mqttPublish):**
```cpp
// Before
void mqttPublish(const char *mainTopic, const char *subTopic, String msg) {
  String topicString = String(mainTopic) + "/" + String(subTopic);
  mqttClient.publish(topicString.c_str(), msg.c_str(), true);
}

// After
void mqttPublish(const char *mainTopic, const char *subTopic, String msg) {
  char topicBuffer[192];  // 128 + 64 max topic length
  snprintf(topicBuffer, sizeof(topicBuffer), "%s/%s", mainTopic, subTopic);
  mqttClient.publish(topicBuffer, msg.c_str(), true);
}
```

**Line 1018 (MQTT client ID):**
```cpp
// Before
String clientId = String(devName) + WiFi.localIP().toString();

// After
char clientId[64];
IPAddress ip = WiFi.localIP();
snprintf(clientId, sizeof(clientId), "%s%d.%d.%d.%d", devName, ip[0], ip[1], ip[2], ip[3]);
```

**Line 1033 (subscription topic):**
```cpp
// Before
String subTopicStr = String(subTopic) + "/#";

// After
char subTopicStr[165];  // 160 + "/#" + null
snprintf(subTopicStr, sizeof(subTopicStr), "%s/#", subTopic);
```

---

### Phase 3: Advanced Optimizations (Optional)

**Effort:** 1-2 hours
**Savings:** 1-2 KB IRAM
**Risk:** Medium
**Result:** 75% → 72-76% IRAM

**Only implement if Phase 1+2 don't achieve <82% IRAM**

#### 3.1 Use const char* in mqttCallback

**File:** MyMeter.ino (lines 1055-1112)
**Savings:** 500 bytes - 1 KB

Replace String comparisons with strcmp + helper function:

```cpp
// Add helper function
bool endsWith(const char* str, const char* suffix) {
  size_t strLen = strlen(str);
  size_t suffixLen = strlen(suffix);
  if (strLen < suffixLen) return false;
  return strcmp(str + strLen - suffixLen, suffix) == 0;
}

// Replace in mqttCallback
// Before
String subTopicKey = "total";
if (String(topic).endsWith(subTopicKey)) { ... }

// After
if (endsWith(topic, "total")) { ... }
```

#### 3.2 Replace setupOTA String Operations

**File:** MyMeter.ino (lines 934-937)
**Savings:** 300-500 bytes

```cpp
// Before
String localIPWithoutDots = WiFi.localIP().toString();
localIPWithoutDots.replace(".", "_");
String ota_client_id = String(devName) + localIPWithoutDots;

// After
IPAddress ip = WiFi.localIP();
char ota_client_id[64];
snprintf(ota_client_id, sizeof(ota_client_id), "%s%d_%d_%d_%d", devName, ip[0], ip[1], ip[2], ip[3]);
```

---

## Implementation Order

### Step 1: Phase 1 (MUST DO)
1. Add `ICACHE_FLASH_ATTR` to 8 functions (30 min)
2. Disable DEBUG in config.h (1 min)
3. Increment BUILD_NUMBER to 30
4. Compile and verify IRAM: should be 80-85%
5. Flash to test device
6. Functional test (config portal, all pages)
7. Commit: "Phase 1: Move web handlers to Flash, disable DEBUG"

### Step 2: Phase 2 (SHOULD DO)
8. Refactor handleMainMenu() (45 min)
9. Compile and test
10. Refactor getHTMLHeader() (1 hour)
11. Compile and test
12. Convert MQTT topic building (30 min)
13. Compile and test
14. Increment BUILD_NUMBER to 31
15. Verify IRAM: should be 75-80%
16. Full functional test + 24h stability test
17. Commit: "Phase 2: Eliminate String concatenation"

### Step 3: Phase 3 (OPTIONAL)
18. Only if IRAM still >82%
19. Implement 3.1 and 3.2
20. Verify IRAM: should be 72-76%
21. Commit: "Phase 3: Advanced optimizations"

---

## Critical Files

- **MyMeter.ino**
  - Lines 610-620 (getHTMLHeader)
  - Lines 627, 647, 665, 688, 709, 732 (handlers)
  - Lines 777-801 (handleMainMenu)
  - Line 338 (mqttPublish)
  - Lines 934-937 (setupOTA)
  - Lines 1018, 1033, 1055-1112 (MQTT)
- **config.h**
  - Line 24 (DEBUG flag)

---

## Memory Impact

### Before Optimizations (v2026.2.29)
```
IRAM: 61100 / 65536 bytes (93%) ← CRITICAL
RAM:  34384 / 80192 bytes (42%)
Flash: 395808 / 1048576 bytes (37%)
```

### After Phase 1 (v2026.2.30)
```
IRAM: ~53000-56000 / 65536 bytes (80-85%) ← SAFE
RAM:  ~34000 / 80192 bytes (42%)
Flash: ~398000 / 1048576 bytes (38%)
Savings: 5-8 KB IRAM
```

### After Phase 2 (v2026.2.31)
```
IRAM: ~49000-53000 / 65536 bytes (75-80%) ← EXCELLENT
RAM:  ~33000 / 80192 bytes (41%) ← Less fragmentation
Flash: ~401000 / 1048576 bytes (38%)
Savings: Additional 3-4 KB IRAM
```

### After Phase 3 (v2026.2.32)
```
IRAM: ~47000-50000 / 65536 bytes (72-76%) ← OPTIMAL
RAM:  ~32500 / 80192 bytes (40%)
Flash: ~402000 / 1048576 bytes (38%)
Savings: Additional 1-2 KB IRAM
Total Savings: 10-14 KB IRAM (93% → 72-76%)
```

---

## Testing Strategy

### Compilation Verification

After each phase:
```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini MyMeter.ino
```

Check output:
```
Sketch uses X bytes (Y%) of program storage space.
Global variables use Z bytes (W%) of dynamic memory.
```

**Targets:**
- Phase 1: Z < 56000 bytes (85%)
- Phase 2: Z < 53000 bytes (80%)
- Phase 3: Z < 50000 bytes (76%)

### Functional Testing

**Config Portal Test:**
1. Trigger portal (6x reset or first boot)
2. Navigate to each page:
   - Main menu
   - WiFi settings
   - MQTT settings
   - Device settings
   - Update settings
3. Save configuration on each page
4. Verify no errors, no crashes
5. Verify all HTML renders correctly

**MQTT Test:**
1. Verify device connects to broker
2. Check published messages (total, voltage, rssi)
3. Test retained settings messages:
   - Send `waitForOTA=true` → OTA enables
   - Send `total=123.45` → Counter updates
   - Send `voltageCalibration=0.1` → Voltage adjusts

**OTA Test:**
1. Enable OTA via MQTT
2. Run ota.sh with optimized firmware
3. Verify successful upload
4. Check device boots correctly
5. Verify functionality post-OTA

**Stability Test:**
1. Run device for 24 hours
2. Monitor for crashes/reboots
3. Verify counter increments
4. Check MQTT messages published regularly

### Rollback Test

Verify rollback works:
1. Flash optimized firmware
2. Re-flash v2026.2.29
3. Verify device works with old firmware
4. Config preserved in LittleFS

---

## Risk Mitigation

### Risk 1: ICACHE_FLASH_ATTR Causes Crashes
- **Likelihood:** Very Low
- **Impact:** High (device crashes on HTTP request)
- **Mitigation:**
  - Only applied to HTTP handlers, not ISRs
  - ESP8266WebServer tested with Flash callbacks
  - If crash occurs: Remove ICACHE_FLASH_ATTR from specific function
- **Detection:** Test all config portal pages

### Risk 2: Buffer Overflow in snprintf
- **Likelihood:** Low
- **Impact:** High (crash, data corruption)
- **Mitigation:**
  - All buffers oversized (1024 bytes for 800-byte HTML)
  - snprintf() auto-truncates (safe by design)
  - Test with longest device names and topics
- **Detection:** Test with 32-char device name, 64-char topic

### Risk 3: String Type Mismatch
- **Likelihood:** Low
- **Impact:** Medium (compilation error)
- **Mitigation:**
  - getHTMLHeader() still returns String (compatible)
  - Incremental changes with compile after each
- **Detection:** Compile after each function change

### Risk 4: Increased Stack Usage
- **Likelihood:** Low
- **Impact:** Medium (stack overflow)
- **Mitigation:**
  - Replaced heap with stack (better for ESP8266)
  - Stack buffers auto-freed (no leaks)
  - 1024-byte buffer fits in 4KB stack
- **Detection:** 24h stability test

---

## Rollback Strategy

### If Phase 1 Fails
1. Remove ICACHE_FLASH_ATTR from crashing function
2. Or re-enable DEBUG temporarily
3. Or flash v2026.2.29

### If Phase 2 Fails
1. Git revert specific commit
2. Or revert individual function (handleMainMenu, getHTMLHeader)
3. Keep Phase 1 changes (safe)

### If Phase 3 Fails
1. Git revert Phase 3 commit
2. Keep Phase 1+2 changes
3. Still have 75-80% IRAM (acceptable)

### Emergency Rollback
```bash
git log --oneline  # Find last known good
git checkout <commit-hash>
# Flash to device
```

Config preserved in LittleFS - no data loss.

---

## Success Criteria

- ✅ IRAM usage <85% after Phase 1
- ✅ IRAM usage <80% after Phase 2 (target)
- ✅ All config portal pages load correctly
- ✅ No HTML rendering issues
- ✅ MQTT publishing works
- ✅ Config save/load works
- ✅ OTA updates succeed
- ✅ No crashes during 24h test
- ✅ Counter increments correctly
- ✅ No compilation warnings
- ✅ Battery life maintained or improved

---

## Version Management

- **v2026.2.30:** Phase 1 complete (ICACHE_FLASH_ATTR + DEBUG off)
- **v2026.2.31:** Phase 2 complete (String optimization)
- **v2026.2.32:** Phase 3 complete (advanced optimizations) - optional

In MyMeter.ino:
```cpp
#define VERSION_YEAR 2026
#define VERSION_MONTH 2
#define BUILD_NUMBER 30  // Increment with each phase
```

---

## Expected Outcomes

### Phase 1 (Mandatory)
- **Effort:** 30-45 minutes
- **Savings:** 5-8 KB IRAM
- **Risk:** Very Low
- **Result:** Device safe from IRAM overflow
- **Deploy:** Immediately to production

### Phase 2 (Recommended)
- **Effort:** 2-3 hours
- **Savings:** 3-4 KB IRAM
- **Risk:** Low-Medium
- **Result:** Optimal IRAM, better heap behavior
- **Deploy:** After testing, strong recommendation

### Phase 3 (Optional)
- **Effort:** 1-2 hours
- **Savings:** 1-2 KB IRAM
- **Risk:** Medium
- **Result:** Maximum optimization
- **Deploy:** Only if needed for future features

---

## Post-Optimization Tasks

1. **Update CLAUDE.md:**
   - New IRAM/RAM percentages
   - Document DEBUG flag should stay disabled
   - Add note about ICACHE_FLASH_ATTR usage

2. **Document in README.md:**
   - Memory optimization applied
   - Performance improvements

3. **Code Comments:**
   - Add comment explaining ICACHE_FLASH_ATTR
   - Document buffer sizes in snprintf calls

4. **Future Considerations:**
   - Reserve 15% IRAM headroom for features
   - Always compile with DEBUG off for production
   - Monitor IRAM usage in build logs

---

## Technical Notes

### Why ICACHE_FLASH_ATTR Works
- ESP8266 has 32KB IRAM (instruction RAM) - fast but scarce
- ESP8266 has 1MB Flash - slow but abundant
- Most code can run from Flash with minimal performance impact
- HTTP handlers are not time-critical - Flash is acceptable
- ISRs and WiFi/MQTT core code must stay in IRAM

### Why String is Problematic
- Arduino String class uses dynamic allocation
- Each `+` or `+=` may trigger realloc()
- Heap fragmentation on ESP8266 (limited RAM)
- String code compiled into IRAM
- char[] arrays are stack-based (better)

### Why snprintf is Better
- Single allocation (stack-based)
- No heap fragmentation
- Predictable memory usage
- Standard C function (optimized)
- Auto-truncates (no overflow)

### Why Phase Order Matters
1. Phase 1: Low risk, high ROI - safe to deploy immediately
2. Phase 2: Higher risk - needs testing but major improvement
3. Phase 3: Highest risk - only if absolutely needed

---

## References

- ESP8266 Memory Architecture: https://arduino-esp8266.readthedocs.io/en/latest/faq/a02-my-esp-crashes.html
- ICACHE_FLASH_ATTR: https://arduino-esp8266.readthedocs.io/en/latest/faq/a02-my-esp-crashes.html#iram-shortage
- Arduino String vs char[]: https://hackingmajenkoblog.wordpress.com/2016/02/04/the-evils-of-arduino-strings/
- ESP8266 Stack and Heap: https://arduino-esp8266.readthedocs.io/en/latest/faq/a02-my-esp-crashes.html#stack

---

## Related Features

- FEATURE_ADVANCED_WIFI.md - Adds 88 bytes RAM, requires IRAM headroom
- Future OTA improvements - May need IRAM space
- Config portal enhancements - Requires stable IRAM baseline

---

## Lessons Learned

1. **Always use ICACHE_FLASH_ATTR** for non-ISR functions
2. **Avoid String concatenation** on embedded devices
3. **Monitor IRAM usage** in every build
4. **Disable DEBUG** in production builds
5. **Test incrementally** - one optimization at a time
6. **Keep rollback plan** ready
