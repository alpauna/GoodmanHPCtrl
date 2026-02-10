# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded HVAC controller for Goodman heatpumps (cooling, heating, defrost modes) running on ESP32. Controls 4 relay outputs (FAN, CNT, W, RV) based on 4 input signals (LPS, DFT, Y, O), 4 OneWire temperature sensors (COMPRESSOR, SUCTION, AMBIENT, CONDENSER), and 1 MCP9600 I2C thermocouple (LIQUID). Provides a REST API, WebSocket, and MQTT interface for remote monitoring and control.

## Build Commands

```bash
# Build for primary target (Freenove ESP32-S3-WROOM)
pio run -e freenove_esp32_s3_wroom

# Build for alternative target (ESP32 DevKit)
pio run -e esp32dev

# Upload firmware (USB on /dev/ttyUSB0)
pio run -t upload -e freenove_esp32_s3_wroom

# Serial monitor (115200 baud)
pio run -t monitor -e freenove_esp32_s3_wroom

# Run tests
pio test -e freenove_esp32_s3_wroom
```

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
| `include/GoodmanHP.h` | GoodmanHP class with input/output pin maps |
| `include/OutPin.h` | OutPin class, OutputPinCallback typedef |
| `include/InputPin.h` | InputPin class, InputResistorType/InputPinType enums, InputPinCallback typedef |
| `include/Logger.h` | Logger class |
| `include/Config.h` | Config class for SD card and JSON configuration |
| `include/TempSensor.h` | TempSensor class for OneWire temperature sensors |
| `src/WebHandler.cpp` | Web server and REST API implementation |
| `include/WebHandler.h` | WebHandler class declaration |
| `src/MQTTHandler.cpp` | MQTT client, callbacks, and reconnect logic |
| `include/MQTTHandler.h` | MQTTHandler class declaration |
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
| `tSaveRuntime` | 5min | Persist heat runtime accumulation to SD card |

### Memory Management

Global `operator new`/`delete` are overridden in `src/PSRAMAllocator.cpp` to route all allocations through PSRAM (`ps_malloc`) when available, falling back to regular `malloc`. PSRAM is initialized early via `__attribute__((constructor(101)))`, which runs before C++ global constructors so PSRAM is available for any static object that allocates memory.

### I/O Classes

- **GoodmanHP** (`GoodmanHP.h/cpp`): Central controller managing input/output pin maps, temperature sensors, and heat pump state machine. Contains:
  - `std::map<String, InputPin*>` for input pins (LPS, DFT, Y, O)
  - `std::map<String, OutPin*>` for output pins (FAN, CNT, W, RV)
  - `TempSensorMap` for temperature sensors (COMPRESSOR, SUCTION, AMBIENT, CONDENSER via OneWire; LIQUID via MCP9600 I2C thermocouple)
  - Pin methods: `addInput()`, `addOutput()`, `getInput()`, `getOutput()`, `getInputMap()`, `getOutputMap()`
  - Temp methods: `addTempSensor()`, `getTempSensor()`, `getTempSensorMap()`, `clearTempSensors()`
  - State machine: OFF, COOL (Y+O active), HEAT (Y active only), DEFROST
  - All outputs are turned OFF on startup via `begin()`
  - RV (reversing valve) automatically controlled: ON in COOL mode, OFF in HEAT/OFF mode
  - W (auxiliary heat) automatically controlled: ON in DEFROST, ERROR (HEAT only), and LOW_TEMP (HEAT only) modes; never turned on in COOL mode
  - Auto-activates CNT relay when Y input becomes active, with 5-minute short cycle protection: if CNT was off for less than 5 minutes, enforces a 30-second delay before reactivation; if off for 5+ minutes (or never activated), CNT activates immediately
  - **Automatic defrost**: After 90 min accumulated CNT runtime in HEAT mode, initiates software defrost (turn off CNT, turn on RV, turn on CNT). Runs for at least 3 minutes, then exits when CONDENSER_TEMP > 41°F or 15-min safety timeout. Runtime resets on COOL mode or after defrost completes. Runtime persists to SD card every 5 min via `tSaveRuntime` task. If Y drops during defrost, all outputs turn off but `_softwareDefrost` stays set; defrost resumes when Y reactivates in HEAT mode.
  - **DFT emergency defrost**: DFT input triggers the same unified defrost cycle from HEAT mode (same 3-min minimum, 41°F exit, 15-min timeout). Uses the same `_softwareDefrost` path as automatic defrost.
  - **LPS fault protection**: When LPS input goes LOW (low refrigerant pressure), immediately shuts down CNT if running and blocks CNT activation. If in HEAT mode (Y active, O not active), turns on W for auxiliary heat. Auto-recovers when LPS goes HIGH (W turned off). Publishes fault events via `LPSFaultCallback`. `lpsFault` field included in `goodman/state` MQTT payload.
  - **Low ambient temperature protection**: When AMBIENT_TEMP drops below configurable threshold (default 20°F), enters `LOW_TEMP` state: shuts down CNT, turns off FAN and RV. Turns on W (auxiliary heat) only if not in COOL mode (O active). W is never turned on in COOL mode. Blocks CNT activation and state updates while active. Auto-recovers when temp rises above threshold. `lowTemp` field included in `goodman/state` MQTT payload.
  - Public methods: `getHeatRuntimeMs()`, `setHeatRuntimeMs()`, `resetHeatRuntime()`, `isSoftwareDefrostActive()`, `isLPSFaultActive()`, `setLPSFaultCallback()`, `isLowTempActive()`, `setLowTempThreshold()`, `getLowTempThreshold()`
