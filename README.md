# GoodmanHPCtrl

ESP32-based controller for Goodman heatpumps with support for cooling, heating, and defrost modes.

## Web Interface

| Page | Screenshot |
|------|-----------|
| Home | ![Home](docs/screenshots/home.png?v=4) |
| Dashboard | ![Dashboard](docs/screenshots/dashboard.png?v=5) |
| Pins | ![Pins](docs/screenshots/pins.png?v=2) |
| Configuration | ![Configuration](docs/screenshots/config.png?v=4) |
| OTA Update | ![OTA Update](docs/screenshots/update.png?v=4) |
| Log | ![Log](docs/screenshots/log.png?v=1) |
| Heap | ![Heap](docs/screenshots/heap.png?v=1) |

## Features

- **Relay control** — 4 output pins (FAN, Contactor, W-Heat, Reversing Valve) driven by 4 input signals (Low Pressure Switch, Defrost, Y-Cool, O-Heat)
- **Temperature monitoring** — 4 OneWire (Dallas DS18B20) sensors (compressor, suction, ambient, condenser) + 1 MCP9600 I2C thermocouple (liquid line)
- **Remote access** — REST API, WebSocket, and MQTT for monitoring and control
- **HTTPS/SSL** — Self-signed ECC P-256 certificate on port 443 for secure `/config`, `/update`, and `/ftp` endpoints. Graceful fallback to HTTP-only if no certs found on SD card
- **Dark/light theme** — Configurable dark/light theme with shared `theme.css` stylesheet. Persisted to SD card config, cached in localStorage for flash-free page loads. Instant preview on config page
- **Admin password protection** — HTTP Basic Auth on sensitive endpoints (`/config`, `/update`, `/ftp`). No password = open access
- **Password encryption** — All passwords (WiFi, MQTT, admin) encrypted at rest on SD card
- **Live dashboard** — Real-time dashboard at `/dashboard` with state banner, protection status pills (startup lockout countdown, short cycle, RV fail, high suction temp), input/output grid, temperatures, and reboot button
- **Pin table with manual override** — Auth-protected `/pins` page showing all GPIO inputs, outputs, and temperatures in a table. "Normal Mode Lockout" checkbox enables manual output control, bypassing the state machine for up to 30 minutes (auto-timeout). CNT enforces short cycle protection even in manual mode. Single auth prompt covers the entire lockout session. "Force Defrost" button triggers a software defrost cycle from HEAT mode (requires no active faults or manual override)
- **Temperature history** — Configurable CSV logging interval (30s-5min, default 2min) per sensor to SD card (`/temps/<sensor>/YYYY-MM-DD.csv`), rolling Canvas line charts on dashboard with 1h/6h/24h/7d timeframe selector, auto-purge after 31 days
- **Web-based configuration** — HTML pages served from `/www/` on SD card for configuration, OTA updates, and monitoring
- **FTP server** — SimpleFTPServer with timed enable/disable (10/30/60 min) from config page. Defaults to OFF; auto-disables after timeout
- **OTA updates** — Firmware upload saves to SD card (`/firmware.new`), then apply to flash. Supports revert to previous firmware from SD backup
- **SD card configuration** — WiFi, MQTT, and sensor settings stored as JSON on SD card
- **Multi-output logging** — Serial, MQTT, SD card with tar.gz compressed log rotation, and WebSocket streaming
- **In-memory log buffer** — 500-entry ring buffer in PSRAM, accessible via `/log` API endpoint
- **NTP time sync** — Automatic time synchronization from NTP servers, refreshes every 2 hours
- **I2C bus** — Initialized on GPIO8 (SDA) / GPIO9 (SCL) with automatic device scan at startup and `/i2c/scan` API endpoint
- **PSRAM support** — All heap allocations routed through PSRAM when available
- **WiFi AP fallback** — Configurable timeout (default 10 minutes) before switching to Access Point mode for OTA recovery and reconfiguration
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
  - Turns on W (auxiliary heat) in HEAT mode only
  - If thermostat switches to COOL mode (Y+O) while in LOW_TEMP, W is turned off — no heating or cooling operates below 20°F in COOL mode
  - Blocks all compressor activation (CNT) while ambient temp is too low
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

### State Table

