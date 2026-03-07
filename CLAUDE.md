# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded HVAC controller for Goodman heatpumps (cooling, heating, defrost modes) running on ESP32. Controls 4 relay outputs (FAN, CNT, W, RV) based on 4 input signals (LPS, DFT, Y, O), up to 6 OneWire temperature sensors (COMPRESSOR, SUCTION, AMBIENT, CONDENSER, LIQUID, VAPOR), and 1 MCP9600 I2C thermocouple (LIQUID fallback). Provides a REST API, WebSocket, MQTT, and CAN bus interface for remote monitoring and control.

## Build Commands

```bash
# Build for primary target (Freenove ESP32-S3-WROOM)
pio run -e freenove_esp32_s3_wroom

# Build for alternative target (ESP32 DevKit)
pio run -e esp32dev

# Upload firmware (USB on /dev/ttyUSB0) — no auto-revert safety net
pio run -t upload -e freenove_esp32_s3_wroom

# Serial monitor (115200 baud)
pio run -t monitor -e freenove_esp32_s3_wroom

# Run tests
pio test -e freenove_esp32_s3_wroom
```

**OTA vs USB firmware updates**: OTA updates (`POST /update` + `POST /apply`) are preferred for production deployments. Before flashing new firmware, OTA automatically backs up the running firmware to `/firmware.bak` on SD card along with its build date (`/firmware.bak.meta`). If the new firmware causes a crash boot loop (3+ consecutive PANIC/WDT resets), the boot watchdog automatically reverts to the backup and reboots — but only if the backup's build date differs from the running firmware (same build = same bug, no point reverting). USB flashing (`pio run -t upload`) bypasses this entirely: no backup is created, so auto-revert is not possible.

## Architecture

### Source Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Application entry point, setup/loop, tasks, WiFi |
| `src/GoodmanHP.cpp` | Heat pump controller with pin management and state machine |
| `src/OutPin.cpp` | Output relay control implementation |
| `src/InputPin.cpp` | Input pin handling implementation |
| `src/Logger.cpp` | Multi-output logging with tar.gz rotation |
| `src/Config.cpp` | SD card and configuration management implementation |
| `src/TempSensor.cpp` | Temperature sensor class implementation |
| `src/CANBus.cpp` | CAN bus (TWAI) driver, message send/receive, heartbeat |
| `include/GoodmanHP.h` | GoodmanHP class with input/output pin maps |
| `include/OutPin.h` | OutPin class, OutputPinCallback typedef |
| `include/InputPin.h` | InputPin class, InputResistorType/InputPinType enums, InputPinCallback typedef |
| `include/Logger.h` | Logger class |
| `include/Config.h` | Config class for SD card and JSON configuration |
| `include/TempSensor.h` | TempSensor class for OneWire temperature sensors |
| `include/CANBus.h` | CANBus class for ESP32 TWAI CAN bus communication |
| `src/WebHandler.cpp` | Web server and REST API implementation |
| `include/WebHandler.h` | WebHandler class declaration |
| `src/MQTTHandler.cpp` | MQTT client, callbacks, and reconnect logic |
| `include/MQTTHandler.h` | MQTTHandler class declaration |
| `src/DisplayManager.cpp` | OLED display pages (status, temps, I/O, system, protections) |
| `include/DisplayManager.h` | DisplayManager class declaration |
| `src/PSRAMAllocator.cpp` | Global operator new/delete PSRAM overrides |

### Execution Model

Task-based cooperative scheduling using TaskScheduler with two scheduler instances (`ts` main, `hts` high-priority). The Arduino `loop()` calls `ts.execute()` each iteration. Key scheduled tasks:

| Task | Interval | Purpose |
|------|----------|---------|
| `tCheckTemps` | 10s | Read OneWire temperature sensors |
| `tRuntime` | 1min | Update runtime counter |
| `_tGetInputs` | 500ms | Process queued input pin changes |
| `_tReconnect` (MQTTHandler) | 10s | MQTT reconnection (disables itself on success) |
| `tWaitOnWiFi` | 1s x60 | WiFi connection wait |
| `tNtpSync` | 2h | NTP time sync (enabled on WiFi connect) |
| `tSaveRuntime` | 5min | Persist heat runtime, defrost state, and rvFail to SD card |
| `tAPReconnect` | `apFallbackSeconds` | WiFi reconnect attempts while in AP mode (disabled by default) |
| `tFetchWeather` | configurable (default 10min) | Fetch outdoor temp from OpenWeatherMap HTTP API (disabled when source != "http") |
| `tFailoverTestEnd` | 30min x1 | One-shot: ends ambient failover test after 30 minutes |
| `tCanPublish` | 2s | Publish HP state (0x200) and temps (0x201) on CAN bus (enabled when `can.enabled = true`) |
| CAN `_tPoll` | 10ms | Poll TWAI RX queue for incoming CAN messages (internal to CANBus class) |
| CAN `_tHeartbeat` | 5s | Send CAN heartbeat (0x3FF, node 0x03) (internal to CANBus class) |

