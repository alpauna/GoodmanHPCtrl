# GoodmanHPCtrl

ESP32-based controller for Goodman heatpumps with support for cooling, heating, and defrost modes.

## Web Interface

| Page | Screenshot |
|------|-----------|
| Home | ![Home](docs/screenshots/home.png?v=5) |
| Dashboard | ![Dashboard](docs/screenshots/dashboard.png?v=7) |
| Pins | ![Pins](docs/screenshots/pins.png?v=2) |
| Configuration | ![Configuration](docs/screenshots/config.png?v=5) |
| OTA Update | ![OTA Update](docs/screenshots/update.png?v=4) |
| Log | ![Log](docs/screenshots/log.png?v=1) |
| Heap | ![Heap](docs/screenshots/heap.png?v=1) |

## Features

- **Relay control** — 4 output pins (FAN, Contactor, W-Heat, Reversing Valve) driven by 4 input signals (Low Pressure Switch, Defrost, Y-Cool, O-Heat)
- **Temperature monitoring** — 4 OneWire (Dallas DS18B20) sensors (compressor, suction, ambient, condenser) + liquid line thermocouple with auto-detection priority: MAX6675 SPI > MAX31850K OneWire > MCP9600 I2C
- **Remote access** — REST API, WebSocket, and MQTT for monitoring and control
- **HTTPS/SSL** — Self-signed ECC P-256 certificate on port 443 for secure `/config`, `/update`, and `/ftp` endpoints. Graceful fallback to HTTP-only if no certs found on SD card
- **Dark/light theme** — Configurable dark/light theme with shared `theme.css` stylesheet. Persisted to SD card config, cached in localStorage for flash-free page loads. Instant preview on config page
- **Admin password protection** — HTTP Basic Auth on sensitive endpoints (`/config`, `/update`, `/ftp`). No password = open access
- **Password encryption** — All passwords (WiFi, MQTT, admin, FTP) encrypted at rest on SD card
- **Live dashboard** — Real-time dashboard at `/dashboard` with state banner, protection status pills with dynamic labels (LPS No Fault/Fault, Temp Ok/Low Temp, Comp Ok/OT, Suct LT Ok/LT, Suct Temp/High Suct with live temp reading, RV Pass/Fail with inline clear button), input/output grid, temperatures, and reboot button. Output indicators include inline status pills:
  - ![SC](docs/screenshots/pill-sc.png) **SC** (Short Cycle) — Shown on CNT output. Green when inactive, red when CNT short cycle protection delay is active (CNT was off < 5 min, waiting 30s before reactivation)
  - ![DFH](docs/screenshots/pill-dfh.png) **DFH** (Defrost Hold) — Shown on CNT, W, and RV outputs during the 3-phase defrost entry and exit transitions. On RV and W: red during entry Phase 1 or exit Phase 1 (pressure equalization). On CNT: red during entry Phase 2 or exit Phase 2 (waiting for CNT short cycle before compressor starts)
