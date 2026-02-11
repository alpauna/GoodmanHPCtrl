# GoodmanHPCtrl

ESP32-based controller for Goodman heatpumps with support for cooling, heating, and defrost modes.

## Features

- **Relay control** — 4 output pins (FAN, Contactor, W-Heat, Reversing Valve) driven by 4 input signals (Low Pressure Switch, Defrost, Y-Cool, O-Heat)
- **Temperature monitoring** — 4 OneWire (Dallas DS18B20) sensors (compressor, suction, ambient, condenser) + 1 MCP9600 I2C thermocouple (liquid line)
- **Remote access** — REST API, WebSocket, and MQTT for monitoring and control
- **OTA updates** — Firmware upload via web interface
- **SD card configuration** — WiFi, MQTT, and sensor settings stored as JSON on SD card
- **Multi-output logging** — Serial, MQTT, SD card with tar.gz compressed log rotation, and WebSocket streaming
- **In-memory log buffer** — 500-entry ring buffer in PSRAM, accessible via `/log` API endpoint
- **NTP time sync** — Automatic time synchronization from NTP servers, refreshes every 2 hours
- **I2C bus** — Initialized on GPIO8 (SDA) / GPIO9 (SCL) with automatic device scan at startup and `/i2c/scan` API endpoint
- **PSRAM support** — All heap allocations routed through PSRAM when available
- **FreeRTOS compatible** — Uses `vTaskDelay()` instead of `delay()` for proper RTOS task yielding

## Architecture

### GoodmanHP Controller

The `GoodmanHP` class is the central controller that manages all I/O pins and the heat pump state machine:

- **Pin Management** — Maintains `std::map` collections for input and output pins
  - `addInput(name, pin)` / `addOutput(name, pin)` — Register pins
  - `getInput(name)` / `getOutput(name)` — Access individual pins
  - `getInputMap()` / `getOutputMap()` — Access full pin collections

- **Startup** — All outputs (FAN, CNT, W, RV) are turned OFF on controller startup

- **State Machine** — Tracks heat pump operating mode:
  - `OFF` — No active request
  - `HEAT` — Y input active (heating mode, RV off, W off)
  - `COOL` — Y and O inputs active (cooling mode, RV on, W off)
  - `DEFROST` — DFT emergency defrost or software defrost cycle (W on)
  - `ERROR` — Fault condition active (LPS low pressure); all outputs shut down, state updates blocked until fault clears
  - `LOW_TEMP` — Ambient temperature below threshold (default 20°F); compressor off, auxiliary heat (W) on, auto-recovers when temp rises

- **Output Control by State:**
  - **RV** (reversing valve): ON in COOL, OFF in HEAT/OFF
  - **W** (auxiliary heat): ON in DEFROST, ERROR (HEAT mode only), and LOW_TEMP (HEAT mode only); OFF otherwise. In COOL mode (Y+O), the system will not operate below 20°F
  - **CNT** (contactor): auto-activates when Y input becomes active, with short cycle protection: if CNT was off for less than 5 minutes, a 30-second delay is enforced before reactivation; if off for 5+ minutes, CNT activates immediately

- **Compressor Over-Temperature Protection** — When COMPRESSOR_TEMP reaches 240°F or above:
  - Immediately shuts down CNT to stop compressor
  - Keeps FAN running to cool the compressor
  - Blocks CNT activation while overtemp is active
  - Rechecks compressor temp every 1 minute
  - Auto-recovers when temp drops below 190°F (50°F hysteresis gap)
  - Logs fault condition and resolution time
  - Highest priority fault — checked before LPS and ambient temp

- **Suction Low-Temperature Protection** (COOL mode only) — Monitors SUCTION_TEMP for freezing conditions:
  - Warns when suction temp drops below 34°F
  - Shuts down CNT below 32°F (critically low), keeps FAN running
  - Blocks CNT activation while fault is active
  - Rechecks every 1 minute
  - Auto-recovers when temp rises above 40°F (8°F hysteresis from critical)
  - Auto-clears if mode changes away from COOL
  - Logs fault condition and resolution time

- **LPS Fault Protection** — When the LPS (Low Pressure Switch) input goes LOW:
  - Immediately shuts down CNT if running
  - Sets state to `ERROR`, blocking all state updates
  - Blocks CNT activation while fault is active
  - Turns on W (auxiliary heat) if in HEAT mode (Y active, O not active); W is never turned on in COOL mode
  - Auto-recovers when LPS goes HIGH; W turned off, short-cycle protection (30s delay) is enforced on CNT reactivation
  - Publishes fault events via MQTT (`goodman/fault` topic)

- **Low Ambient Temperature Protection** — When AMBIENT_TEMP drops below the configurable threshold (default 20°F):
  - Enters `LOW_TEMP` state: shuts down CNT, turns off FAN and RV
  - Turns on W (auxiliary heat) as backup heating, but only if not in COOL mode (W is never turned on in COOL mode)
  - Blocks compressor activation while ambient temp is too low
  - Auto-recovers when temperature rises above threshold
  - Threshold is configurable via `lowTemp.threshold` in SD card config

