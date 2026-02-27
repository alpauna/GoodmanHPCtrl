# Home Assistant Integration Options for GoodmanHPCtrl

Research document exploring approaches to deeply integrate the ESP32 HVAC controller with Home Assistant beyond basic MQTT sensors.

## Current State

The device publishes MQTT telemetry on 3 topics (`{prefix}/temps`, `{prefix}/state`, `{prefix}/fault`). A `discovery.py` script and `configuration.yaml` are available for basic entity creation. The device is publish-only — no MQTT command subscriptions.

---

## 1. MQTT Auto-Discovery from Firmware (Recommended First Step)

### How It Works

The ESP32 publishes retained JSON config messages to `homeassistant/<component>/<node_id>/<object_id>/config` on MQTT connect. HA's MQTT integration automatically creates all entities grouped under a single device. LWT (Last Will and Testament) provides availability tracking.

### Implementation

Changes to `MQTTHandler.cpp`:

1. **LWT**: Call `_client.setWill("goodman/availability", 1, true, "offline")` in `begin()` before connect
2. **Availability**: Publish `online` (retained) to `goodman/availability` in `onConnect()`
3. **Discovery**: New `publishDiscovery()` method publishes ~25 retained config messages for all entities
4. **HA birth**: Subscribe to `homeassistant/status` and re-publish discovery when HA restarts
5. **Config toggle**: `heatpump.haDiscovery.enabled` in config JSON, checkbox on config page

### Discovery Payload Format

Per-entity (traditional):
```
homeassistant/sensor/goodman/compressor_temp/config  (retained)
```
```json
{
  "name": "Compressor Temperature",
  "unique_id": "goodman_compressor_temp",
  "state_topic": "goodman/temps",
  "value_template": "{{ value_json.COMPRESSOR_TEMP | default('') }}",
  "unit_of_measurement": "°F",
  "device_class": "temperature",
  "state_class": "measurement",
  "availability_topic": "goodman/availability",
  "device": {
    "identifiers": ["goodman_hp_ctrl"],
    "name": "Goodman Heat Pump",
    "manufacturer": "Custom",
    "model": "ESP32-S3 HVAC Controller",
    "sw_version": "1.2",
    "configuration_url": "http://192.168.1.138"
  }
}
```

Consolidated device discovery (HA 2024.1+) publishes a single message to `homeassistant/device/goodman/config` with all components in a `cmps` block using abbreviated keys (`stat_t`, `val_tpl`, `dev_cla`, etc.) to reduce payload size.

### Complexity: Low-Medium (3-5 days)

### Pros
- Zero-config in HA — entities auto-appear under a single device
- Leverages existing MQTT infrastructure, no new libraries
- Entities persist across HA restarts (retained messages on broker)
- Availability tracking via LWT
- Industry standard (ESPHome, Tasmota, Zigbee2MQTT all use this)

### Cons
- Requires MQTT broker (already in use)
- Discovery payloads are verbose to build in C++
- Broker losing retained messages requires device reconnect

### Firmware Changes: Yes (moderate, ~150-300 lines in MQTTHandler.cpp)

---

## 2. Custom HACS Dashboard Card

### How It Works

A custom Lovelace card (JavaScript/TypeScript web component using LitElement) published to HACS. The card reads HA entity states and renders a purpose-built HVAC dashboard. It does NOT talk to the ESP32 directly — all data flows through HA entities.

### What the Card Would Display

- **State banner**: Current mode (OFF/COOL/HEAT/DEFROST/ERROR/LOW_TEMP) with color coding
- **Temperature gauges**: 5 sensors with min/max ranges
- **Input indicators**: Y, O, LPS, DFT as colored pills
- **Output relay status**: FAN, CNT, W, RV, AUX as on/off indicators
- **Fault indicators**: Red alert pills for LPS, compressor OT, suction LT, RV fail, high suction
- **Protection states**: Low temp, defrost, manual override, state validating
- **Heat runtime**: Minutes counter with progress toward defrost threshold
- **Transition countdowns**: Defrost phases, cool/heat transition timers