- **OutPin** (`OutPin.h/cpp`): Output relay control with configurable activation delay, PWM support, on/off counters, and callback on state change. Delay is implemented via a TaskScheduler task.
- **InputPin** (`InputPin.h/cpp`): Digital/analog input with configurable pull-up/down, ISR-based interrupt detection, debouncing via delayed verification (circular buffer queue checked by `_tGetInputs`), and callback on change.
- **TempSensor** (`TempSensor.h/cpp`): Temperature sensor wrapper with encapsulated state and callbacks. Supports OneWire (via `update()`) and external sources like MCP9600 I2C thermocouple (via `updateValue()`):
  - Properties: `description`, `deviceAddress`, `value`, `previous`, `valid`
  - Callbacks: `setUpdateCallback()`, `setChangeCallback()`
  - Methods: `setMCP9600(Adafruit_MCP9600*)` assigns I2C thermocouple source; `update(DallasTemperature*, threshold)` reads from MCP9600 if set, otherwise from OneWire; `updateValue(float tempF, threshold)` accepts a raw Fahrenheit value directly. All fire change callback if delta exceeds threshold
  - Static helpers:
    - `addressToString(uint8_t*)` — Convert DeviceAddress to hex string
    - `stringToAddress(String&, uint8_t*)` — Parse hex string to DeviceAddress
    - `printAddress(uint8_t*)` — Print address to Serial in hex format
    - `discoverSensors(DallasTemperature*, TempSensorMap&, updateCb, changeCb)` — Enumerate OneWire bus and populate TempSensorMap
    - `getDefaultDescription(uint8_t index)` — Returns sensor name by index (COMPRESSOR_TEMP, SUCTION_TEMP, AMBIENT_TEMP, CONDENSER_TEMP)

### GPIO Pin Mapping (ESP32-S3)

Inputs: LPS=GPIO15, DFT=GPIO16, Y=GPIO17, O=GPIO18
Outputs: FAN=GPIO4, CNT=GPIO5 (3s delay), W=GPIO6, RV=GPIO7
OneWire bus: GPIO21
I2C: SDA=GPIO8, SCL=GPIO9 — MCP9600 thermocouple amplifier at 0x67 (LIQUID_TEMP)

### Networking