- **Automatic Defrost** — After 90 minutes of accumulated CNT runtime in HEAT mode, initiates software defrost (turns off CNT, turns on RV, turns on CNT):
  - Heat runtime only accumulates when DFT input is active (closed at 32°F, indicating icing conditions)
  - DFT turning off (temps > 32°F) clears accumulated runtime — no ice risk
  - Runs for at least 3 minutes before checking exit conditions
  - Rechecks CONDENSER_TEMP every 1 minute during defrost with logging
  - Exits when CONDENSER_TEMP > 60°F or 15-minute safety timeout
  - Only COOL and DEFROST modes clear accumulated runtime; Y going off does not
  - Only HEAT mode adds time to accumulated runtime
  - Runtime persists to SD card every 5 minutes, restored on boot
  - If Y drops during defrost, all outputs turn off but defrost resumes when Y reactivates in HEAT mode

### Class Structure

| Class | Purpose |
|-------|---------|
| `GoodmanHP` | Central controller with pin maps, temp sensors, and state machine |
| `InputPin` | Digital/analog input with ISR, debouncing, callbacks |
| `OutPin` | Output relay with delay, PWM support, state tracking, hardware state validation |
| `TempSensor` | Temperature sensor with callbacks; supports OneWire (DS18B20) and I2C (MCP9600) |
| `Config` | SD card and JSON configuration management |
| `Logger` | Multi-output logging with tar.gz rotation, ring buffer, and WebSocket streaming |

## Hardware

**Supported boards:**
- Freenove ESP32-S3-WROOM (primary)
- ESP32 DevKit

**GPIO Pin Mapping (ESP32-S3):**

| Pin | GPIO | Direction | Description |
|-----|------|-----------|-------------|
| LPS | 15 | Input | Low Pressure Switch |
| DFT | 16 | Input | Defrost signal |
| Y | 17 | Input | Compressor request (heat or cool) |
| O | 18 | Input | Reversing valve signal (cool mode) |
| FAN | 4 | Output | Fan relay |
| CNT | 5 | Output | Contactor relay (3s delay) |
| W | 6 | Output | Heating relay |
| RV | 7 | Output | Reversing valve relay |
| SDA | 8 | I/O | I2C data |
| SCL | 9 | I/O | I2C clock |
| OneWire | 21 | I/O | Temperature sensor bus |

**I2C Devices:**

| Device | Address | Description |
|--------|---------|-------------|
| MCP9600 | 0x67 | Type-K thermocouple amplifier (LIQUID_TEMP) |

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE extension)
- USB cable connected to ESP32 board

### Build and Upload

```bash
# Build
pio run -e freenove_esp32_s3_wroom

# Upload firmware
pio run -t upload -e freenove_esp32_s3_wroom

# Serial monitor
pio run -t monitor -e freenove_esp32_s3_wroom
```

### SD Card Configuration

Place a `config.txt` file on the SD card with the following format:

```json
{
  "project": "Goodman",
  "created": "Feb 06 2026",
  "description": "Goodman heatpump controller",
  "wifi": {
    "ssid": "your-ssid",
    "password": "your-password"
  },
  "mqtt": {
    "user": "mqtt-user",
    "password": "mqtt-password",
    "host": "192.168.0.46",
    "port": 1883
  },
  "logging": {
    "maxLogSize": 52428800,
    "maxOldLogCount": 10
  },
  "runtime": {
    "heatAccumulatedMs": 0
  },
  "timezone": {
    "gmtOffset": -21600,
    "daylightOffset": 3600
  },
  "lowTemp": {
    "threshold": 20.0
  },
  "sensors": {
    "temp": {
      "288514B2000000EA": { "description": "AMBIENT_TEMP", "name": "AMBIENT_TEMP" },
      "28C7E8B200000076": { "description": "CONDENSER_TEMP", "name": "CONDENSER_TEMP" },
      "28DCC0B200000013": { "description": "COMPRESSOR_TEMP", "name": "COMPRESSOR_TEMP" },
      "2862D5B2000000A9": { "description": "SUCTION_TEMP", "name": "SUCTION_TEMP" }
    }
  }
}
```

**Configuration options:**
- `logging.maxLogSize` — Maximum log file size in bytes before rotation (default: 52428800 = 50MB)
- `logging.maxOldLogCount` — Number of rotated log files to keep (default: 10)

**Log file rotation:**
- Active log: `/log.txt` (uncompressed)
- Rotated logs: `/log.1.tar.gz`, `/log.2.tar.gz`, ... `/log.N.tar.gz` (compressed)
- When `/log.txt` exceeds `maxLogSize`, it is compressed and rotated
- Oldest log is deleted when count exceeds `maxOldLogCount`
- Falls back to `.txt` extension if compression fails

**In-memory log ring buffer:**
- Stores the last 500 log entries in PSRAM for fast retrieval
- Access via `GET /log` — returns all entries as JSON
- Use `GET /log?limit=N` to return only the last N entries
- Response format:
  ```json
  {"count": 42, "entries": ["[2026/02/10 14:32:01] [INFO ] [HP] State changed", "..."]}
  ```