### Card Configuration (in HA Lovelace)
```yaml
type: custom:goodman-heatpump-card
state_entity: sensor.goodman_state
temps:
  compressor: sensor.goodman_compressor_temp
  suction: sensor.goodman_suction_temp
  ambient: sensor.goodman_ambient_temp
  condenser: sensor.goodman_condenser_temp
  liquid: sensor.goodman_liquid_temp
runtime_entity: sensor.goodman_heat_runtime
```

### HACS Repository Structure
```
goodman-heatpump-card/
  hacs.json
  dist/goodman-heatpump-card.js   # Bundled LitElement component
  src/
    goodman-heatpump-card.ts       # Main card source
    styles.ts                      # CSS-in-JS styles
  rollup.config.js
  package.json
```

### Reference Projects
- **[simple-thermostat](https://github.com/nervetattoo/simple-thermostat)** — LitElement, Rollup, 59 releases
- **[lovelace-thermostat-card](https://github.com/fineemb/lovelace-thermostat-card)** — Nest-style SVG arc gauges

### Complexity: Medium-High (2-4 weeks)

### Pros
- Rich, purpose-built visualization tailored to this controller
- Installable by any HA user via HACS with one click
- No firmware changes — reads from HA entities
- Can match the existing dashboard's look and feel

### Cons
- Separate project to maintain alongside firmware
- Requires entities to already exist in HA (depends on approach 1 or YAML)
- HA frontend APIs are poorly documented and change between versions
- Must learn LitElement + Lovelace lifecycle

### Firmware Changes: None (reads HA entity state only)

---

## 3. Direct HA REST API from ESP32

### How It Works

The ESP32 makes HTTP POST requests to HA's REST API using a Long-Lived Access Token to create/update entity states:

```
POST http://<ha-ip>:8123/api/states/sensor.goodman_compressor_temp
Authorization: Bearer <token>
Content-Type: application/json

{"state": "72.5", "attributes": {"friendly_name": "Compressor Temp", "unit_of_measurement": "°F", "device_class": "temperature"}}
```

### Config Page Addition

- **Fieldset: "Home Assistant"**
  - Checkbox: "Enable HA REST API"
  - Text field: "HA URL" (e.g., `http://192.168.1.x:8123`)
  - Text field: "Long-Lived Access Token" (encrypted with `$AES$` scheme, same as WiFi/MQTT passwords)

### Critical Limitation

**Entities vanish on HA restart.** REST-created entities are ephemeral — they only exist while the ESP32 keeps updating them. When HA reboots, all entities disappear until the next device update. There is no entity registration or persistence mechanism via the REST API.

### Available Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/states/<entity_id>` | POST | Create/update entity state |
| `/api/events/<event_type>` | POST | Fire an event |
| `/api/services/<domain>/<service>` | POST | Call a service (e.g., notify) |
| `/api/` | GET | Health check |

### Complexity: Low-Medium (1-2 weeks)

### Pros
- No MQTT broker dependency — direct device-to-HA
- Conceptually simple (HTTP POST + JSON)
- Token auth is straightforward
- Could push notifications on faults via `/api/services/notify/...`

### Cons
- **Entities vanish on HA restart** — fatal for sensor data
- No device grouping in HA device registry
- No availability/offline detection
- Heavy on ESP32 resources (25+ HTTP requests vs 2-3 MQTT publishes)
- Each request is ~50-200ms on LAN; 25 entities = 1-5 seconds per update cycle
- HTTPS overhead is expensive on ESP32

### Firmware Changes: Yes (new HTTPClient code, config fields, ~200-400 lines)

---

## 4. HA WebSocket API from ESP32

### How It Works

The ESP32 opens a persistent WebSocket to `ws://<ha-ip>:8123/api/websocket`, authenticates with a long-lived access token, and can fire events, call services, and subscribe to HA state changes.

### Authentication Flow
1. Server sends `{"type": "auth_required"}`
2. Client sends `{"type": "auth", "access_token": "<token>"}`
3. Server sends `{"type": "auth_ok"}`

### Critical Limitation

**Cannot create or update entity states via WebSocket.** The WS API supports `fire_event` and `call_service` but has no `POST /api/states` equivalent. You cannot create sensors or binary_sensors through the WebSocket API alone.

### What It IS Good For (Future)
- Subscribing to HA events (e.g., thermostat setpoint changes)
- Calling HA services (e.g., push notifications on faults)
- Bidirectional communication from HA to the ESP32
- Real-time event stream

### Complexity: Medium-High (2-3 weeks)

### Pros
- Persistent connection, real-time bidirectional
- Could enable HA-to-device commands in the future

### Cons
- **Cannot create entities** — fatal for sensor data without a companion approach
- Additional library dependency and memory pressure
- Complex reconnection/error handling
- Device already runs a WS server; adding a client increases complexity

### Firmware Changes: Yes (significant, ~500+ lines, new WS client library)

---

## 5. Custom Integration (`custom_components/`)

### How It Works

A Python package at `config/custom_components/goodman_heatpump/` that subscribes to MQTT topics programmatically and creates entities with richer behavior than YAML templates. Installable via HACS.

### Structure
```
custom_components/goodman_heatpump/
  __init__.py
  manifest.json       # domain, name, version, dependencies: ["mqtt"]
  sensor.py           # Temperature + state + runtime entities
  binary_sensor.py    # Inputs, outputs, faults, flags
  config_flow.py      # UI-based configuration (Settings → Integrations → Add)
```

### Complexity: High (2-3 weeks)

### Pros
- Richest integration — custom services, diagnostics, config flow UI
- Could expose `goodman_heatpump.clear_rv_fail` as an HA service
- Distributable via HACS

### Cons
- Overkill for read-only telemetry
- HA integration API evolves with breaking changes
- Requires Python + understanding of HA async architecture
- Must test across HA versions

### Firmware Changes: None (subscribes to existing MQTT topics)

---

## Comparison

| Approach | Complexity | Entities Persist | Device Grouping | Availability | Firmware Changes |
|----------|:----------:|:----------------:|:---------------:|:------------:|:----------------:|
| **MQTT Discovery** | Low-Med | Yes | Yes | Yes (LWT) | Moderate |
| **HACS Card** | Med-High | N/A (reads entities) | N/A | N/A | None |
| REST API | Low-Med | **No** | **No** | **No** | Moderate |
| WebSocket API | Med-High | **Cannot create** | N/A | N/A | Significant |
| Custom Integration | High | Yes | Yes | Custom | None |

---

## Recommended Phased Approach

### Phase 1: MQTT Auto-Discovery (3-5 days)

Add `publishDiscovery()` to `MQTTHandler.cpp`. Delivers 90% of the value with minimal effort. Config page checkbox to enable/disable. No HA access token needed.

**Result:** All 25+ entities auto-appear in HA under a single device with availability tracking.

### Phase 2: HACS Dashboard Card (2-4 weeks)

Build a custom Lovelace card that reads the MQTT-discovered entities and renders a purpose-built HVAC dashboard matching the existing web UI's style.

**Result:** One-click installable card from HACS with state banner, temps, I/O grid, faults.

### Phase 3: HA Access Token + REST API Services (1-2 weeks, optional)

Add config page field for HA Long-Lived Access Token (encrypted with `$AES$`). Use it to push HA notifications on faults (`POST /api/services/notify/...`) and optionally read HA state. Not for entity creation — that stays on MQTT.

**Result:** Device can trigger HA automations and notifications directly on fault events.

### Config Page UI for All Phases

```
┌─ Home Assistant ──────────────────────────────────┐
│ ☑ Enable MQTT Auto-Discovery                      │
│   Discovery Prefix: [homeassistant          ]     │
│                                                    │
│ ☐ Enable HA API (optional)                        │
│   HA URL:          [http://192.168.1.x:8123 ]     │
│   Access Token:    [••••••••••••••••••••••   ]     │
└───────────────────────────────────────────────────┘
```

Config JSON:
```json
{
  "homeAssistant": {
    "discoveryEnabled": true,
    "discoveryPrefix": "homeassistant",
    "accessToken": "$AES$...",
    "apiUrl": "http://192.168.1.x:8123"
  }
}
```