| State | Condition | FAN | CNT | RV | W | Notes |
|-------|-----------|-----|-----|----|---|-------|
| `OFF` | Y inactive | OFF | OFF | OFF | OFF | All outputs off |
| `HEAT` | Y active, O inactive | ON | ON | OFF | OFF | CNT has 30s short-cycle protection |
| `COOL` | Y active, O active | ON | ON | ON | OFF | Will not operate below 20°F |
| `DEFROST` | Software defrost active, Y active | OFF | ON | ON | ON | 3-min minimum, exits at condenser > 60°F or 15-min timeout |
| `ERROR` | LPS fault (low pressure) | OFF | OFF | OFF | ON* | *W on only in HEAT mode (Y active, O inactive) |
| `LOW_TEMP` | Ambient < 20°F | OFF | OFF | OFF | ON* | *W on only in HEAT mode; W turns off if thermostat switches to COOL (Y+O) |

**Fault Conditions** (overlay on current state, block CNT activation):

| Fault | Condition | Trigger | Recovery | FAN | CNT | W | Priority |
|-------|-----------|---------|----------|-----|-----|---|----------|
| Compressor overtemp | COMPRESSOR_TEMP ≥ 240°F | Any mode | Temp < 190°F | ON | OFF | OFF | 1 (highest) |
| Suction low temp | SUCTION_TEMP < 32°F | COOL mode only | Temp > 40°F | ON | OFF | OFF | 2 |
| LPS fault | LPS input LOW | Any mode | LPS goes HIGH | OFF | OFF | ON* | 3 |
| Low ambient temp | AMBIENT_TEMP < 20°F | Any mode | Temp ≥ 20°F | OFF | OFF | ON* | 4 |

\* W on only in HEAT mode (Y active, O inactive); never activated in COOL mode (Y+O).

**Fault Priority:** Compressor overtemp > Suction low temp > LPS fault > Low ambient temp. Higher-priority faults take precedence; lower-priority checks are skipped while a higher-priority fault is active.

### Class Structure

| Class | Purpose |
|-------|---------|
| `GoodmanHP` | Central controller with pin maps, temp sensors, and state machine |
| `InputPin` | Digital/analog input with ISR, debouncing, callbacks |
| `OutPin` | Output relay with delay, PWM support, state tracking, hardware state validation |
| `TempSensor` | Temperature sensor with callbacks; supports OneWire (DS18B20) and I2C (MCP9600) |
| `Config` | SD card and JSON configuration management |
| `Logger` | Multi-output logging with tar.gz rotation, ring buffer, and WebSocket streaming |
| `WebHandler` | AsyncWebServer (port 80) with REST API, WebSocket, and HTTPS redirects |
| `HttpsServer` | ESP-IDF HTTPS server (port 443) for secure endpoints |
| `MQTTHandler` | MQTT client with auto-reconnect and topic publishing |

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

### Secrets Setup

Create `secrets.ini` in the project root (gitignored — never committed):

```ini
[secrets]
build_flags =
	-D AP_PASSWORD=\"your-ap-password\"
	-D XOR_KEY=\"your-random-base64-key\"
```