**WebSocket log streaming:**
- All log entries are broadcast to connected `/ws` clients in real-time
- Message format: `{"type":"log","message":"[2026/02/10 14:32:01] [INFO ] [HP] ..."}`
- Enabled by default; toggle via `POST /log/config?websocket=true|false`

Sensor addresses are discovered automatically on startup and can be mapped to names via this config.

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/temps` | Current temperature readings |
| GET | `/heap` | Memory/heap statistics |
| GET | `/scan` | WiFi network scan |
| GET | `/log` | Recent log entries from ring buffer |
| GET | `/log/level` | Current log level |
| POST | `/log/level` | Set log level |
| GET | `/log/config` | Logger output configuration |
| POST | `/log/config` | Configure logger outputs (serial, mqtt, sdcard, websocket) |
| GET | `/i2c/scan` | Scan I2C bus for connected devices |
| GET | `/update` | OTA firmware update page |
| POST | `/update` | Upload new firmware |
| WS | `/ws` | WebSocket for real-time data and log streaming |

## MQTT Topics

The controller publishes to a configurable MQTT broker (default `192.168.0.46:1883`). Subscribe with `mosquitto_sub -t "goodman/#"` to receive all topics.

### `goodman/log`

Log messages from the Logger. Published as plain text strings in the format:

```
[2026/02/10 14:32:01] [INFO] [HP] State changed: OFF -> HEAT
```

### `goodman/temps`

Temperature sensor readings, published whenever any sensor value changes.

```json
{
  "COMPRESSOR_TEMP": 72.5,
  "SUCTION_TEMP": 65.2,
  "AMBIENT_TEMP": 48.1,
  "CONDENSER_TEMP": 38.7,
  "LIQUID_TEMP": 185.3
}
```

Only sensors with valid readings are included. Values are in Fahrenheit.

### `goodman/state`

Full controller state, published on every state transition, fault event, and compressor overtemp change.

```json
{
  "state": "HEAT",
  "inputs": {
    "LPS": true,
    "DFT": false,
    "Y": true,
    "O": false
  },
  "outputs": {
    "FAN": true,
    "CNT": true,
    "W": false,
    "RV": false
  },
  "heatRuntimeMin": 42,
  "defrost": false,
  "lpsFault": false,
  "lowTemp": false,
  "compressorOverTemp": false,
  "suctionLowTemp": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | Current state: `OFF`, `HEAT`, `COOL`, `DEFROST`, `ERROR`, or `LOW_TEMP` |
| `inputs` | object | Input pin active states (true = active) |
| `outputs` | object | Output pin states (true = on) |
| `heatRuntimeMin` | number | Accumulated HEAT mode CNT runtime in minutes |
| `defrost` | bool | Whether a software defrost cycle is active |
| `lpsFault` | bool | Whether an LPS low-pressure fault is active |
| `lowTemp` | bool | Whether ambient temperature is below the low-temp threshold |
| `compressorOverTemp` | bool | Whether compressor temperature exceeds 240°F threshold |
| `suctionLowTemp` | bool | Whether suction temperature is critically low in COOL mode (< 32°F) |

### `goodman/fault`

Fault events, published when a fault activates or clears.

```json
{
  "fault": "LPS",
  "message": "Low refrigerant pressure",
  "active": true
}
```

When the fault clears:

```json
{
  "fault": "LPS",
  "message": "Low refrigerant pressure cleared",
  "active": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `fault` | string | Fault identifier (currently `LPS`) |
| `message` | string | Human-readable fault description |
| `active` | bool | `true` when fault activates, `false` when cleared |

## Build Notes

- **PSRAM allocation** — Global `operator new` and `operator delete` are overridden in `src/PSRAMAllocator.cpp` to route all heap allocations through PSRAM via `ps_malloc()` when available, falling back to standard `malloc()` otherwise. PSRAM is initialized early using `__attribute__((constructor(101)))`, which runs before C++ global constructors, ensuring PSRAM is available for any static object that allocates memory. The `BOARD_HAS_PSRAM` build flag must be defined in `platformio.ini` for the ESP-IDF framework to enable PSRAM support. This approach keeps allocation logic out of `main.cpp` and avoids per-allocation init checks.

- **AsyncTCP watchdog** — The `CONFIG_ASYNC_TCP_USE_WDT=0` build flag is required in `platformio.ini`. Without it, AsyncTCP subscribes its task to the ESP-IDF task watchdog (5s timeout). When the MQTT broker is slow or unreachable, the async_tcp task cannot reset the watchdog in time, causing a panic and reboot. This flag prevents the async_tcp task from registering with the watchdog.

## Dependencies

Managed automatically by PlatformIO. Key libraries:

- [TaskScheduler](https://github.com/arkhipenko/TaskScheduler) — Cooperative multitasking
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) — OneWire sensor driver
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — Async HTTP/WebSocket server
- [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) — MQTT client
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing/serialization
- [SdFat](https://github.com/adafruit/SdFat) — SD card filesystem
- [ESP32-targz](https://github.com/tobozo/ESP32-targz) — tar.gz compression for log rotation
- [Adafruit MCP9600](https://github.com/adafruit/Adafruit_MCP9600) — I2C thermocouple amplifier driver