- **AsyncWebServer** on port 80 with REST endpoints (`/temps`, `/heap`, `/scan`, `/log`, `/log/level`, `/log/config`, `/update` for OTA)
- **WebSocket** at `/ws`
- **MQTT** (`MQTTHandler` wrapping AsyncMqttClient) to configurable broker, default `192.168.0.46:1883`
  - `goodman/log` — log messages (Logger output)
  - `goodman/temps` — all valid temp sensor values as JSON, published on any sensor change. Format: `{"COMPRESSOR_TEMP":72.5,"SUCTION_TEMP":65.2,"LIQUID_TEMP":185.3,...}`
  - `goodman/state` — state + inputs/outputs as JSON, published on state transitions. Format: `{"state":"HEAT","inputs":{...},"outputs":{...},"heatRuntimeMin":42,"defrost":false,"lpsFault":false,"lowTemp":false}`
  - `goodman/fault` — fault events as JSON, published when faults activate/clear. Format: `{"fault":"LPS","message":"Low refrigerant pressure","active":true}`

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
};
```

**SD Card Methods:**
- `initSDCard()` — Initialize SdFat filesystem
- `openConfigFile(filename, config, proj)` — Open or create config file
- `loadTempConfig(filename, config, proj)` — Load JSON config into TempSensorMap and ProjectInfo
- `saveConfiguration(filename, config, proj)` — Write config to SD card
- `updateRuntime(filename, heatRuntimeMs)` — Update only the runtime field in config JSON
- `clearConfig(config)` — Free memory and clear TempSensorMap

**Config Getters/Setters:**
- WiFi: `getWifiSSID()`, `getWifiPassword()`, `setWifiSSID()`, `setWifiPassword()`
- MQTT: `getMqttHost()`, `getMqttPort()`, `getMqttUser()`, `getMqttPassword()` (and setters)
- SD access: `getSd()` returns `SdFs*` for Logger and other components

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
Log.setLogFile(config.getSd(), "/log.txt", proj.maxLogSize, proj.maxOldLogCount);
```

JSON config stored on SD card at `/config.txt` (SdFat library, SPI interface). Contains WiFi credentials, MQTT settings, log rotation settings, timezone settings, and temperature sensor address-to-name mappings. Loaded during `setup()`, writable via API.

**JSON structure:**
```json
{
  "project": "...",
  "created": "...",
  "description": "...",
  "wifi": { "ssid": "...", "password": "..." },
  "mqtt": { "user": "...", "password": "...", "host": "...", "port": 1883 },
  "logging": { "maxLogSize": 52428800, "maxOldLogCount": 10 },
  "runtime": { "heatAccumulatedMs": 0 },
  "timezone": { "gmtOffset": -21600, "daylightOffset": 3600 },
  "lowTemp": { "threshold": 20.0 },
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

**SdFat/SD library swap**: The project uses SdFat (Adafruit fork) for normal SD card operations, but ESP32-targz requires Arduino's `SD` library (`fs::FS` interface). During log rotation, Logger temporarily calls `_sd->end()`, initializes Arduino `SD` for compression, then restores SdFat via `_sd->begin()`. This swap is isolated to `compressFile()` in `Logger.cpp`.

### NTP Time Sync

RTC time is synchronized from NTP servers (`192.168.0.1`, `time.nist.gov`) using the ESP32's built-in SNTP client. The `tNtpSync` task is enabled when WiFi connects, syncs immediately, then repeats every 2 hours. Timezone is configurable via `gmtOffsetSec` and `daylightOffsetSec` in `ProjectInfo`, persisted to SD card in the `timezone` JSON section. Default: US Central (UTC-6, DST +1hr). Values are passed to `WebHandler::setTimezone()` before `begin()` and used in `configTime()` calls.

### State Machine

```
enum AC_STATE { OFF, COOL, HEAT, DEFROST, ERROR, LOW_TEMP }
```

### Control Flow Example

Y input pin ISR fires → change queued in circular buffer → `_tGetInputs` calls `onCheckInputQueue()` → `onInput()` callback → uses `hpController.getOutput("CNT")` to activate/deactivate CNT output relay (with 3s delay on activation).

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
- `SD_FAT_TYPE=3`: Uses SdFs (supports FAT16/FAT32/exFAT)
- `DEST_FS_USES_SD`: Required by ESP32-targz to use Arduino SD filesystem