`AP_PASSWORD` is used for WiFi AP fallback mode. `XOR_KEY` is used for password encryption on the SD card (see [Password Encryption](#password-encryption)). Generate a random key with: `openssl rand -base64 32`

### Build and Upload

```bash
# Build
pio run -e freenove_esp32_s3_wroom

# Upload firmware
pio run -t upload -e freenove_esp32_s3_wroom

# Serial monitor
pio run -t monitor -e freenove_esp32_s3_wroom
```

### SD Card Setup

The SD card should contain:

```
/config.txt              — Device configuration (WiFi, MQTT, sensors, admin password)
/www/index.html          — Home page
/www/dashboard.html      — Live dashboard with charts
/www/pins.html           — Pin table with manual override
/www/config.html         — Configuration page
/www/update.html         — OTA update page
/www/log.html            — Log viewer
/www/heap.html           — System/heap info
/www/wifi.html           — WiFi scan and setup
/www/theme.css           — Shared dark/light theme stylesheet
/cert.pem                — HTTPS certificate (optional, see below)
/key.pem                 — HTTPS private key (optional, see below)
/temps/<sensor>/*.csv    — Temperature history CSVs (auto-created)
```

**Generate config.txt interactively:**

```bash
./scripts/configure.sh --local
```

This prompts for WiFi and MQTT credentials and writes `data/config.txt`. Copy it to the SD card root. Passwords are stored in plaintext and encrypted automatically on first boot.

**Manual config.txt format:**

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
  "heatpump": {
    "lowTemp": { "threshold": 20.0 },
    "highSuctionTemp": { "threshold": 140.0, "rvFail": false },
    "shortCycle": { "rv": 30000, "cnt": 30000 }
  },
  "tempHistory": {
    "intervalSec": 120
  },
  "ui": {
    "theme": "dark"
  },
  "admin": {
    "password": ""
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
- `tempHistory.intervalSec` — Temperature history capture interval in seconds, 30-300 (default: 120)
- `heatpump.lowTemp.threshold` — Ambient temp (°F) below which compressor is blocked (default: 20.0)
- `heatpump.highSuctionTemp.threshold` — Suction temp (°F) above which RV fail is detected during defrost (default: 140.0)
- `heatpump.shortCycle.rv` — RV pressure equalization delay in ms during defrost transition (default: 30000)
- `heatpump.shortCycle.cnt` — CNT short cycle delay in ms on Y activation (default: 30000)

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

**Temperature history CSV logging:**
- Logs all 5 temperature sensors (ambient, compressor, suction, condenser, liquid) at a configurable interval (30s-5min, default 2min)
- Per-sensor CSV files: `/temps/<sensor>/YYYY-MM-DD.csv` (e.g., `/temps/ambient/2026-02-11.csv`)
- CSV format (no header): `epoch_seconds,temperature_fahrenheit`
- ~56 KB/day per sensor, ~8.5 MB/month total across all sensors
- Auto-purges CSV files older than 31 days
- Access via `GET /temps/history?sensor=<name>` API endpoint

Sensor addresses are discovered automatically on startup and can be mapped to names via this config.

### HTTPS / SSL

The device runs a secondary HTTPS server (ESP-IDF `esp_https_server`) on port 443 for sensitive endpoints (`/config`, `/update`, `/ftp`). The AsyncWebServer on port 80 redirects those paths to HTTPS.

**Generate a self-signed certificate:**

```bash
./scripts/generate-cert.sh
```

This creates `cert.pem` and `key.pem` (ECC P-256, 10-year validity). Copy both to the SD card root. If no certificates are found, all endpoints fall back to HTTP.

### Admin Password

Sensitive endpoints (`/config`, `/update`, `/ftp`) are protected by HTTP Basic Auth when an admin password is set.

- **No password set** — All endpoints are open, no authentication required
- **Password set** — Browser prompts for Basic Auth (username: `admin`, password: your admin password)
- Set the admin password from the config page (`/config`) or via the API
- Setting a password automatically disables FTP if it was running

### Password Encryption

All passwords (WiFi, MQTT, admin) are encrypted at rest on the SD card. Plaintext passwords are automatically encrypted on the next config save.

Optionally, an eFuse HMAC key can be provisioned for hardware-backed encryption:

```bash
./scripts/burn-efuse-key.sh [/dev/ttyUSB0]
```

This permanently burns a random 256-bit HMAC key to eFuse BLOCK_KEY0. Without an eFuse key, passwords are encrypted using a stable key from `secrets.ini`.

### FTP Server

FTP (SimpleFTPServer on port 21) is used for uploading HTML files to the SD card's `/www/` directory.

- **FTP defaults to OFF** at boot
- Enabling/disabling FTP requires admin authentication (via `/ftp` API endpoint)
- Enable from the config page with timed durations (10/30/60 min), auto-disables after timeout
- FTP login credentials are `admin`/`admin` (separate from admin password)

**Upload web pages via FTP:**

```bash
./scripts/update-www.sh
```

This script prompts for the device IP and admin password, enables FTP for 10 minutes, and uploads all files from `data/www/` to the device's `/www/` directory.

### WiFi AP Fallback

If the device cannot connect to WiFi for 20 minutes, it automatically switches to Access Point (AP) mode for emergency OTA updates and configuration changes.

- **SSID:** `GoodmanHP`
- **Password:** Defined at build time via `AP_PASSWORD` in `secrets.ini` (gitignored)
- **IP:** `192.168.4.1`
- All web endpoints work in AP mode (dashboard, config, OTA update, log, heap)
- HTTPS is not available in AP mode — use HTTP (`http://192.168.4.1`)
- AP credentials are logged overtly at WARN level to both serial and log file
- AP mode persists until reboot

**Setup:** The AP password is defined in `secrets.ini` (see [Secrets Setup](#secrets-setup)). The build will fail with a clear `#error` if `secrets.ini` is missing or `AP_PASSWORD` is not defined.

**Recovery workflow:**
1. Connect to `GoodmanHP` WiFi network with the AP password
2. Browse to `http://192.168.4.1/config` to fix WiFi credentials
3. Or upload new firmware via `http://192.168.4.1/update`
4. Reboot the device to reconnect to the configured WiFi network

## Scripts

All scripts prompt interactively for required parameters (device IP, admin password, etc.).

| Script | Description | Parameters |
|--------|-------------|------------|
| `configure.sh` | Configure WiFi/MQTT credentials on device or generate local config | `--local` (optional): generate `data/config.txt` instead of pushing to device |
| `configure.sh --local` | Generate `data/config.txt` for SD card provisioning | None (interactive prompts only) |
| `ota-update.sh` | OTA firmware upload, verify, flash, and reboot via HTTPS | `--revert` (optional): roll back to previous firmware backup |
| `update-www.sh` | Upload HTML files from `data/www/` to device SD card via FTP | None (interactive prompts only) |
| `backup-config.sh` | Download `config.txt` from device SD card for local backup | None (interactive prompts only) |
| `generate-cert.sh` | Generate self-signed ECC P-256 cert for HTTPS | None (no prompts, requires `openssl`) |
| `burn-efuse-key.sh` | Burn hardware encryption key to ESP32-S3 eFuse | `[PORT]` (optional, default: `/dev/ttyUSB0`) |

**Interactive prompts** (where applicable): Device IP, Admin password, WiFi/MQTT credentials, confirmation prompts.

### `scripts/configure.sh`

Configure WiFi and MQTT credentials on the device or generate a local config file for SD card provisioning.

```
./scripts/configure.sh           # Push config to device via HTTPS API
./scripts/configure.sh --local   # Generate data/config.txt for SD card
```

**Prompts (network mode):** Device IP, Admin password, WiFi SSID, WiFi password, Current WiFi password (if changing), MQTT host/port/user, MQTT password, Current MQTT password (if changing)

**Prompts (local mode):** WiFi SSID, WiFi password, MQTT host/port/user, MQTT password

### `scripts/ota-update.sh`

OTA firmware update via HTTPS. Uploads the PlatformIO build output to the device SD card, verifies the upload size, applies (backs up current firmware + flashes new), and waits for reboot.

```
./scripts/ota-update.sh           # Upload and flash firmware
./scripts/ota-update.sh --revert  # Roll back to previous firmware backup
```

**Prompts:** Device IP, Admin password

**Requires:** Built firmware at `.pio/build/freenove_esp32_s3_wroom/firmware.bin`

### `scripts/update-www.sh`

Upload all HTML files from `data/www/` to the device SD card `/www/` directory via FTP. Automatically enables FTP for 10 minutes.

```
./scripts/update-www.sh
```

**Prompts:** Device IP, Admin password

### `scripts/backup-config.sh`

Download `config.txt` from the device SD card for local backup via FTP. Saves timestamped copies to `backups/<YYYYMMDD-HHMMSS>/config.txt` and a latest copy at `backups/config-latest.txt`.

```
./scripts/backup-config.sh
```

**Prompts:** Device IP, Admin password

**Note:** The `backups/` directory is gitignored since config files contain credentials.

### `scripts/generate-cert.sh`

Generate a self-signed ECC P-256 certificate (10-year validity) for the ESP32 HTTPS server. Outputs `cert.pem` and `key.pem` to the project root.

```
./scripts/generate-cert.sh
```

**No prompts.** Requires `openssl`. Copy output files to SD card root.

### `scripts/burn-efuse-key.sh`

Burn a hardware encryption key to ESP32-S3 eFuse for password encryption at rest. The key is read-protected — only the hardware HMAC peripheral can access it.

```
./scripts/burn-efuse-key.sh [PORT]
```

**Parameters:** `PORT` — serial port (default: `/dev/ttyUSB0`)

**Prompts:** Two confirmation prompts (type `yes`, then `BURN`)

**WARNING:** eFuse burning is permanent and irreversible. Each key block can only be written once per chip. A backup of the key is saved to `efuse_hmac_key.bin` (gitignored).

## API Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/` | | Home page (served from SD `/www/index.html`) |
| GET | `/dashboard` | | Live dashboard with state, I/O, temps, and charts |
| GET | `/pins` | Yes | Pin table page / JSON (`?format=json`) with manual override control |
| POST | `/pins` | Yes | Toggle manual override, set output state, or force defrost (JSON body) |
| GET | `/state` | | Full controller state as JSON (see below) |
| GET | `/temps` | | Current temperature readings |
| GET | `/temps/history` | | Temperature history CSV data (`?sensor=<name>`, optional `&date=YYYY-MM-DD`) |
| GET | `/heap` | | Memory/heap statistics |
| GET | `/scan` | | WiFi network scan |
| GET | `/log` | | Recent log entries from ring buffer (`?limit=N`) |
| GET | `/log/level` | | Current log level |
| POST | `/log/level` | | Set log level |
| GET | `/log/config` | | Logger output configuration |
| POST | `/log/config` | | Configure logger outputs (serial, mqtt, sdcard, websocket) |
| GET | `/theme` | | Current theme setting (`{"theme":"dark"}`) |
| GET | `/theme.css` | | Shared dark/light theme CSS stylesheet |
| GET | `/i2c/scan` | | Scan I2C bus for connected devices |
| GET | `/config` | Yes | Configuration page / JSON (`?format=json`) |
| POST | `/config` | Yes | Update configuration (JSON body) |
| GET | `/update` | Yes | OTA firmware update page |
| POST | `/update` | Yes | Upload new firmware (saved to SD as `/firmware.new`) |
| GET | `/apply` | Yes | Check if uploaded firmware exists (`{"exists":bool,"size":N}`) |
| POST | `/apply` | Yes | Flash firmware from `/firmware.new`, reboots on success |
| GET | `/revert` | Yes | Check if firmware backup exists (`{"exists":bool,"size":N}`) |
| POST | `/revert` | Yes | Revert to previous firmware from SD backup, reboots on success |
| POST | `/reboot` | Yes | Reboot the device (2s delay) |
| GET | `/ftp` | Yes | FTP server status (`{"active":bool,"remainingMinutes":N}`) |
| POST | `/ftp` | Yes | Enable/disable FTP (`{"duration":N}` minutes, 0=off) |
| WS | `/ws` | | WebSocket for real-time data and log streaming |

**Auth** = Requires HTTP Basic Auth when admin password is set. Endpoints marked with "Yes" redirect to HTTPS (port 443) when SSL certificates are available.

### `GET /state`

Returns the full controller state as JSON. Used by the dashboard for real-time polling.

```json
{
  "state": "HEAT",
  "inputs": { "LPS": true, "DFT": false, "Y": true, "O": false },
  "outputs": { "FAN": true, "CNT": true, "W": false, "RV": false },
  "heatRuntimeMin": 42,
  "defrost": false,
  "lpsFault": false,
  "lowTemp": false,
  "compressorOverTemp": false,
  "suctionLowTemp": false,
  "startupLockout": false,
  "startupLockoutRemainSec": 0,
  "shortCycleProtection": false,
  "rvFail": false,
  "highSuctionTemp": false,
  "defrostTransition": false,
  "defrostTransitionRemainSec": 0,
  "manualOverride": false,
  "manualOverrideRemainSec": 0,
  "temps": { "AMBIENT_TEMP": 48.1, "COMPRESSOR_TEMP": 72.5, "SUCTION_TEMP": 65.2, "CONDENSER_TEMP": 38.7, "LIQUID_TEMP": 185.3 },
  "cpuLoad0": 23,
  "cpuLoad1": 50,
  "freeHeap": 159232,
  "wifiSSID": "your-ssid",
  "wifiRSSI": -48,
  "wifiIP": "192.168.1.136",
  "apMode": false,
  "buildDate": "Feb 12 2026 03:18:44"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `startupLockout` | bool | Whether the 5-minute startup lockout is active |
| `startupLockoutRemainSec` | number | Seconds remaining in startup lockout (0 when inactive) |
| `shortCycleProtection` | bool | Whether short-cycle protection delay is active on CNT |
| `rvFail` | bool | Whether RV fail (high suction temp during defrost) is latched |
| `highSuctionTemp` | bool | Whether suction temp is above threshold during defrost |
| `defrostTransition` | bool | Whether RV pressure equalization delay is active |
| `defrostTransitionRemainSec` | number | Seconds remaining in defrost transition (0 when inactive) |
| `manualOverride` | bool | Whether manual override (pin control page) is active |
| `manualOverrideRemainSec` | number | Seconds remaining in manual override (0 when inactive) |
| `cpuLoad0` | number | CPU load percentage for Core 0 (WiFi/protocol stack) |
| `cpuLoad1` | number | CPU load percentage for Core 1 (Arduino loop/tasks) |
| `freeHeap` | number | Free heap memory in bytes |
| `wifiSSID` | string | Connected WiFi network name |
| `wifiRSSI` | number | WiFi signal strength in dBm |
| `wifiIP` | string | Device IP address |
| `apMode` | bool | Whether the device is in AP fallback mode |
| `buildDate` | string | Firmware build date and time (compile timestamp) |

### `GET /temps/history`

Returns temperature history CSV data for a specific sensor. Requires `?sensor=` parameter.

**List available files:**
```
GET /temps/history?sensor=ambient
→ {"files":[{"date":"2026-02-11","size":56000},{"date":"2026-02-10","size":55800}]}
```

**Download a day's CSV:**
```
GET /temps/history?sensor=ambient&date=2026-02-11
→ 1739318400,48.1
  1739318430,48.2
  ...
```

Valid sensor names: `ambient`, `compressor`, `suction`, `condenser`, `liquid`

### OTA Firmware Update Workflow

1. `POST /update` — Upload firmware binary (saved to SD card as `/firmware.new`)
2. `GET /apply` — Verify uploaded firmware exists and check size
3. `POST /apply` — Flash firmware from SD to ESP32, reboots automatically
4. `POST /revert` — Roll back to previous firmware (backup created during apply)

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
  "suctionLowTemp": false,
  "startupLockout": false,
  "startupLockoutRemainSec": 0,
  "shortCycleProtection": false,
  "rvFail": false,
  "highSuctionTemp": false,
  "manualOverride": false
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
| `startupLockout` | bool | Whether the 5-minute startup lockout is active |
| `startupLockoutRemainSec` | number | Seconds remaining in startup lockout (0 when inactive) |
| `shortCycleProtection` | bool | Whether short-cycle protection delay is active on CNT |
| `rvFail` | bool | Whether RV fail (high suction temp during defrost) is latched |
| `highSuctionTemp` | bool | Whether suction temp is above threshold during defrost |
| `manualOverride` | bool | Whether manual override is active from pin control page |
| `apMode` | bool | Whether the device is in AP fallback mode |

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

- **HTTPS server separation** — `HttpsServer.cpp` is in a separate translation unit because `esp_https_server.h` (ESP-IDF) and `ESPAsyncWebServer.h` both define `HTTP_PUT`, `HTTP_OPTIONS`, and `HTTP_PATCH` enums and cannot coexist in the same TU. Logger.h forward-declares `AsyncWebSocket` to avoid pulling in the ESPAsyncWebServer header chain.

## Dependencies

Managed automatically by PlatformIO. Key libraries:

- [TaskScheduler](https://github.com/arkhipenko/TaskScheduler) — Cooperative multitasking
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) — OneWire sensor driver
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — Async HTTP/WebSocket server
- [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) — MQTT client
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing/serialization
- [ESP32-targz](https://github.com/tobozo/ESP32-targz) — tar.gz compression for log rotation
- [Adafruit MCP9600](https://github.com/adafruit/Adafruit_MCP9600) — I2C thermocouple amplifier driver
- [SD](https://github.com/espressif/arduino-esp32/tree/master/libraries/SD) — Arduino SD card library (used for all file operations)
- [SimpleFTPServer](https://github.com/xreef/SimpleFTPServer) — FTP server for SD card file uploads (STORAGE_SD mode)