### Memory Management

Global `operator new`/`delete` are overridden in `src/PSRAMAllocator.cpp` to route all allocations through PSRAM (`ps_malloc`) when available, falling back to regular `malloc`. PSRAM is initialized early via `__attribute__((constructor(101)))`, which runs before C++ global constructors so PSRAM is available for any static object that allocates memory.

### I/O Classes

- **GoodmanHP** (`GoodmanHP.h/cpp`): Central controller managing input/output pin maps, temperature sensors, and heat pump state machine. Contains:
  - `std::map<String, InputPin*>` for input pins (LPS, DFT, Y, O)
  - `std::map<String, OutPin*>` for output pins (FAN, CNT, W, RV)
  - `TempSensorMap` for temperature sensors (COMPRESSOR, SUCTION, AMBIENT, CONDENSER, LIQUID, VAPOR via OneWire; LIQUID also supported via MCP9600 I2C thermocouple fallback)
  - Pin methods: `addInput()`, `addOutput()`, `getInput()`, `getOutput()`, `getInputMap()`, `getOutputMap()`
  - Temp methods: `addTempSensor()`, `getTempSensor()`, `getTempSensorMap()`, `clearTempSensors()`
  - State machine: OFF, COOL (Y+O active), HEAT (Y active only), DEFROST
  - All outputs are turned OFF on startup via `begin()`
  - RV (reversing valve) automatically controlled: ON in COOL mode, OFF in HEAT/OFF mode
  - W (auxiliary heat) automatically controlled: ON in DEFROST, ERROR (HEAT only), LOW_TEMP (HEAT only), and RV_FAIL (HEAT only) modes; never turned on in COOL mode
  - Auto-activates CNT relay when Y input becomes active, with 5-minute short cycle protection: if CNT was off for less than 5 minutes, enforces a 30-second delay before reactivation; if off for 5+ minutes (or never activated), CNT activates immediately
  - **Automatic defrost (3-phase sequencing with adaptive bands)**: Defrost timing adapts to ambient temperature using 3 configurable bands. When accumulated CNT runtime in HEAT mode exceeds the active band's runtime threshold, initiates software defrost via a 3-phase output sequence:
    - **Adaptive defrost bands**: Two breakpoint temperatures (`_coldMaxTempF` default 23°F, `_warmMinTempF` default 31°F) divide ambient into 3 bands. `selectDefrostBand()` reads AMBIENT_TEMP (3-tier fallback: sensor/weather/ESP32) and returns the active band. Defaults to Warm (most conservative) when no ambient data available.

      | Band | Ambient Range | Runtime Threshold | Min Defrost | Exit Temp |
      |------|--------------|-------------------|-------------|-----------|
      | Cold | ≤ 23°F | 30 min | 2 min | 45°F |
      | Mid  | 23–31°F | 60 min | 3 min | 55°F |
      | Warm | ≥ 31°F | 90 min | 3 min | 60°F |

    - **Band snapshot**: When defrost starts, the active band's parameters are snapshotted into `_defrostSnapshotBand`. All exit criteria (min runtime, condenser exit temp) use the snapshot, preventing mid-cycle parameter jumps if ambient drifts.
    - **Phase 1** (`_defrostTransition`): All outputs off (CNT, FAN, W, RV) for pressure equalization. Duration: `_rvShortCycleMs` (default 30s, configurable).
    - **Phase 2** (`_defrostCntPending`): RV and W turned on, CNT remains off for short cycle delay. Duration: `_cntShortCycleMs` (default 30s, configurable).
    - **Phase 3**: CNT turned on, defrost fully active. Runs for at least the snapshotted band's `minRuntimeMs`, then exits when CONDENSER_TEMP >= snapshotted band's `exitTempF` or 15-min safety timeout.
    - Runtime resets on COOL mode or after defrost completes. Runtime persists to SD card every 5 min via `tSaveRuntime` task.
    - **Defrost state persistence**: `_softwareDefrost` is persisted to `heatpump.defrost.active` in config JSON via `tSaveRuntime` (every 5 min, on change detection). On boot, `restoreSoftwareDefrost()` sets `_softwareDefrost = true` without initiating transitions. When Y activates in HEAT mode, `updateState()` sees the flag and transitions to DEFROST, restarting from Phase 1.
    - **Y drop during defrost entry**: All outputs off (including W), transition flags cleared, but `_softwareDefrost` stays set. Defrost restarts from Phase 1 when Y reactivates in HEAT mode.
    - **COOL cancellation**: If thermostat switches to COOL (O active) during a pending defrost, defrost is cancelled entirely, heat runtime is cleared, and normal COOL mode proceeds.
    - CNT activation is blocked by `_softwareDefrost` in `checkYAndActivateCNT()` — during defrost, CNT is managed exclusively by `checkDefrostNeeded()`.
  - **Defrost exit transition (reverse 3-phase)**: When defrost completes (`stopSoftwareDefrost()`), the system performs a reverse 3-phase sequence to safely switch the reversing valve back to heat position:
    - **Exit Phase 1** (`_defrostTransition` + `_defrostExiting`): CNT, FAN, and W turned OFF immediately; RV stays ON. Duration: `_rvShortCycleMs` (default 30s). Pressure equalization after compressor stops.
    - **Exit Phase 2** (`_defrostCntPending` + `_defrostExiting`): RV turned OFF (switches back to heat position), CNT remains off. Duration: `_cntShortCycleMs` (default 30s). Allows RV to physically seat.
    - **Exit Complete**: CNT and FAN turned ON — normal HEAT mode resumes. `_defrostExiting` cleared.
    - `_softwareDefrost` is cleared at exit start, so `updateState()` transitions DEFROST → HEAT. Guards on `_defrostExiting` prevent RV/W/FAN from being modified by `updateState()` during exit.
    - **Y drop during exit**: Cancels exit transition, all outputs off. Normal state machine resumes on Y reactivation.
    - CNT activation is also blocked by `_defrostExiting` in `checkYAndActivateCNT()`.
  - **DFT emergency defrost**: DFT input triggers the same unified 3-phase defrost cycle from HEAT mode. Uses the same `_softwareDefrost` path as automatic defrost.
  - **LPS fault protection**: When LPS input goes LOW (low refrigerant pressure), immediately shuts down CNT if running and blocks CNT activation. W (auxiliary heat) continuously follows Y during active fault: ON when Y active and O not active, OFF when Y drops or O active. W is never turned on in COOL mode. Auto-recovers when LPS goes HIGH (W turned off). Publishes fault events via `LPSFaultCallback`. `lpsFault` field included in `goodman/state` MQTT payload.
  - **Low ambient temperature protection**: When AMBIENT_TEMP drops below configurable threshold (default 20°F) and stays below for 10 continuous minutes (`LOW_TEMP_VALIDATION_MS`), enters `LOW_TEMP` state: shuts down CNT, turns off FAN and RV. Turns on W (auxiliary heat) only if not in COOL mode (O active). W is never turned on in COOL mode. Blocks CNT activation and state updates while active. Auto-recovers when temp stays above threshold for 10 continuous minutes. If temp reverses during validation window, pending transition is cancelled (prevents fluttering). `_lowTempPendingEntry`/`_lowTempPendingExit`/`_lowTempPendingTick` track validation state. `lowTemp`, `lowTempPendingEntry`, `lowTempPendingExit`, `lowTempPendingRemainSec` in `/state` JSON and MQTT payload.
  - **State validation delay**: After any state transition, holds the new state for a configurable period (default 30s) before allowing another normal-priority change. OFF and ERROR bypass the timer (high priority). HEAT, COOL, DEFROST, LOW_TEMP are gated. LOW_TEMP output protection (CNT/FAN/RV off) is immediate but state label deferred until validation completes. Dashboard "State Hold" pill with countdown. Configurable via `heatpump.stateValidation.delayMs` (0–300s, live)
  - **Output state validation**: Every 10s, `validateOutputStates()` verifies outputs against a table-driven expected-state map. Global invariants: W off when O active, CNT off during faults, CNT off when no ambient data available. Per-state table with auto-correction. Logs errors on mismatch, "State check OK" every 30s when clean. Skips during transitions/lockout/override
  - **Ambient temperature fallback (3-tier)**: When AMBIENT_TEMP sensor is invalid, `_tskCheckTemps` (10s) uses cached weather data if fresh, otherwise falls back to ESP32 internal die temp (`temperatureRead()`). `enum class AmbientSource { SENSOR, WEATHER, INTERNAL }` tracks current source. Weather data cached in `_weatherTempF` with staleness timeout (`_weatherStaleMs`, default 30 min). Sources: MQTT subscription (plain float payload) or OpenWeatherMap HTTP API (`tFetchWeather`, configurable interval, default 10 min). `setAmbientFailoverTest(bool)` forces sensor invalid for testing (30 min auto-cancel via `tFailoverTestEnd`). State validator enforces CNT OFF when no ambient data available (sensor invalid + no weather). `ambientSource`, `weatherTempF`, `weatherTempAgeSec`, `failoverTest` in `/state` JSON.
  - **Subcooling calculation**: `getSubcoolingF()` returns `CONDENSER_TEMP - LIQUID_TEMP` (how much refrigerant has subcooled past saturation). `isSubcoolingValid()` returns true when both sensors are valid and compressor is running (HEAT, COOL, or DEFROST). Relevant for TXV systems. `subcoolingF` included in `/state` JSON when valid.
  - **CAN bus mode**: When `can.enabled` is true in config, `isYActive()` and `isOActive()` return CAN-derived values from `setCANInputState()` instead of physical pin states. DFT and LPS always remain physical (local safety devices). 10-second CAN timeout: if no 0x100 message received for 10s, `isYActive()` returns false for safe shutdown. CAN mode is config-only (requires save + reboot). Methods: `setCANMode()`, `isCANMode()`, `setCANInputState()`, `getCANLastRxTick()`
  - Public methods: `getHeatRuntimeMs()`, `setHeatRuntimeMs()`, `resetHeatRuntime()`, `isSoftwareDefrostActive()`, `restoreSoftwareDefrost()`, `isDefrostTransitionActive()`, `isDefrostCntPendingActive()`, `isDefrostExitingActive()`, `getDefrostTransitionRemainingMs()`, `getDefrostCntPendingRemainingMs()`, `isLPSFaultActive()`, `setLPSFaultCallback()`, `isLowTempActive()`, `isLowTempPendingEntry()`, `isLowTempPendingExit()`, `getLowTempPendingRemainingMs()`, `setLowTempThreshold()`, `getLowTempThreshold()`, `setColdMaxTempF()`, `getColdMaxTempF()`, `setWarmMinTempF()`, `getWarmMinTempF()`, `setDefrostBand()`, `getDefrostBand()`, `getActiveDefrostBand()`, `getActiveDefrostBandString()`, `getActiveRuntimeThresholdMs()`, `getActiveMinRuntimeMs()`, `getActiveExitTempF()`, `isStateValidating()`, `getStateValidationRemainingMs()`, `setStateValidationMs()`, `getStateValidationMs()`, `getSubcoolingF()`, `isSubcoolingValid()`, `setCANMode()`, `isCANMode()`, `setCANInputState()`, `getCANLastRxTick()`
