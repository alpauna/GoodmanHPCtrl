# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Embedded HVAC controller for Goodman heatpumps (cooling, heating, defrost modes) running on ESP32. Controls 4 relay outputs (FAN, CNT, W, RV) based on 4 input signals (LPS, DFT, Y, O) and 4 OneWire temperature sensors (LINE, SUCTION, AMBIENT, CONDENSER). Provides a REST API, WebSocket, and MQTT interface for remote monitoring and control.

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
| `src/main.cpp` | Application entry point, setup/loop, tasks, web API, MQTT, SD card config |
| `src/OutPin.cpp` | Output relay control implementation |
| `src/InputPin.cpp` | Input pin handling implementation |
| `src/Logger.cpp` | Multi-output logging with tar.gz rotation |
| `include/OutPin.h` | OutPin class, OutputPinCallback typedef |
| `include/InputPin.h` | InputPin class, InputResistorType/InputPinType enums, InputPinCallback typedef |
| `include/Logger.h` | Logger class |

### Execution Model

Task-based cooperative scheduling using TaskScheduler with two scheduler instances (`ts` main, `hts` high-priority). The Arduino `loop()` calls `ts.execute()` each iteration. Key scheduled tasks:

| Task | Interval | Purpose |
|------|----------|---------|
| `tCheckTemps` | 10s | Read OneWire temperature sensors |
| `tRuntime` | 1min | Update runtime counter |
| `_tGetInputs` | 500ms | Process queued input pin changes |
| `tConnectMQQT` | 1s | MQTT reconnection (disables itself on success) |
| `tWaitOnWiFi` | 1s x60 | WiFi connection wait |
| `tNtpSync` | 2h | NTP time sync (enabled on WiFi connect) |

### Memory Management

Global `operator new`/`delete` are overridden to route all allocations through PSRAM (`ps_malloc`) when available, falling back to regular `malloc`.

### I/O Classes

- **OutPin** (`OutPin.h/cpp`): Output relay control with configurable activation delay, PWM support, on/off counters, and callback on state change. Delay is implemented via a TaskScheduler task.
- **InputPin** (`InputPin.h/cpp`): Digital/analog input with configurable pull-up/down, ISR-based interrupt detection, debouncing via delayed verification (circular buffer queue checked by `_tGetInputs`), and callback on change.
- **CurrentTemp** (defined in `main.cpp`): Temperature sensor data holder with update/change callbacks.

### GPIO Pin Mapping (ESP32-S3)

Inputs: LPS=GPIO15, DFT=GPIO16, Y=GPIO17, O=GPIO18
Outputs: FAN=GPIO4, CNT=GPIO5 (3s delay), W=GPIO6, RV=GPIO7
OneWire bus: GPIO21

### Networking

- **AsyncWebServer** on port 80 with REST endpoints (`/temps`, `/heap`, `/scan`, `/log/level`, `/log/config`, `/update` for OTA)
- **WebSocket** at `/ws`
- **MQTT** (AsyncMqttClient) to configurable broker, default `192.168.0.46:1883`

### Configuration

JSON config stored on SD card at `/config.txt` (SdFat library, SPI interface). Contains WiFi credentials, MQTT settings, and temperature sensor address-to-name mappings. Loaded during `setup()`, writable via API.

### Logger

Multi-output logging (Serial, MQTT topic, SD card with file rotation). Runtime-configurable level (ERROR/WARN/INFO/DEBUG) and output toggles via HTTP API.

**Log format**: `[YYYY/MM/DD HH:MM:SS] [LEVEL] [TAG] message`
- Uses RTC time from NTP sync when available
- Falls back to `----/--/-- --:--:--` before NTP sync completes

**Log rotation** follows a Linux-style scheme using ESP32-targz for compression:
- `/log.txt` — active log (uncompressed)
- `/log.1.tar.gz` through `/log.N.tar.gz` — rotated archives (compressed)
- Rotation triggers when log file exceeds 50MB (configurable)
- Falls back to plain rename (`.txt`) if compression fails

**SdFat/SD library swap**: The project uses SdFat (Adafruit fork) for normal SD card operations, but ESP32-targz requires Arduino's `SD` library (`fs::FS` interface). During log rotation, Logger temporarily calls `_sd->end()`, initializes Arduino `SD` for compression, then restores SdFat via `_sd->begin()`. This swap is isolated to `compressFile()` in `Logger.cpp`.

### NTP Time Sync

RTC time is synchronized from NTP servers (`pool.ntp.org`, `time.nist.gov`) using the ESP32's built-in SNTP client. The `tNtpSync` task is enabled when WiFi connects, syncs immediately, then repeats every 2 hours. Timezone offset is configurable via `_gmtOffset_sec` and `_daylightOffset_sec` variables in `main.cpp`.

### State Machine

```
enum AC_STATE { OFF, COOL, HEAT, DEFROST }
```

### Control Flow Example

Y input pin ISR fires → change queued in circular buffer → `_tGetInputs` calls `onCheckInputQueue()` → `onInput()` callback → activates/deactivates CNT output relay (with 3s delay on activation).

## Key Build Flags

- `BOARD_ESP32_S3_WROOM` / `BOARD_ESP32_ROVER`: Board-specific conditional compilation
- `BOARD_HAS_PSRAM`: Enables PSRAM allocation path
- `_TASK_TIMEOUT`, `_TASK_STD_FUNCTION`, `_TASK_HEADER_AND_CPP`: TaskScheduler features
- `CIRCULAR_BUFFER_INT_SAFE`: Required; enforced by `#error` directive
- `SD_FAT_TYPE=3`: Uses SdFs (supports FAT16/FAT32/exFAT)
- `DEST_FS_USES_SD`: Required by ESP32-targz to use Arduino SD filesystem