- **Pin table with manual override** — Auth-protected `/pins` page showing all GPIO inputs, outputs, and temperatures in a table. "Normal Mode Lockout" checkbox enables manual output control, bypassing the state machine for up to 30 minutes (auto-timeout). CNT enforces short cycle protection even in manual mode. Single auth prompt covers the entire lockout session. "Force Defrost" button triggers a software defrost cycle from HEAT mode (requires no active faults or manual override)
- **Temperature history** — Configurable CSV logging interval (30s-5min, default 2min) per sensor to SD card (`/temps/<sensor>/YYYY-MM-DD.csv`), rolling Canvas line charts on dashboard with 1h/6h/24h/7d timeframe selector, auto-purge after 31 days
- **Web-based configuration** — HTML pages served from `/www/` on SD card for configuration, OTA updates, and monitoring
- **FTP server** — SimpleFTPServer with timed enable/disable (10/30/60 min) from config page. Configurable password (encrypted on SD card); defaults to `admin`/`admin` when not set. Defaults to OFF; auto-disables after timeout
- **OTA updates** — Firmware upload saves to SD card (`/firmware.new`), then apply to flash. Automatically backs up running firmware with build date metadata before flashing. Supports manual revert and automatic crash recovery (see [Crash Recovery](#crash-recovery))
- **SD card configuration** — WiFi, MQTT, and sensor settings stored as JSON on SD card
- **Multi-output logging** — Serial, MQTT, SD card with tar.gz compressed log rotation, and WebSocket streaming
- **In-memory log buffer** — 500-entry ring buffer in PSRAM, accessible via `/log` API endpoint
- **NTP time sync** — Automatic time synchronization from NTP servers, refreshes every 2 hours
- **I2C bus** — Initialized on GPIO8 (SDA) / GPIO9 (SCL) with automatic device scan at startup and `/i2c/scan` API endpoint
- **PSRAM support** — All heap allocations routed through PSRAM when available
- **WiFi AP fallback** — Configurable timeout (default 10 minutes) before switching to Access Point mode for OTA recovery and reconfiguration
- **Multi-unit support** — Configurable system name (max 20 chars, alphanumeric + spaces) and MQTT topic prefix. System name is used as the web UI brand, AP SSID, OLED display name, HTTPS auth realm, and SSL certificate CN. Multiple controllers can publish to the same MQTT broker with unique prefixes (e.g., `unit1/temps`, `unit2/temps`)
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
  - **W** (auxiliary heat): ON in DEFROST, ERROR (HEAT mode only), LOW_TEMP (HEAT mode only), and RV_FAIL (HEAT mode only); OFF otherwise. In COOL mode (Y+O), the system will not operate below 20°F
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

- **Automatic Defrost (3-Phase Sequencing)** — After a configurable heat runtime threshold (default 90 minutes, range 30–90 min) of accumulated CNT runtime in HEAT mode while DFT is active (coil temp ≤ 32°F, indicating icing conditions), initiates a 3-phase software defrost cycle for safe pressure equalization and output sequencing:

  **Phase 1 — Pressure Equalization** (`defrostTransition`):
  - All outputs OFF (CNT, FAN, W, RV)
  - Duration: `heatpump.shortCycle.rv` (default 30s)
  - Allows system pressures to equalize before reversing valve switches

  **Phase 2 — RV + W Engaged, CNT Hold** (`defrostCntPending`):
  - RV and W turned ON, CNT remains OFF
  - Duration: `heatpump.shortCycle.cnt` (default 30s)
  - Allows reversing valve to fully seat before compressor starts

  **Phase 3 — Active Defrost**:
  - CNT turned ON, defrost fully active
  - Runs for at least `heatpump.defrost.minRuntimeMs` (default 3 min) before checking exit conditions
  - Rechecks CONDENSER_TEMP every 1 minute during defrost with logging
  - Exits when CONDENSER_TEMP >= `heatpump.defrost.exitTempF` (default 60°F) or 15-minute safety timeout

- **Defrost Exit Transition (3-Phase)** — When defrost completes (condenser temp reached or timeout), the system performs a reverse 3-phase sequence to safely switch the reversing valve back to heat position before restarting the compressor:

  **Exit Phase 1 — Pressure Equalization** (`defrostTransition` + `defrostExiting`):
  - CNT and FAN turned OFF, RV and W stay ON
  - Duration: `heatpump.shortCycle.rv` (default 30s)
  - Allows system pressures to equalize after compressor stops

  **Exit Phase 2 — RV Switch + CNT Hold** (`defrostCntPending` + `defrostExiting`):
  - RV and W turned OFF (RV switches back to heat position), CNT remains OFF
  - Duration: `heatpump.shortCycle.cnt` (default 30s)
  - Allows reversing valve to physically seat before compressor restarts

  **Exit Complete**:
  - CNT and FAN turned ON — system resumes normal HEAT mode operation
  - State machine has already transitioned from DEFROST to HEAT at exit start

  **Additional defrost behavior:**
  - Heat runtime only accumulates when DFT input is active (closed at 32°F, indicating icing conditions)
  - DFT turning off (temps > 32°F) clears accumulated runtime — no ice risk
  - Only COOL and DEFROST modes clear accumulated runtime; Y going off does not
  - Only HEAT mode adds time to accumulated runtime
  - Runtime persists to SD card every 5 minutes, restored on boot
  - **Y drop during defrost entry**: All outputs turn off (including W), transition flags are cleared, but `_softwareDefrost` stays set. When Y reactivates in HEAT mode, defrost restarts from Phase 1
  - **Y drop during defrost exit**: All outputs turn off, exit transition cancelled. Normal state machine resumes on Y reactivation
  - **COOL cancellation**: If the thermostat switches to COOL mode (O becomes active) during any phase of a pending defrost, the defrost is cancelled entirely, heat runtime is cleared, and the system enters normal COOL mode

### State Table

| State | Condition | FAN | CNT | RV | W | Notes |
|-------|-----------|-----|-----|----|---|-------|
| `OFF` | Y inactive | OFF | OFF | OFF | OFF | All outputs off |
| `HEAT` | Y active, O inactive | ON | ON | OFF | OFF | CNT has 30s short-cycle protection |
| `COOL` | Y active, O active | ON | ON | ON | OFF | Will not operate below 20°F |
| `DEFROST` Phase 1 | Pressure equalization (30s) | OFF | OFF | OFF | OFF | All outputs off for pressure equalization |
| `DEFROST` Phase 2 | RV + W engaged, CNT hold (30s) | OFF | OFF | ON | ON | Reversing valve seats before compressor starts |
| `DEFROST` Phase 3 | Active defrost | OFF | ON | ON | ON | 3-min minimum, exits at condenser ≥ 60°F or 15-min timeout |
| `HEAT` Exit Phase 1 | Defrost exit pressure equalization (30s) | OFF | OFF | ON | ON | RV+W stay on from defrost, CNT+FAN off |
| `HEAT` Exit Phase 2 | RV switch + CNT hold (30s) | OFF | OFF | OFF | OFF | RV switches back to heat position |
| `HEAT` Exit Complete | CNT+FAN resume | ON | ON | OFF | OFF | Normal HEAT mode resumes |
| `ERROR` | LPS fault (low pressure) | OFF | OFF | OFF | ON* | *W on only in HEAT mode (Y active, O inactive) |
| `LOW_TEMP` | Ambient < 20°F | OFF | OFF | OFF | ON* | *W on only in HEAT mode; W turns off if thermostat switches to COOL (Y+O) |
| `HEAT` + RV Fail | RV fail latched | ON | OFF | OFF | ON* | *W on only in HEAT mode; CNT blocked until cleared |

**Fault Conditions** (overlay on current state, block CNT activation):

| Fault | Condition | Trigger | Recovery | FAN | CNT | W | Priority |
|-------|-----------|---------|----------|-----|-----|---|----------|
| Compressor overtemp | COMPRESSOR_TEMP ≥ 240°F | Any mode | Temp < 190°F | ON | OFF | OFF | 1 (highest) |
| Suction low temp | SUCTION_TEMP < 32°F | COOL mode only | Temp > 40°F | ON | OFF | OFF | 2 |
| LPS fault | LPS input LOW | Any mode | LPS goes HIGH | OFF | OFF | ON* | 3 |
| Low ambient temp | AMBIENT_TEMP < 20°F | Any mode | Temp ≥ 20°F | OFF | OFF | ON* | 4 |
| RV fail | SUCTION_TEMP ≥ 140°F during defrost | Defrost only | Manual clear via dashboard/config | ON | OFF | ON* | Latched |

\* W on only in HEAT mode (Y active, O inactive); never activated in COOL mode (Y+O).

**Fault Priority:** Compressor overtemp > Suction low temp > LPS fault > Low ambient temp. Higher-priority faults take precedence; lower-priority checks are skipped while a higher-priority fault is active.

### Class Structure

| Class | Purpose |
|-------|---------|
| `GoodmanHP` | Central controller with pin maps, temp sensors, and state machine |
| `InputPin` | Digital/analog input with ISR, debouncing, callbacks |
| `OutPin` | Output relay with delay, PWM support, state tracking, hardware state validation |
| `TempSensor` | Temperature sensor with callbacks; supports OneWire (DS18B20), I2C (MCP9600), and SPI (MAX6675) |
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
| MAX6675 CLK | 39 | Output | SPI thermocouple clock (software SPI) |
| MAX6675 CS | 40 | Output | SPI thermocouple chip select |
| MAX6675 DO | 41 | Input | SPI thermocouple data out |

**I2C Devices:**

| Device | Address | Description |
|--------|---------|-------------|
| MCP9600 | 0x67 | Type-K thermocouple amplifier (LIQUID_TEMP) |
| SSD1306 | 0x3C | 128x64 OLED display (5-page auto-cycling status) |

**SPI Devices (software SPI):**

| Device | Description |
|--------|-------------|
| MAX6675 | Type-K thermocouple reader for LIQUID_TEMP (highest priority in auto-detection) |

**MAX6675 Thermocouple Board Mounting:**

The MAX6675 breakout board (32mm x 16mm) mounts to the inner ceiling of the top shell enclosure via a single M3 screw pillar. The terminal block protrudes through a rectangular cutout in the top shell.

- **Mounting pillar**: 6mm OD, 6mm tall, M3 tap hole (2.5mm), attached to inner ceiling
- **Pillar position**: 35mm from left outer edge, 28mm from top outer edge, offset 2mm toward bottom of board from center
- **Terminal cutout**: 16mm x 14mm rectangular hole through the top shell at the top end of the board
- **Countersink**: Shell thinned from 3mm to 1.5mm around the terminal opening, extending to the outer top edge for terminal block clearance
- **Default SPI pins**: CLK=GPIO 39 (TCK), CS=GPIO 40 (TDO), DO=GPIO 41 (TDI) — freed JTAG header pins on ESP32-S3
- **3D shell files**: Located in `shell/` — originals (3mm walls) and modified versions (3.5mm walls, flipped mating profile). See [3D Printed Shell](#3d-printed-shell) section below

### 3D Printed Shell

The enclosure is an EasyEDA-exported 3D shell, post-processed with FreeCAD Python scripts for thicker walls, a flipped mating profile, and mounting hardware. All files are in the `shell/` directory.

**Shell files:**

| File | Description |
|------|-------------|
| `3DShell_GoodmanHPv3_T_3mm_TC.step/.stl` | Original top shell (3mm walls, tongue on outer half, TC mount) |
| `3DShell_GoodmanHPv3_B_3mm.step/.stl` | Original bottom shell (3mm walls, groove on outer half) |
| `3DShell_GoodmanHPv3_T_3.5mm_TC_flipped.step/.stl` | Modified top shell (3.5mm walls, groove replaces tongue) |
| `3DShell_GoodmanHPv3_B_3.5mm_flipped.step/.stl` | Modified bottom shell (3.5mm walls, tongue replaces groove) |
| `modify_top.py` | FreeCAD script to generate the modified top shell |
| `modify_bottom.py` | FreeCAD script to generate the modified bottom shell |
| `cutout.py` | Reusable FreeCAD utility library for cutout, standoff, and mounting operations |

**Why the mating profile was flipped:** The original design had a tongue (raised band) on the top shell mating rim. When 3D printed, this thin horizontal band is prone to layer separation because it prints as an unsupported overhang at the shell's open edge. Moving the tongue to the bottom shell and the groove to the top eliminates this — the bottom tongue prints vertically up from the floor (strong layer adhesion), and the top groove is simply a recess in a solid wall.

**Modifications applied:**

1. **Wall thickening (3mm to 3.5mm)** — 0.5mm added to the outer face of all 4 walls. Inner cavity dimensions unchanged so the PCB still fits. Wall cutouts (connector openings on right and back walls) are preserved by pre-cutting matching holes in the thickening ring before fusing.

2. **Mating profile flip** — The rabbet/step joint between shells was inverted:
   - **Bottom**: Original groove (Z=31-35) filled, new outer tongue added with 0.10mm clearance per side
   - **Top**: Original tongue (Z=28-32) removed, new inner lip + outer groove cut

3. **M3 screw pillar** — 5mm OD, 3mm tall pillar on the top shell ceiling for mounting the MAX6675 thermocouple board, with M3 (2.5mm) through-hole centered in the pillar

**Utility library (`cutout.py`):**

Composable functions for shell modifications. Each takes a `Part.Shape` and returns a modified shape. OCCT boolean operation quirks (oversized plugs, Z range matching, flush trimming) are handled internally:

| Function | Purpose |
|----------|---------|
| `rectangular_cutout()` | Cut a window through a Z-normal surface |
| `reposition_cutout()` | Plug an existing cutout and re-cut at a new position (3-step: oversized plug, matching-Z cut, flush trim) |
| `add_standoff()` | Fuse a cylindrical pillar onto a surface (with auto-trim for reliable OCCT fuse) |
| `drill_hole()` | Vertical through-hole along Z axis |
| `drill_hole_lateral()` | Horizontal through-hole along X or Y axis (screw holes through walls) |
| `add_standoffs_with_holes()` | Multiple standoffs + centered screw holes (convenience combo) |
| `wall_ring()` | Rectangular ring (outer box minus inner box) for wall thickening and mating profiles |
| `z_probe()` | Probe Z intersections at an XY point (verification) |
| `verify_cutout()` | Check a rectangular cutout is fully open (probes center + corners) |
| `verify_solid()` | Check material exists at a point between two Z levels |

Example usage:
```python
from cutout import reposition_cutout, add_standoff, drill_hole

# Reposition an LCD cutout (plug old hole, cut new, trim flush — all in one call)
result = reposition_cutout(result,
    old_x1=-38.48, old_y1=59.59, old_x2=-11.43, old_y2=76.59,
    new_x1=-38.48, new_y1=61.59, new_x2=-11.43, new_y2=77.59,
    z_inner=48.50, z_outer=51.50)

# Add a mounting standoff below the ceiling
result = add_standoff(result, cx=-63.76, cy=67.14,
    z_surface=48.50, height=3.0, radius=2.5, z_outer=51.50)

# Drill a screw hole through the standoff
result = drill_hole(result, cx=-63.76, cy=67.14,
    z_bottom=45.50, z_top=51.50, radius=1.25)
```

**Regenerating modified shells:**

```bash
# Requires FreeCAD installed (sudo apt install freecad)
freecadcmd shell/modify_top.py
freecadcmd shell/modify_bottom.py
```

**Key geometry constants (all in mm, origin at EasyEDA export origin):**

| Constant | Value | Description |
|----------|-------|-------------|
| Outer walls | X[-98.26, 4.92] Y[-32.48, 97.12] | Original 3mm outer face |
| Inner walls | X[-95.26, 1.92] Y[-29.48, 94.12] | Original 3mm inner face |
| New outer | X[-98.76, 5.42] Y[-32.98, 97.62] | After 0.5mm thickening |
| Bottom mating zone | Z=31 to Z=35 | Groove (original) / Tongue (modified) |
| Top mating zone | Z=28 to Z=32 | Tongue (original) / Groove (modified) |
| Right wall cutout | Y[47, 88.5] Z[7, 24] | Connector opening, +X wall |
| Back wall lower cutout | X[-50, -35] Z[6, 13] | Opening, +Y wall |
| Back wall upper cutout | X[-89.5, -3.5] Z[14, 17] | Opening, +Y wall |

### Modifying EasyEDA 3D Shell Exports with Claude Code

EasyEDA exports PCB enclosures as STEP files with thin walls (typically 1.5mm). These often need post-processing — thicker walls, mating features, mounting hardware, cutouts — which can be done programmatically via FreeCAD's Python API and Claude Code. Below is a reusable prompt template and the key lessons learned from this project's shell modifications.

**Prerequisites:** `freecad` package installed (`sudo apt install freecad`), STEP files exported from EasyEDA's 3D shell generator.

**Prompt template** — adapt the bracketed sections to your project:

> I have EasyEDA-exported STEP enclosure shells at `[path/to/shells/]`:
> - `[Bottom_Shell.step]` — bottom half, open top
> - `[Top_Shell.step]` — top half, open bottom
>
> The shells have [1.5mm] walls. I need the following modifications:
>
> 1. **Wall thickening** — Increase wall thickness from [1.5mm] to [3mm] by expanding outward. Keep inner cavity dimensions identical to the original so the PCB still fits. Preserve all interior features (screw pillars, standoffs, alignment posts) from the original STEP by extracting them via boolean intersection and fusing them back into the thickened shell.
>
> 2. **Mating joint** — Add a rabbet/step joint between top and bottom shells in the overlap zone where both rims meet:
>    - Bottom: remove outer half of wall for [4mm] from the rim (groove)
>    - Top: remove inner half of wall in the overlap zone + add outer-half tongue extending [4mm] below the rim with [0.2mm] clearance
>    - The tongue must overlap at least [2mm] into existing wall geometry above the rim for OCCT fuse to succeed (coincident-face workaround)
>
> 3. **Screw holes** — Drill [M1.2] through-holes at [left and right wall midpoints], centered in the interlocking overlap zone so the screw passes through both the tongue and groove lip
>
> 4. **Cutouts** — Cut rectangular openings in the top shell for: [LCD window at X1,Y1 to X2,Y2], [wire pass-through at ...], [connector at ...]
>
> 5. **Mounting features** — Add a screw pillar on the inner ceiling of the top shell at [position] for mounting a [component]. [Xmm] OD, [Xmm] tall, [MX] tap hole
>
> Write a FreeCAD Python script (`freecadcmd script.py`) that loads the STEP files, performs all modifications via boolean operations, and exports both STL and STEP. Include a verification section that cross-sections the results and prints geometry at key Z levels.

**Key lessons for FreeCAD/OCCT boolean operations:**

| Issue | Symptom | Fix |
|-------|---------|-----|
| Coincident face fuse failure | `fuse()` adds volume but geometry doesn't extend beyond shared face | Extend the new body 1-2mm past the shared face into existing solid so they overlap, not just touch |
| `makeOffsetShape` fails on complex shells | Exception or degenerate result | Rebuild parametrically: create outer box, cut inner cavity, extract+fuse interior features from original via `common()` |
| Thickening shifts features | Mounting tabs, screw pillars stay at original positions | After thickening, extract features by region (`common()` with probe box), `translated()` to correct position, cut old + fuse shifted |
| Groove/tongue on wrong wall half | Joint doesn't interlock — both pieces occupy same space | Groove removes **outer** half; tongue occupies **outer** half (filling the groove space). Cut **inner** half of mating shell in overlap zone so it nests with the other shell's remaining inner lip |
| `removeSplitter()` drops geometry | Thin or newly-fused features disappear | Check volume before/after; if features vanish, skip `removeSplitter()` or increase overlap |
| STEP export loses thin features | Feature exists in memory (volume correct) but missing after reload | Ensure minimum 0.5mm thickness on all features; export STL as backup (mesh-based, more tolerant) |

**Verification pattern** — always include in your script:
```python
# Cross-section at key Z levels to verify wall profiles
for z in [key_z_values]:
    plane = Part.makePlane(250, 200, Vector(-150, -100, z), Vector(0, 0, 1))
    sec = shape.section(plane)
    sbb = sec.BoundBox
    print(f"Z={z}: X[{sbb.XMin:.2f},{sbb.XMax:.2f}] Y[{sbb.YMin:.2f},{sbb.YMax:.2f}]")
```

**Typical workflow:**
1. **Analyze** — Load STEP, print bounding boxes, cross-section at rim levels to get exact wall coordinates (outer, inner, midpoint)
2. **Thicken** — Parametric rebuild with `makeBox` + `cut` (more reliable than `makeOffsetShape` on EasyEDA exports)
3. **Interior features** — `original.common(cavity_box)` extracts pillars/standoffs, `fuse()` them into new shell
4. **Mating edge** — Groove (cut outer half) + tongue (fuse outer half extension with 2mm overlap into wall) + step (cut inner half in overlap zone)
5. **Hardware** — Screw holes (`makeCylinder` + `cut`), mounting pillars (`makeCylinder` + `fuse`), cutouts (`makeBox` + `cut`)
6. **Export** — `.exportStep()` for CAD, `MeshPart.meshFromShape()` + `.write()` for STL printing

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE extension)
- USB cable connected to ESP32 board

**Linux/macOS:** Shell scripts in `scripts/` (`.sh`) require `bash` and `curl`.

**Windows:** PowerShell scripts in `scripts/` (`.ps1`) require PowerShell 5.1+ (included with Windows 10/11) and `curl.exe` (included with Windows 10+). Some scripts also require `openssl` (included with [Git for Windows](https://gitforwindows.org/)). Run with: `powershell -ExecutionPolicy Bypass -File .\scripts\<script>.ps1`

### Secrets Setup

Create `secrets.ini` in the project root (gitignored — never committed):

```ini
[secrets]
build_flags =
	-D XOR_KEY=\"your-random-base64-key\"
```

`XOR_KEY` is used as a fallback key for password encryption on the SD card when no eFuse HMAC key is provisioned (see [Password Encryption](#password-encryption)). Generate a random key with: `openssl rand -base64 32`

**AP mode password** is configured from the config page (`wifi.apPassword` in config JSON) or auto-generated randomly at runtime if not set. It is no longer a build-time constant.

### Build and Upload

PlatformIO commands work the same on all platforms:

```bash
# Build
pio run -e freenove_esp32_s3_wroom

# Upload firmware
pio run -t upload -e freenove_esp32_s3_wroom

# Serial monitor
pio run -t monitor -e freenove_esp32_s3_wroom
```

On Linux, if `pio` is not on PATH, use `~/.platformio/penv/bin/pio`. On Windows, use `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

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
# Linux/macOS
./scripts/configure.sh --local

# Windows (PowerShell)
.\scripts\configure.ps1 -Local
```

This prompts for system name, MQTT prefix, WiFi, and MQTT credentials, then writes `data/config.txt`. Copy it to the SD card root. Passwords are stored in plaintext and encrypted automatically on first boot.

**Manual config.txt format:**

```json
{
  "project": "Goodman",
  "created": "Feb 06 2026",
  "description": "Goodman heatpump controller",
  "system": {
    "name": "Goodman HP",
    "mqttPrefix": "goodman"
  },
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
    "shortCycle": { "rv": 30000, "cnt": 30000 },
    "defrost": { "minRuntimeMs": 180000, "exitTempF": 60.0, "heatRuntimeThresholdMs": 5400000 }
  },
  "tempHistory": {
    "intervalSec": 120
  },
  "ui": {
    "theme": "dark"
  },
  "admin": {
    "password": "",
    "ftpPassword": ""
  },
  "sensors": {
    "temp": {
      "288514B2000000EA": { "description": "AMBIENT_TEMP", "name": "AMBIENT_TEMP" },
      "28C7E8B200000076": { "description": "CONDENSER_TEMP", "name": "CONDENSER_TEMP" },
      "28DCC0B200000013": { "description": "COMPRESSOR_TEMP", "name": "COMPRESSOR_TEMP" },
      "2862D5B2000000A9": { "description": "SUCTION_TEMP", "name": "SUCTION_TEMP" }
    },
    "max6675": { "clk": 39, "cs": 40, "do": 41, "enabled": true }
  }
}
```

**Configuration options:**
- `system.name` — System display name, max 20 characters, alphanumeric + spaces (default: "Goodman HP"). Used as web UI brand, AP SSID, OLED display name, HTTPS auth realm, and SSL certificate CN. Requires reboot
- `system.mqttPrefix` — MQTT topic prefix (default: "goodman"). Topics become `<prefix>/temps`, `<prefix>/state`, `<prefix>/fault`, `<prefix>/log`. Requires reboot
- `logging.maxLogSize` — Maximum log file size in bytes before rotation (default: 52428800 = 50MB)
- `logging.maxOldLogCount` — Number of rotated log files to keep (default: 10)
- `tempHistory.intervalSec` — Temperature history capture interval in seconds, 30-300 (default: 120)
- `heatpump.lowTemp.threshold` — Ambient temp (°F) below which compressor is blocked (default: 20.0)
- `heatpump.highSuctionTemp.threshold` — Suction temp (°F) above which RV fail is detected during defrost (default: 140.0)
- `heatpump.shortCycle.rv` — Phase 1 pressure equalization delay in ms (default: 30000)
- `heatpump.shortCycle.cnt` — Phase 2 CNT short cycle delay in ms (default: 30000)
- `heatpump.defrost.minRuntimeMs` — Minimum Phase 3 runtime in ms before checking exit conditions (default: 180000 = 3 min)
- `heatpump.defrost.exitTempF` — Condenser temp (°F) at which Phase 3 exits (default: 60.0)
- `heatpump.defrost.heatRuntimeThresholdMs` — Accumulated HEAT runtime in ms before triggering defrost (default: 5400000 = 90 min, range: 30–90 min via config page)
- `sensors.max6675.clk` — MAX6675 SPI clock pin (default: 39, requires reboot)
- `sensors.max6675.cs` — MAX6675 SPI chip select pin (default: 40, requires reboot)
- `sensors.max6675.do` — MAX6675 SPI data out pin (default: 41, requires reboot)
- `sensors.max6675.enabled` — Enable MAX6675 thermocouple sensor (default: true, requires reboot)

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
# Linux/macOS
./scripts/generate-cert.sh                    # CN=ESP32 (default)
./scripts/generate-cert.sh "My Heatpump"      # CN=My Heatpump

# Windows (PowerShell)
.\scripts\generate-cert.ps1                    # CN=ESP32 (default)
.\scripts\generate-cert.ps1 -Name "My Heatpump"
```

This creates `cert.pem` and `key.pem` (ECC P-256, 10-year validity). The CN (Common Name) defaults to "ESP32" but can be set to match the system name. Copy both files to the SD card root. If no certificates are found, all endpoints fall back to HTTP. The device also auto-generates a self-signed certificate on boot if none are found on the SD card, using the configured system name as the CN.

### Admin Password

Sensitive endpoints (`/config`, `/update`, `/ftp`) are protected by HTTP Basic Auth when an admin password is set.

- **No password set** — All endpoints are open, no authentication required
- **Password set** — Browser prompts for Basic Auth (username: `admin`, password: your admin password)
- Set the admin password from the config page (`/config`) or via the API
- Setting a password automatically disables FTP if it was running

### Password Encryption

All passwords (WiFi, MQTT, admin, FTP) are encrypted at rest on the SD card using **AES-256-GCM** when a hardware HMAC key is provisioned, or XOR obfuscation as a fallback. Plaintext passwords are automatically encrypted on the next config save.

**Encryption tiers:**

| Tier | Method | Key Source | Prefix | Security |
|------|--------|------------|--------|----------|
| 1 (recommended) | AES-256-GCM | eFuse HMAC hardware key | `$AES$` | Key never leaves silicon — only the HMAC peripheral can access it. Cannot be extracted even with physical access to the flash/SD card |
| 2 (fallback) | XOR obfuscation | `XOR_KEY` from `secrets.ini` | `$ENC$` | Symmetric key compiled into firmware — extractable from firmware binary |
| 3 (no key) | Plaintext | None | (none) | No encryption — passwords readable on SD card |

**How it works:** On boot, the firmware calls `esp_hmac_calculate()` with eFuse BLOCK_KEY0 and a salt to derive a 256-bit AES key. The eFuse key is read-protected (`-/-`) — software cannot read it directly; only the ESP32-S3 hardware HMAC peripheral can use it to produce derived keys. Each password is encrypted with a unique random 12-byte IV (nonce), and the GCM authentication tag ensures tampering is detected.

**Provisioning the eFuse HMAC key:**

```bash
# Linux/macOS
./scripts/burn-efuse-key.sh [/dev/ttyUSB0]

# Windows (PowerShell)
.\scripts\burn-efuse-key.ps1 [-Port COM3]
```

This permanently burns a random 256-bit HMAC key to eFuse BLOCK_KEY0 with `HMAC_UP` purpose. The key block becomes read-protected and write-protected immediately after burning.

**WARNING:** eFuse burning is permanent and irreversible. Each key block can only be written once per chip. A backup of the key is saved to `efuse_hmac_key.bin` (gitignored) — store this securely.

**Verified on hardware (2026-02-22):**

```
$ espefuse.py summary (before burn)
BLOCK_KEY0: Purpose=USER, Access=R/W, Data=00 00 00 ... 00

$ espefuse.py burn_key BLOCK_KEY0 efuse_hmac_key.bin HMAC_UP
BURN BLOCK4 - OK
BURN BLOCK0 - OK

$ espefuse.py summary (after burn)
BLOCK_KEY0: Purpose=HMAC_UP, Access=-/-, Data=?? ?? ?? ... ??

Boot log:
  AES-256-GCM encryption enabled (eFuse HMAC key found)
  Admin password encryption: $AES$
  Admin password: set
```

Existing `$ENC$` passwords are automatically re-encrypted as `$AES$` on the next config save. The firmware handles all three tiers transparently — it decrypts any format and re-encrypts with the highest available tier on save.

### FTP Server

FTP (SimpleFTPServer on port 21) is used for uploading HTML files to the SD card's `/www/` directory.

- **FTP defaults to OFF** at boot
- Enabling/disabling FTP requires admin authentication (via `/ftp` API endpoint)
- Enable from the config page with timed durations (10/30/60 min), auto-disables after timeout
- **FTP password** is configurable from the config page. Stored encrypted (`$AES$`/`$ENC$`) in `admin.ftpPassword` in config JSON. When not set, defaults to `admin`. Username is always `admin`
- The `/ftp` GET status endpoint includes the active password in its response, allowing scripts to auto-detect the configured password
- **Max password length: 16 characters** (SimpleFTPServer `FTP_CRED_SIZE` limit)

**Upload web pages via HTTPS:**

```bash
# Linux/macOS
./scripts/update-www.sh                          # Upload all files
./scripts/update-www.sh dashboard.html           # Upload a single file
./scripts/update-www.sh --usb [port]             # Flash via USB serial

# Windows (PowerShell)
.\scripts\update-www.ps1                          # Upload all files
.\scripts\update-www.ps1 -File dashboard.html     # Upload a single file
.\scripts\update-www.ps1 -USB [-Port COM3]        # Flash via USB serial
```

This script prompts for the device IP and admin password, then uploads files from `data/www/` to the device's LittleFS `/www/` directory via HTTPS.

### WiFi AP Fallback

If the device cannot connect to WiFi for a configurable timeout (default 10 minutes), it automatically switches to Access Point (AP) mode for emergency OTA updates and configuration changes.

- **SSID:** Configured system name (default: `Goodman HP`). Set via config page or `system.name` in config JSON
- **Password:** Random 8-character alphanumeric, generated fresh on each AP activation. Displayed on the OLED screen and logged to serial
- **IP:** `192.168.4.1`
- All web endpoints work in AP mode (dashboard, config, OTA update, log, heap)
- HTTPS is not available in AP mode — use HTTP (`http://192.168.4.1`)
- OLED display shows AP credentials (SSID, password, IP) and holds the screen 3x longer for readability
- The device automatically attempts to reconnect to the configured WiFi every 60 seconds while in AP mode
- AP mode ends automatically when WiFi reconnects, or persists until reboot

**Recovery workflow:**
1. Check the OLED display or serial log for the AP SSID and password
2. Connect to the AP WiFi network
3. Browse to `http://192.168.4.1/config` to fix WiFi credentials
4. Or upload new firmware via `http://192.168.4.1/update`
5. The device will reconnect to WiFi automatically when credentials are corrected, or reboot to force reconnection

### Crash Recovery

The device has multiple layers of crash recovery to prevent unrecoverable states in the field.

#### Boot Watchdog / Safe Mode

An `RTC_NOINIT_ATTR` crash counter persists across software resets (cleared only on power-on). Each PANIC, INT_WDT, TASK_WDT, or WDT reset increments the counter. If it reaches 3 consecutive crash boots, the device enters **safe mode** automatically.

**Safe mode behavior:**
- Heat pump controller is fully disabled (no I/O pins, no state machine, no relay control)
- WiFi, web server, configuration, FTP, logging, CPU monitoring, and OLED display remain operational
- The OLED display shows `** SAFE MODE **` on the boot screen and protections page
- `/state` JSON includes `safeMode: true`, `crashBootCount`, and `resetReason`
- A 30-second stable boot timer resets the crash counter, so a single successful boot clears the history

**Forcing safe mode:** `POST /safemode/force` sets a one-shot config flag (`safeMode.force` in config JSON) that triggers safe mode on the next boot. The flag is automatically cleared after entering safe mode so subsequent boots are normal. Useful for maintenance or debugging.

**Clearing safe mode:** `POST /safemode/clear` clears the force flag and reboots.

#### Automatic Firmware Revert

When the device enters safe mode due to a crash boot loop (3+ consecutive crash resets) and a firmware backup exists on SD card (`/firmware.bak`), the boot watchdog automatically attempts to revert to the previous firmware.

**Build date verification:** Each OTA apply operation saves the running firmware's build date to `/firmware.bak.meta` alongside the binary backup. Before reverting, the watchdog compares the backup's build date against the currently running firmware. If they match, the backup is the same build as the crashing firmware — reverting would just re-flash the same broken code. In this case, the revert is skipped and the device continues in safe mode.

| Scenario | Backup exists | Build dates | Action |
|----------|:---:|:---:|--------|
| OTA update caused crash loop | Yes | Different | Auto-reverts to backup, reboots |
| Same firmware crashing | Yes | Same | Skips revert, stays in safe mode |
| No metadata file (legacy backup) | Yes | Unknown | Attempts revert (benefit of the doubt) |
| USB upload caused crash loop | No | N/A | No backup available, stays in safe mode |

**Why OTA is preferred over USB:** OTA updates (`POST /update` + `POST /apply`) automatically back up the running firmware to `/firmware.bak` with build date metadata before flashing. If the new firmware causes a crash loop, the boot watchdog can revert automatically. USB flashing (`pio run -t upload`) bypasses this entirely — no backup is created, so auto-revert is not possible. Always prefer OTA for production deployments.

#### Reboot Rate Limiting

To prevent denial-of-service via the `/reboot` endpoint, the device tracks rapid software reboots using an `RTC_NOINIT_ATTR` counter (separate from the crash counter). After 3 rapid reboots within 5 minutes, further `/reboot` API calls return `429 Too Many Requests`. The rate limit clears after 5 minutes of stable uptime.

Rate-limited reboot attempts are logged at `[ERROR] [SEC]` level with the client IP address and User-Agent header for forensic analysis.

#### Recovery Options Summary

| Problem | Recovery |
|---------|----------|
| Bad OTA firmware (crash loop) | Automatic: boot watchdog reverts to `/firmware.bak` |
| Bad USB firmware (crash loop) | Manual: enter safe mode (auto after 3 crashes), upload fixed firmware via web UI or OTA |
| Bad configuration | Manual: force safe mode (`POST /safemode/force`), fix config via web UI |
| WiFi credentials wrong | Automatic: AP fallback after 10 min, fix via `http://192.168.4.1/config` |
| Device unreachable | Manual: power cycle, connect via AP mode, or USB serial |
| `/reboot` DoS attack | Automatic: rate limited after 3 rapid reboots |

## Scripts

All scripts prompt interactively for required parameters (device IP, admin password, etc.). Each script has both a Linux/macOS (`.sh`) and Windows PowerShell (`.ps1`) version with identical functionality.

| Linux/macOS | Windows (PowerShell) | Description |
|-------------|---------------------|-------------|
| `configure.sh [--local]` | `configure.ps1 [-Local]` | Configure device credentials or generate local config.txt |
| `ota-update.sh [--revert]` | `ota-update.ps1 [-Revert]` | OTA firmware upload, verify, flash, reboot, or rollback |
| `update-www.sh [file] [--usb]` | `update-www.ps1 [-File name] [-USB] [-Port COM3]` | Upload web pages to device via HTTPS or USB serial |
| `backup-config.sh` | `backup-config.ps1` | Download config.txt from device for local backup |
| `restore-config.sh [file]` | `restore-config.ps1 [-File path]` | Restore config.txt to device from a local backup |
| `generate-cert.sh [name]` | `generate-cert.ps1 [-Name "name"]` | Generate self-signed ECC P-256 cert for HTTPS |
| `burn-efuse-key.sh [port]` | `burn-efuse-key.ps1 [-Port COM3]` | Burn hardware encryption key to ESP32-S3 eFuse |

**Interactive prompts** (where applicable): Device IP, Admin password, System name, MQTT prefix, WiFi/MQTT credentials, confirmation prompts. PowerShell scripts use masked input (`Read-Host -AsSecureString`) for all password prompts.

### `scripts/configure.sh` / `configure.ps1`

Configure WiFi and MQTT credentials on the device or generate a local config file for SD card provisioning. System name is validated: alphanumeric + spaces only, max 20 characters, sanitized automatically if invalid characters are entered.

```bash
# Linux/macOS
./scripts/configure.sh           # Push config to device via HTTPS API
./scripts/configure.sh --local   # Generate data/config.txt for SD card

# Windows (PowerShell)
.\scripts\configure.ps1           # Push config to device via HTTPS API
.\scripts\configure.ps1 -Local    # Generate data\config.txt for SD card
```

**Prompts (network mode):** Device IP, Admin password, System Name, MQTT Topic Prefix, WiFi SSID, WiFi password, Current WiFi password (if changing), MQTT host/port/user, MQTT password, Current MQTT password (if changing)

**Prompts (local mode):** System Name, MQTT Topic Prefix, WiFi SSID, WiFi password, MQTT host/port/user, MQTT password

### `scripts/ota-update.sh` / `ota-update.ps1`

OTA firmware update via HTTPS. Uploads the PlatformIO build output to the device SD card, verifies the upload size, applies (backs up current firmware + flashes new), and waits for reboot.

```bash
# Linux/macOS
./scripts/ota-update.sh           # Upload and flash firmware
./scripts/ota-update.sh --revert  # Roll back to previous firmware backup

# Windows (PowerShell)
.\scripts\ota-update.ps1           # Upload and flash firmware
.\scripts\ota-update.ps1 -Revert   # Roll back to previous firmware backup
```

**Prompts:** Device IP, Admin password

**Requires:** Built firmware at `.pio/build/freenove_esp32_s3_wroom/firmware.bin`

### `scripts/update-www.sh` / `update-www.ps1`

Upload HTML files from `data/www/` to the device LittleFS `/www/` directory via HTTPS, or flash the entire LittleFS image via USB serial.

```bash
# Linux/macOS
./scripts/update-www.sh                    # Upload all files over HTTPS
./scripts/update-www.sh dashboard.html     # Upload a single file
./scripts/update-www.sh --usb [port]       # Flash via USB serial

# Windows (PowerShell)
.\scripts\update-www.ps1                    # Upload all files over HTTPS
.\scripts\update-www.ps1 -File dashboard.html
.\scripts\update-www.ps1 -USB [-Port COM3]  # Flash via USB serial
```

**Prompts (HTTPS mode):** Device IP, Admin password

### `scripts/backup-config.sh` / `backup-config.ps1`

Download `config.txt` from the device SD card for local backup via FTP. Saves timestamped copies to `backups/<YYYYMMDD-HHMMSS>/config.txt` and a latest copy at `backups/config-latest.txt`. Automatically detects the configured FTP password from the device's `/ftp` status endpoint.

```bash
# Linux/macOS
./scripts/backup-config.sh

# Windows (PowerShell)
.\scripts\backup-config.ps1
```

**Prompts:** Device IP, Admin password

**Note:** The `backups/` directory is gitignored since config files contain credentials.

### `scripts/restore-config.sh` / `restore-config.ps1`

Restore a previously backed-up `config.txt` to the device SD card via FTP. Lists available backups for selection, validates JSON before uploading, verifies the upload size matches, and optionally reboots to load the new config. Automatically detects the configured FTP password from the device's `/ftp` status endpoint.

```bash
# Linux/macOS
./scripts/restore-config.sh                              # Pick from available backups
./scripts/restore-config.sh backups/config-latest.txt    # Restore a specific file

# Windows (PowerShell)
.\scripts\restore-config.ps1                              # Pick from available backups
.\scripts\restore-config.ps1 -File backups\config-latest.txt
```

**Prompts:** Backup selection (if no file specified), Device IP, Admin password, Reboot confirmation

### `scripts/generate-cert.sh` / `generate-cert.ps1`

Generate a self-signed ECC P-256 certificate (10-year validity) for the ESP32 HTTPS server. Outputs `cert.pem` and `key.pem` to the project root. An optional system name parameter sets the certificate CN (Common Name).

```bash
# Linux/macOS
./scripts/generate-cert.sh                    # CN=ESP32 (default)
./scripts/generate-cert.sh "Goodman HP"       # CN=Goodman HP

# Windows (PowerShell)
.\scripts\generate-cert.ps1                    # CN=ESP32 (default)
.\scripts\generate-cert.ps1 -Name "Goodman HP"
```

**No interactive prompts.** Requires `openssl` (included with Git for Windows). Copy output files to SD card root.

### `scripts/burn-efuse-key.sh` / `burn-efuse-key.ps1`

Burn a hardware encryption key to ESP32-S3 eFuse for password encryption at rest. The key is read-protected — only the hardware HMAC peripheral can access it.

```bash
# Linux/macOS
./scripts/burn-efuse-key.sh [/dev/ttyUSB0]

# Windows (PowerShell)
.\scripts\burn-efuse-key.ps1 [-Port COM3]
```

**Parameters:** Serial port (default: `/dev/ttyUSB0` on Linux, `COM3` on Windows)

**Prompts:** Two confirmation prompts (type `yes`, then `BURN`)

**WARNING:** eFuse burning is permanent and irreversible. Each key block can only be written once per chip. A backup of the key is saved to `efuse_hmac_key.bin` (gitignored).

## API Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/` | | Home page with clickable cards linking to each section (served from SD `/www/index.html`) |
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
| GET | `/theme` | | Current theme and system name (`{"theme":"dark","systemName":"Goodman HP"}`) |
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
| POST | `/reboot` | Yes | Reboot the device (2s delay). Rate limited: returns 429 after 3 rapid reboots |
| POST | `/safemode/force` | Yes | Force safe mode on next boot (one-shot flag, cleared after entering safe mode) |
| POST | `/safemode/clear` | Yes | Clear forced safe mode flag and reboot |
| GET | `/ftp` | Yes | FTP server status (`{"active":bool,"remainingMinutes":N,"password":"..."}`) |
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
  "defrostCntPending": false,
  "defrostCntPendingRemainSec": 0,
  "defrostExiting": false,
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
| `defrostTransition` | bool | Whether Phase 1 (pressure equalization) is active — entry or exit |
| `defrostTransitionRemainSec` | number | Seconds remaining in Phase 1 (0 when inactive) |
| `defrostCntPending` | bool | Whether Phase 2 (CNT hold) is active — entry or exit |
| `defrostCntPendingRemainSec` | number | Seconds remaining in Phase 2 (0 when inactive) |
| `defrostExiting` | bool | Whether a defrost exit transition is in progress (reverse 3-phase) |
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
| `safeMode` | bool | Whether the device is in safe mode (crash recovery or forced) |
| `crashBootCount` | number | Number of consecutive crash boots (resets after 30s stable uptime) |
| `resetReason` | string | ESP32 reset reason (e.g., `POWERON`, `SW_RESET`, `PANIC`, `INT_WDT`) |
| `rebootRateLimited` | bool | Whether reboot API calls are rate-limited (clears after 5 min stable) |

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
3. `POST /apply` — Backs up running firmware to `/firmware.bak` with build date metadata (`/firmware.bak.meta`), then flashes new firmware from SD and reboots
4. `POST /revert` — Roll back to previous firmware from SD backup

If the new firmware causes a crash boot loop, the boot watchdog automatically reverts to the backup (see [Crash Recovery](#crash-recovery)).

## MQTT Topics

The controller publishes to a configurable MQTT broker (default `192.168.0.46:1883`). The topic prefix defaults to `goodman` but is configurable via `system.mqttPrefix` in config JSON or the config page, allowing multiple units on the same broker (e.g., `unit1/temps`, `unit2/temps`).

Subscribe with `mosquitto_sub -t "goodman/#"` to receive all topics (replace `goodman` with your configured prefix).

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
  "defrostTransition": false,
  "defrostCntPending": false,
  "defrostExiting": false,
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
| `defrostTransition` | bool | Whether Phase 1 (pressure equalization) is active — entry or exit |
| `defrostCntPending` | bool | Whether Phase 2 (CNT hold) is active — entry or exit |
| `defrostExiting` | bool | Whether a defrost exit transition is in progress (distinguishes exit from entry) |
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

## Known Issues

### MCP9600 I2C Bus Crash

Some MCP9600 thermocouple amplifier chips have a hardware bug where a bare I2C address probe (sending only the address byte with no register data via `beginTransmission`/`endTransmission`) crashes the chip and locks up the entire I2C bus. The chip remains unresponsive until power cycled — a software reboot is not sufficient to recover it.

**Workaround implemented:** The firmware initializes the MCP9600 via its Adafruit driver (which performs a full Device ID register read at 0x20) *before* any I2C bus scan, and skips address 0x67 in all scan loops. The `/i2c/scan` web endpoint uses a full register read instead of a bare probe for 0x67. The Adafruit MCP9600 library itself already passes `begin(false)` internally to skip the bare probe — the issue was only with the generic I2C scan code.

If the MCP9600 stops responding after flashing firmware that performed bare I2C scans, a full power cycle (unplug power, not just software reboot) will recover it.

**Reference:** [Adafruit Forums — MCP9600 I2C issues](https://forums.adafruit.com/viewtopic.php?t=163742)

## Known Bugs

| # | Description | Severity | Fixed In |
|---|-------------|----------|----------|
| [1](bugs/1.md) | Defrost exit turns on CNT+FAN with Y inactive — compressor runs without indoor airflow. `_defrostStartTick=0` causes false 15-min timeout when Y drops during Phase 1/2, triggering exit sequence that turns on compressor without checking Y. Suction temp peaked at 148°F. | Critical | `028e568` |
| [2](bugs/2.md) | Manual override bypasses startup lockout — CNT can be turned ON 69 seconds after reboot during 180s lockout period. `setManualOverride()` and `setManualOutput()` did not check `_startupLockout`. Short cycle condition had redundant AND making it ineffective after 30s. | High | `6f0871c` |

## Dependencies

Managed automatically by PlatformIO. Key libraries:

- [TaskScheduler](https://github.com/arkhipenko/TaskScheduler) — Cooperative multitasking
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) — OneWire sensor driver
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) — Async HTTP/WebSocket server
- [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) — MQTT client
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON parsing/serialization
- [ESP32-targz](https://github.com/tobozo/ESP32-targz) — tar.gz compression for log rotation
- [Adafruit MCP9600](https://github.com/adafruit/Adafruit_MCP9600) — I2C thermocouple amplifier driver
- [Adafruit MAX6675](https://github.com/adafruit/MAX6675-library) — SPI thermocouple reader driver
- [SD](https://github.com/espressif/arduino-esp32/tree/master/libraries/SD) — Arduino SD card library (used for all file operations)
- [SimpleFTPServer](https://github.com/xreef/SimpleFTPServer) — FTP server for SD card file uploads (STORAGE_SD mode)