- **OutPin** (`OutPin.h/cpp`): Output relay control with configurable activation delay, PWM support, on/off counters, and callback on state change. Delay is implemented via a TaskScheduler task.
- **InputPin** (`InputPin.h/cpp`): Digital/analog input with configurable pull-up/down, ISR-based interrupt detection, and confirmed-state debouncing. `isActive()` returns the debounced/validated state (not live GPIO). On pin change: ISR queues event → `_tGetInputs` (500ms) reads live GPIO and starts a configurable delay task (default 10s) → after delay, GPIO is re-read to validate the pin is still in the expected state. Mismatches are discarded as false triggers and logged as warnings. Both activation and deactivation go through the full delay. Configurable via `heatpump.inputDelay.ms` (0–60s, live). Methods: `readLiveState()` (bypass debounce), `setDelay(ms)`, `getDelay()`, `setPendingState()`.
- **TempSensor** (`TempSensor.h/cpp`): Temperature sensor wrapper with encapsulated state and callbacks. Supports OneWire (via `update()`) and external sources like MCP9600 I2C thermocouple (via `updateValue()`):
  - Properties: `description`, `deviceAddress`, `value`, `previous`, `valid`
  - Callbacks: `setUpdateCallback()`, `setChangeCallback()`
  - Methods: `setMCP9600(Adafruit_MCP9600*)` assigns I2C thermocouple source; `update(DallasTemperature*, threshold)` reads from MCP9600 if set, otherwise from OneWire; `updateValue(float tempF, threshold)` accepts a raw Fahrenheit value directly. All fire change callback if delta exceeds threshold
  - Static helpers:
    - `addressToString(uint8_t*)` — Convert DeviceAddress to hex string
    - `stringToAddress(String&, uint8_t*)` — Parse hex string to DeviceAddress
    - `printAddress(uint8_t*)` — Print address to Serial in hex format
    - `discoverSensors(DallasTemperature*, TempSensorMap&, updateCb, changeCb)` — Enumerate OneWire bus and populate TempSensorMap. Logs via Logger (ONEWIRE tag) instead of Serial
    - `getDefaultDescription(uint8_t index)` — Returns sensor name by index (0=COMPRESSOR_TEMP, 1=SUCTION_TEMP, 2=AMBIENT_TEMP, 3=CONDENSER_TEMP, 4=LIQUID_TEMP, 5=VAPOR_TEMP)
  - **OneWire auto-merge**: After `loadTempConfig()`, `setup()` enumerates the OneWire bus and merges any devices whose address is not already in the config-loaded TempSensorMap. New sensors get the first available default name. Logged as `[INFO] [MAIN] OneWire: new sensor <name> (<addr>)`. If no OneWire devices are found on the bus, a warning is logged: `[WARN] [MAIN] OneWire: no devices found on GPIO <pin>`

