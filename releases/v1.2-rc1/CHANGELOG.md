# v1.2-rc1 — Release Candidate 1

**Date:** 2026-02-25
**Board:** Freenove ESP32-S3 WROOM N8R8 (8MB Flash / 8MB PSRAM)
**Build:** PlatformIO espressif32 6.12.0, Arduino ESP32 3.20017

## New Features

- **HEAT↔COOL mode transitions** — 3-phase sequenced reversing valve switching for both HEAT→COOL and COOL→HEAT, with pressure equalization and short cycle protection
- **State validation delay** — Configurable hold time (default 30s) after state changes prevents rapid cycling; HIGH priority states (OFF, ERROR) bypass
- **Table-driven output validation** — `validateOutputStates()` verifies all outputs match expected state every 10s, covers all transition phases, auto-corrects mismatches
- **Input pin polling** — Replaced ISR-based input detection with polling-based validation (default 2s delay), more reliable on ESP32-S3
- **DFT debounce** — 30-second sustained off required before heat runtime resets, prevents false resets from intermittent DFT contact
- **Defrost elapsed timer** — Dashboard pill shows running M:SS count-up during active defrost; orange "Defrost Pending" state when defrost flag set but Y dropped
- **WiFi AP mode** — Automatic fallback to AP mode after configurable WiFi disconnect timeout; random password, OLED display, auto-reconnect
- **Session-based authentication** — Login page with configurable session timeout, Basic Auth fallback for API/script compatibility
- **Boot watchdog + safe mode** — Auto-detects crash boot loops (3+ consecutive resets), enters safe mode; auto-reverts to backup firmware if available
- **OTA auto-revert** — Backs up running firmware before OTA update; reverts on crash boot loop if backup has different build date
- **Configurable system name + MQTT prefix** — Multi-unit support with customizable system name and MQTT topic prefix
- **SSD1306 OLED display** — 128x64 I2C display with 5 rotating pages (status, temps, I/O, system, protections)
- **MAX6675 SPI thermocouple** — Additional thermocouple source for LIQUID_TEMP with configurable pin mapping
- **Configurable FTP password** — Encrypted FTP credentials with timed enable (10/30/60 min auto-disable)
- **Configurable W/AUX in LOW_TEMP** — Independent enable checkboxes for W (auxiliary heat) and AUX relay in low ambient temp mode

## Bug Fixes

- Fix defrost pill showing red with stale elapsed time in OFF state when defrost is pending
- Fix `_defrostStartTick` not resetting on Y drop, causing stale elapsed time on resume
- Skip state validation timer during manual override
- Block manual override during startup lockout to prevent CNT activation
- Fix FTP password dangling pointer
- Fix HTTPS client IP logging
- Block USB JTAG and SD SPI bus pins from MAX6675 GPIO selection
- Fix AP test mode disconnecting WiFi before HTTP response completes
- Avoid bare I2C probe on MCP9600 to prevent chip crash and bus lockup
- Fix inflated CPU load by yielding every loop iteration
- Fix reboot button authentication on pins page

## Configuration Changes

New config keys (all backward-compatible with defaults):
- `heatpump.stateValidation.delayMs` — State hold time in ms (default 30000)
- `heatpump.inputDelay.ms` — Input validation delay in ms (default 2000)
- `heatpump.lowTemp.enableW` / `enableAux` — W/AUX enable in LOW_TEMP (default true)
- `wifi.apFallbackSeconds` — WiFi disconnect timeout before AP mode (default 600)
- `wifi.apPassword` — AP mode password (blank = auto-generate)
- `admin.password` — Admin password for web auth
- `ui.theme` — Light/dark theme preference

## Files

| File | Description |
|------|-------------|
| `firmware.bin` | ESP32-S3 firmware binary (OTA or USB flash) |
| `littlefs.bin` | LittleFS filesystem image (web UI pages) |

## Flash Instructions

**OTA (preferred):**
```bash
# Upload firmware
curl -k -u admin:PASSWORD https://DEVICE_IP/update --data-binary @firmware.bin

# Apply (backs up current firmware first)
curl -k -u admin:PASSWORD -X POST https://DEVICE_IP/apply

# Upload LittleFS (web pages)
curl -k -u admin:PASSWORD https://DEVICE_IP/upload/www -F "file=@littlefs.bin"
```

**USB:**
```bash
# Firmware
pio run -t upload -e freenove_esp32_s3_wroom

# LittleFS
pio run -t uploadfs -e freenove_esp32_s3_wroom
```
