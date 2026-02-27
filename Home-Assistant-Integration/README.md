# Home Assistant Integration for Goodman Heat Pump Controller

This folder contains everything needed to integrate the ESP32 HVAC controller's MQTT telemetry into Home Assistant.

## MQTT Topics Published by the Controller

| Topic | Format | Published When |
|-------|--------|----------------|
| `{prefix}/temps` | JSON: `{"COMPRESSOR_TEMP":72.5,"SUCTION_TEMP":65.2,...}` | Any sensor value changes |
| `{prefix}/state` | JSON: state, inputs, outputs, faults, flags | State transitions |
| `{prefix}/fault` | JSON: `{"fault":"LPS","message":"...","active":true}` | Fault activate/clear |

Default prefix: `goodman` (configurable via `system.mqttPrefix` in device config).

The controller is **publish-only** — it does not subscribe to any command topics.

## Integration Approaches

### 1. Discovery Script (Recommended Starting Point)

**`discovery.py`** — Python script that publishes MQTT auto-discovery messages to your broker. Home Assistant automatically creates all entities grouped under a single device.

```bash
pip install paho-mqtt
python discovery.py --broker 192.168.1.1 --prefix goodman
```

**Pros:** No firmware changes, entities auto-appear in HA, device grouping works out of the box.
**Cons:** Must re-run if broker loses retained messages.

### 2. Manual YAML Configuration

**`configuration.yaml`** — Drop-in MQTT sensor/binary_sensor definitions for HA's `configuration.yaml`.

Copy the contents into your HA config (or use `!include`), restart HA or reload MQTT entities.

**Pros:** No dependencies, full control over entity names/icons.
**Cons:** Tedious to maintain, no automatic device grouping.

### 3. Firmware MQTT Discovery (Future)

Migrate the discovery payloads from the script into `MQTTHandler::publishDiscovery()` so the device self-registers with any HA instance automatically. Requires firmware changes:
- Add LWT (`{prefix}/availability` with `online`/`offline`)
- Publish retained discovery configs on MQTT connect
- Publish availability `online` after connect

### 4. Custom Integration (`custom_components/`)

A full Python integration package. Overkill for read-only telemetry but useful if bidirectional control (sending commands from HA to the controller) is added later.

## Entity Mapping

| Data | HA Entity Type | Device Class | Notes |
|------|---------------|--------------|-------|
| Temperature sensors | `sensor` | `temperature` | `state_class: measurement`, unit `°F` |
| Controller state | `sensor` | `enum` | OFF/COOL/HEAT/DEFROST/ERROR/LOW_TEMP |
| Heat runtime | `sensor` | `duration` | Unit: `min` |
| Input pins (Y, O, LPS, DFT) | `binary_sensor` | — | Read-only thermostat/switch signals |
| Output relays (FAN, CNT, W, RV, AUX) | `binary_sensor` | `running` | NOT `switch` — outputs are state-machine controlled |
| Fault flags | `binary_sensor` | `problem` | lpsFault, compressorOverTemp, suctionLowTemp, rvFail, highSuctionTemp |
| Protection states | `binary_sensor` | — | lowTemp, defrost, manualOverride, stateValidating |

## Files

| File | Description |
|------|-------------|
| `discovery.py` | MQTT auto-discovery script (approach 1) |
| `configuration.yaml` | Manual YAML config (approach 2) |
| `README.md` | This file |