### GPIO Pin Mapping (ESP32-S3)

Inputs: LPS=GPIO15, DFT=GPIO16, Y=GPIO17, O=GPIO18
Outputs: FAN=GPIO4, CNT=GPIO5 (3s delay), W=GPIO6, RV=GPIO7
OneWire bus: GPIO21
I2C: SDA=GPIO8, SCL=GPIO9 — MCP9600 thermocouple amplifier at 0x67 (LIQUID_TEMP)
CAN bus: TX=GPIO13, RX=GPIO14 — ESP32 TWAI at 250 kbps (enabled via `can.enabled` config)

### Networking

- **AsyncWebServer** on port 80 with REST endpoints (`/temps`, `/heap`, `/scan`, `/log`, `/log/level`, `/log/config`, `/update` for OTA, `/ap/test`, `/ap/stop`)
- **WebSocket** at `/ws`
- **WiFi AP Mode**: Dual-mode `WIFI_AP_STA` with automatic WiFi reconnection. See [WiFi AP Mode](#wifi-ap-mode) section below.
- **MQTT** (`MQTTHandler` wrapping AsyncMqttClient) to configurable broker, default `192.168.0.46:1883`
  - `goodman/log` — log messages (Logger output)
  - `goodman/temps` — all valid temp sensor values as JSON, published on any sensor change. Format: `{"COMPRESSOR_TEMP":72.5,"SUCTION_TEMP":65.2,"LIQUID_TEMP":185.3,...}`
  - `goodman/state` — state + inputs/outputs as JSON, published on state transitions. Format: `{"state":"HEAT","inputs":{...},"outputs":{...},"heatRuntimeMin":42,"defrostBand":"Mid","defrostBandThresholdMin":60,"defrost":false,"defrostTransition":false,"defrostCntPending":false,"defrostExiting":false,"lpsFault":false,"lowTemp":false}`
  - `goodman/fault` — fault events as JSON, published when faults activate/clear. Format: `{"fault":"LPS","message":"Low refrigerant pressure","active":true}`
  - Configurable weather topic subscription (e.g., `homeassistant/sensor/outdoor_temp/state`) — parses plain float payload as outdoor temp for ambient fallback
- **CAN bus** (`CANBus` class wrapping ESP32 TWAI driver) at 250 kbps on GPIO13 (TX) / GPIO14 (RX), node 0x03 (HP_CTRL)
  - `0x200` (TX, 2s) — HP state: state enum, output bits, fault bits, protection bits, heat runtime, input bits, defrost band + ambient source
  - `0x201` (TX, 2s) — HP temps: AMBIENT, CONDENSER, SUCTION, LIQUID as int16×10 (big-endian), -9999 = invalid
  - `0x3FF` (TX, 5s) — Heartbeat: node ID + uptime seconds
  - `0x100` (RX) — Thermostat state from AThermostat: mode, setpoints, flags. When CAN mode enabled, maps thermostat mode to virtual Y/O inputs (HEAT→Y=true/O=false, COOL→Y=true/O=true, OFF/FAN→Y=false/O=false). `forceNoHP` flag overrides all to Y=false/O=false. `defrost` flag triggers `forceDefrost()`
  - 10-second CAN timeout safety: if no 0x100 received, `isYActive()` returns false → system goes OFF
  - DFT and LPS always physical — never virtualized over CAN
  - Enabled via `can.enabled` in config JSON (default false, requires reboot)
  - See `docs/canbus-goodmanhp-implementation.md` for full byte layout details

### Configuration

The **Config** class (`Config.h/cpp`) manages SD card operations and JSON configuration.

**ProjectInfo struct** (defined in `Config.h`):
```cpp
struct ProjectInfo {
    String name;           // Project name
    String createdOnDate;  // Creation date
    String description;    // Project description
    String encrypt;        // Encryption key (unused)
    bool encrypted;        // Encryption flag (unused)
    uint32_t maxLogSize;   // Max log file size in bytes before rotation
    uint8_t maxOldLogCount; // Number of rotated log files to keep
    uint32_t heatRuntimeAccumulatedMs; // Accumulated HEAT mode CNT runtime (persisted)
    int32_t gmtOffsetSec;        // GMT offset in seconds (default -21600 = UTC-6)
    int32_t daylightOffsetSec;   // DST offset in seconds (default 3600 = 1hr)
    float lowTempThreshold;      // Ambient temp threshold in F below which compressor is blocked (default 20.0)
    float highSuctionTempThreshold; // Suction temp above which RV fail detected during defrost (default 140.0)
    bool rvFail;                 // Latched RV fail flag (persisted)
    uint32_t rvShortCycleMs;     // RV pressure equalization delay in defrost transition (default 30000)
    uint32_t cntShortCycleMs;    // CNT short cycle delay on Y activation (default 30000)
    // Adaptive defrost bands (Cold / Mid / Warm)
    float defrostColdMaxTempF;   // Band breakpoint: ≤ this = Cold (default 23°F)
    float defrostWarmMinTempF;   // Band breakpoint: ≥ this = Warm (default 31°F)
    uint32_t defrostColdRuntimeMs;    // Cold: runtime threshold ms (default 1800000 = 30 min)
    uint32_t defrostColdMinRuntimeMs; // Cold: min defrost ms (default 120000 = 2 min)
    float defrostColdExitTempF;       // Cold: condenser cutoff (default 45°F)
    uint32_t defrostMidRuntimeMs;     // Mid: runtime threshold ms (default 3600000 = 60 min)
    uint32_t defrostMidMinRuntimeMs;  // Mid: min defrost ms (default 180000 = 3 min)
    float defrostMidExitTempF;        // Mid: condenser cutoff (default 55°F)
    uint32_t defrostWarmRuntimeMs;    // Warm: runtime threshold ms (default 5400000 = 90 min)
    uint32_t defrostWarmMinRuntimeMs; // Warm: min defrost ms (default 180000 = 3 min)
    float defrostWarmExitTempF;       // Warm: condenser cutoff (default 60°F)
    bool softwareDefrost;        // Persisted software defrost state (survives reboot)
    uint32_t stateValidationMs;  // State validation delay in ms (default 30000 = 30s)
    uint32_t inputDelayMs;       // Input pin validation delay in ms (default 10000 = 10s)
    uint32_t apFallbackSeconds;  // WiFi disconnect time before AP fallback (default 600 = 10 min)
    String apPassword;           // AP mode password override (empty = auto-generate at runtime)
    uint32_t tempHistoryIntervalSec; // Temp history capture interval in seconds (30-300, default 120)
    String theme;                // UI theme: "light" or "dark" (default "light")
    String weatherSource;        // Weather source: "none", "mqtt", or "http" (default "none")
    String weatherMqttTopic;     // MQTT topic for weather temp (e.g., "homeassistant/sensor/outdoor_temp/state")
    String weatherApiKey;        // OpenWeatherMap API key (encrypted with $AES$/$ENC$)
    String weatherZipCode;       // ZIP code for HTTP weather (e.g., "57106")
    String weatherCountry;       // Country code (default "US")
    uint32_t weatherStaleMinutes; // Max age before cached weather expires (default 30)
    uint32_t weatherRefreshMinutes; // HTTP fetch interval in minutes (default 10, range 1-60)
    bool canEnabled;             // Enable CAN bus for thermostat communication (default false)
};
```

**SD Card Methods:**
- `initSDCard()` — Initialize SD card filesystem
- `openConfigFile(filename, config, proj)` — Open or create config file
- `loadTempConfig(filename, config, proj)` — Load JSON config into TempSensorMap and ProjectInfo
- `saveConfiguration(filename, config, proj)` — Write config to SD card
- `updateRuntime(filename, heatRuntimeMs, softwareDefrost)` — Update runtime and defrost active fields in config JSON
- `clearConfig(config)` — Free memory and clear TempSensorMap
- `generateRandomPassword(length)` — Static method generating random password using `esp_fill_random()` hardware RNG with ambiguity-free charset (no 0/O/o, 1/l/I)

**Config Getters/Setters:**
- WiFi: `getWifiSSID()`, `getWifiPassword()`, `setWifiSSID()`, `setWifiPassword()`
- MQTT: `getMqttHost()`, `getMqttPort()`, `getMqttUser()`, `getMqttPassword()` (and setters)
- SD access: Uses global `SD` object (Arduino SD library)

**Usage:**
```cpp
ProjectInfo proj = {"Project Name", __DATE__, "Description", "", false, 50*1024*1024, 10, 0, -21600, 3600};
Config config;
config.setTempSensorDiscoveryCallback([](TempSensorMap& tempMap) { getTempSensors(tempMap); });
if (config.initSDCard()) {
    TempSensorMap& tempSensors = hpController.getTempSensorMap();
    if (config.openConfigFile("/config.txt", tempSensors, proj)) {
        config.loadTempConfig("/config.txt", tempSensors, proj);
        _WIFI_SSID = config.getWifiSSID();
        _MQTT_HOST = config.getMqttHost();
    }
}
Log.setLogFile("/log.txt", proj.maxLogSize, proj.maxOldLogCount);
```

JSON config stored on SD card at `/config.txt` (Arduino SD library, SPI interface). Contains WiFi credentials, MQTT settings, log rotation settings, timezone settings, and temperature sensor address-to-name mappings. Loaded during `setup()`, writable via API.

**JSON structure:**
```json
{
  "project": "...",
  "created": "...",
  "description": "...",
  "wifi": { "ssid": "...", "password": "...", "apFallbackSeconds": 600, "apPassword": "..." },
  "mqtt": { "user": "...", "password": "...", "host": "...", "port": 1883 },
  "logging": { "maxLogSize": 52428800, "maxOldLogCount": 10 },
  "runtime": { "heatAccumulatedMs": 0 },
  "timezone": { "gmtOffset": -21600, "daylightOffset": 3600 },
  "heatpump": {
    "lowTemp": { "threshold": 20.0 },
    "highSuctionTemp": { "threshold": 140.0, "rvFail": false },
    "shortCycle": { "rv": 30000, "cnt": 30000 },
    "defrost": {
      "active": false,
      "coldMaxTemp": 23.0,
      "warmMinTemp": 31.0,
      "cold": { "runtimeThresholdMs": 1800000, "minRuntimeMs": 120000, "exitTempF": 45.0 },
      "mid":  { "runtimeThresholdMs": 3600000, "minRuntimeMs": 180000, "exitTempF": 55.0 },
      "warm": { "runtimeThresholdMs": 5400000, "minRuntimeMs": 180000, "exitTempF": 60.0 }
    },
    "stateValidation": { "delayMs": 30000 },
    "inputDelay": { "ms": 10000 }
  },
  "tempHistory": { "intervalSec": 120 },
  "weather": { "source": "none", "mqttTopic": "", "apiKey": "", "zipCode": "", "country": "US", "staleMinutes": 30, "refreshMinutes": 10 },
  "can": { "enabled": false },
  "ui": { "theme": "dark" },
  "admin": { "password": "" },
  "sensors": { "temp": { ... } }
}
```

### Logger

Multi-output logging (Serial, MQTT topic, SD card with file rotation, WebSocket). Runtime-configurable level (ERROR/WARN/INFO/DEBUG) and output toggles via HTTP API.

**In-memory ring buffer**: Stores the last 500 log entries (configurable via `setRingBufferSize()`) in PSRAM for fast access. Accessible via `GET /log` endpoint with optional `?limit=N` query param. Returns JSON: `{"count":N,"entries":["...","..."]}`.

**WebSocket log streaming**: All log entries are broadcast to connected `/ws` clients as JSON: `{"type":"log","message":"..."}`. Enabled by default when WebHandler starts. Configurable via `POST /log/config?websocket=true|false`.

**Log format**: `[YYYY/MM/DD HH:MM:SS] [LEVEL] [TAG] message`
- Uses RTC time from NTP sync when available
- Falls back to `----/--/-- --:--:--` before NTP sync completes

**Log rotation** follows a Linux-style scheme using ESP32-targz for compression:
- `/log.txt` — active log (uncompressed)
- `/log.1.tar.gz` through `/log.N.tar.gz` — rotated archives (compressed)
- Rotation triggers when log file exceeds `maxLogSize` (default 50MB, configurable via ProjectInfo)
- Number of old logs kept is `maxOldLogCount` (default 10, configurable via ProjectInfo)
- Falls back to plain rename (`.txt`) if compression fails

### NTP Time Sync

RTC time is synchronized from NTP servers (`192.168.0.1`, `time.nist.gov`) using the ESP32's built-in SNTP client. The `tNtpSync` task is enabled when WiFi connects, syncs immediately, then repeats every 2 hours. Timezone is configurable via `gmtOffsetSec` and `daylightOffsetSec` in `ProjectInfo`, persisted to SD card in the `timezone` JSON section. Default: US Central (UTC-6, DST +1hr). Values are passed to `WebHandler::setTimezone()` before `begin()` and used in `configTime()` calls.

### WiFi AP Mode

When WiFi connection fails for `apFallbackSeconds` (default 600 = 10 min), the system enters AP mode as a fallback. AP mode can also be triggered manually via `POST /ap/test` from the config page.

**Password generation**: Uses `Config::generateRandomPassword()` (8-char, hardware RNG, ambiguity-free charset). Password priority: configured `apPassword` (if >= 8 chars) > previously generated password (reused across AP sessions) > new random password. Password is never regenerated if one already exists for the session.

**Dual-mode operation**: Uses `WIFI_AP_STA` mode (not pure `WIFI_AP`), keeping the STA interface active for background WiFi reconnection. SSID: `GoodmanHP`, IP: `192.168.4.1`.

**Automatic WiFi reconnection**: The `tAPReconnect` task runs at `apFallbackSeconds` interval while in AP mode. Each tick checks if WiFi has reconnected — if so, exits AP mode automatically (disables soft AP, switches to `WIFI_STA`). If not connected, retries `WiFi.begin()`. The `SYSTEM_EVENT_STA_GOT_IP` event handler also exits AP mode immediately when WiFi reconnects.

**AP mode endpoints**:
- `POST /ap/test` — Starts AP alongside existing WiFi (no disconnect), returns JSON `{"ssid":"GoodmanHP","password":"...","ip":"192.168.4.1"}`. Requires auth.
- `POST /ap/stop` — Stops soft AP, returns to `WIFI_STA`, reconnects WiFi. Requires auth.

**LCD display**: When `_apModeActive` is true, the status page (page 0) shows AP credentials: "AP MODE" header, SSID, password, and IP. Page rotation holds the AP screen 3x longer than other pages so the password is readable. Other pages remain unchanged.

**Config page**: WiFi fieldset includes AP Password input (leave blank for auto-generate) and Test AP Mode / Stop AP Mode buttons. `apPassword` encrypted with same `$AES$`/`$ENC$` scheme as other passwords. Persisted to `wifi.apPassword` in config JSON.

**Globals**: `bool _apModeActive`, `String _apPassword` — accessible by DisplayManager via `extern`.

### OLED Display

128x64 SSD1306 OLED managed by `DisplayManager` class. 5 rotating pages with configurable interval (3–60 sec, default 10):

| Page | Content |
|------|---------|
| 0 - Status | State banner, WiFi IP, uptime, heat runtime. Shows AP credentials when `_apModeActive` (held 3x longer) |
| 1 - Temps | All 6 temperature sensors (COMP, SUCT, AMB, COND, LIQ, VAPR) with 9px spacing |
| 2 - I/O | Input states (LPS, DFT, Y, O) and output states (FAN, CNT, W, RV) |
| 3 - System | Free heap, CPU load (both cores), PSRAM, WiFi RSSI |
| 4 - Protections | Active protections: LPS fault, low temp, RV fail, defrost, SC protect, comp over temp, suction low, startup lockout |

Page indicator dots at bottom-right. Display can be enabled/disabled (powers off OLED when disabled).

### State Machine

```
enum AC_STATE { OFF, COOL, HEAT, DEFROST, ERROR, LOW_TEMP }
```

### Control Flow Example

**Physical input mode (default):** Y input pin ISR fires → change queued in `_isrEvent` map → `_tGetInputs` (500ms) calls `onCheckInputQueue()` → reads live GPIO, starts validation delay task (default 10s) → after delay, `Callback()` re-reads GPIO to confirm → if validated, updates `_confirmedActive` and fires `onInput()` callback → `GoodmanHP::update()` reads `isActive()` (confirmed state) and activates CNT output relay (with 3s delay on activation). If GPIO reverts during the delay, the change is discarded as a false trigger.

**CAN bus mode (`can.enabled = true`):** TWAI poll task (10ms) receives 0x100 from AThermostat → `onCanReceive()` maps thermostat mode to Y/O booleans → `setCANInputState(yActive, oActive)` stores values and updates `_canLastRxTick` → `GoodmanHP::update()` calls `isYActive()`/`isOActive()` which return CAN-derived values → normal state machine proceeds. If no 0x100 for 10s, `isYActive()` returns false → safe shutdown. DFT/LPS still use physical pins.

### GoodmanHP Initialization

```cpp
// Global instance with scheduler
GoodmanHP hpController(&ts);

// In setup():
hpController.addInput("LPS", new InputPin(...));
hpController.addInput("DFT", new InputPin(...));
hpController.addInput("Y", new InputPin(...));
hpController.addInput("O", new InputPin(...));
hpController.addOutput("FAN", new OutPin(...));
hpController.addOutput("CNT", new OutPin(...));
hpController.addOutput("W", new OutPin(...));
hpController.addOutput("RV", new OutPin(...));
hpController.begin();
```

## Key Build Flags

- `BOARD_ESP32_S3_WROOM` / `BOARD_ESP32_ROVER`: Board-specific conditional compilation
- `BOARD_HAS_PSRAM`: Enables PSRAM allocation path
- `_TASK_TIMEOUT`, `_TASK_STD_FUNCTION`, `_TASK_HEADER_AND_CPP`: TaskScheduler features
- `CIRCULAR_BUFFER_INT_SAFE`: Required; enforced by `#error` directive
- `DEST_FS_USES_SD`: Required by ESP32-targz to use Arduino SD filesystem
