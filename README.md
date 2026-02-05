# GoodmanHPCtrl

ESP32-based controller for Goodman heatpumps with support for cooling, heating, and defrost modes.

## Features

- **Relay control** — 4 output pins (FAN, Contactor, W-Heat, Reversing Valve) driven by 4 input signals (Low Pressure Switch, Defrost, Y-Cool, O-Heat)
- **Temperature monitoring** — 4 OneWire (Dallas DS18B20) sensors: line, suction, ambient, and condenser
- **Remote access** — REST API, WebSocket, and MQTT for monitoring and control
- **OTA updates** — Firmware upload via web interface
- **SD card configuration** — WiFi, MQTT, and sensor settings stored as JSON on SD card
- **Multi-output logging** — Serial, MQTT, and SD card with tar.gz compressed log rotation
- **NTP time sync** — Automatic time synchronization from NTP servers, refreshes every 2 hours
- **PSRAM support** — All heap allocations routed through PSRAM when available

## Hardware

**Supported boards:**
- Freenove ESP32-S3-WROOM (primary)
- ESP32 DevKit

**GPIO Pin Mapping (ESP32-S3):**

| Pin | GPIO | Direction | Description |
|-----|------|-----------|-------------|
| LPS | 15 | Input | Low Pressure Switch |
| DFT | 16 | Input | Defrost signal |
| Y | 17 | Input | Cooling request |
| O | 18 | Input | Heating request |
| FAN | 4 | Output | Fan relay |
| CNT | 5 | Output | Contactor relay (3s delay) |
| W | 6 | Output | Heating relay |
| RV | 7 | Output | Reversing valve relay |
| OneWire | 21 | I/O | Temperature sensor bus |

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
  "sensors": {
    "temp": {
      "288514B2000000EA": { "description": "AMBIENT_TEMP", "name": "AMBIENT_TEMP" },
      "28C7E8B200000076": { "description": "CONDENSER_TEMP", "name": "CONDENSER_TEMP" },
      "28DCC0B200000013": { "description": "LINE_TEMP", "name": "LINE_TEMP" },
      "2862D5B2000000A9": { "description": "SUCTION_TEMP", "name": "SUCTION_TEMP" }
    }
  }
}
```

Sensor addresses are discovered automatically on startup and can be mapped to names via this config.

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/temps` | Current temperature readings |
| GET | `/heap` | Memory/heap statistics |
| GET | `/scan` | WiFi network scan |
| GET | `/log/level` | Current log level |
| POST | `/log/level` | Set log level |
| GET | `/log/config` | Logger output configuration |
| POST | `/log/config` | Configure logger outputs |
| GET | `/update` | OTA firmware update page |
| POST | `/update` | Upload new firmware |
| WS | `/ws` | WebSocket for real-time data |

## Dependencies

Managed automatically by PlatformIO. Key libraries:

- [TaskScheduler](https://github.com/arkhipenko/TaskScheduler) — Cooperative multitasking
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) — OneWire sensor driver
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — Async HTTP/WebSocket server
- [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) — MQTT client
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing/serialization
- [SdFat](https://github.com/adafruit/SdFat) — SD card filesystem
- [ESP32-targz](https://github.com/tobozo/ESP32-targz) — tar.gz compression for log rotation
