# CAN Bus Implementation — GoodmanHPCtrl

## Overview

GoodmanHPCtrl joins a CAN bus network as **node 0x03 (HP_CTRL)** at 250 kbps using the ESP32 TWAI driver. The CAN bus connects three nodes:

| Node ID | Device | Role |
|---------|--------|------|
| 0x01 | AThermostat | Sends mode/setpoints, receives HP status |
| 0x02 | Thermo Display | Receives state for display |
| 0x03 | GoodmanHPCtrl | Publishes HP state/temps, receives thermostat mode |

## GPIO Pins

| Signal | GPIO | Direction |
|--------|------|-----------|
| CAN TX | GPIO 38 | Output (to SN65HVD230DR pin 1 "D") |
| CAN RX | GPIO 14 | Input (from SN65HVD230DR pin 4 "R") |

## CAN Transceiver — SN65HVD230DR

The SN65HVD230DR (TI, SOIC-8) is the physical layer interface between the ESP32 TWAI controller and the CAN bus. It operates at 3.3V — no level shifter needed for the ESP32-S3.

### Key Specs

| Parameter | Value |
|-----------|-------|
| Supply Voltage | 3.0–3.6V |
| Data Rate | Up to 1 Mbps (250 kbps used) |
| Bus Nodes | Up to 120 |
| ESD Protection | +/-15kV HBM |
| Standby Current | 650 uA typical |
| Operating Temp | -40 to 125°C |
| Package | SOIC-8 |

### Pin Configuration

```
              +---------+
     D    1  |o        |  8   Rs
    GND   2  |         |  7   CANH
    VCC   3  |         |  6   CANL
     R    4  |         |  5   Vref
              +---------+
```

### Wiring to ESP32-S3

| SN65HVD230DR Pin | Name | Connection | Notes |
|------------------|------|------------|-------|
| 1 | D (TXD) | ESP32 GPIO 38 | CAN transmit data input — drives dominant/recessive on bus |
| 2 | GND | Ground | Common ground with ESP32 |
| 3 | VCC | 3.3V | ESP32 3.3V rail — add 100nF bypass cap to GND |
| 4 | R (RXD) | ESP32 GPIO 14 | CAN receive data output — dominant=low, recessive=high |
| 5 | Vref | — | VCC/2 reference output (1.65V) — leave unconnected or use for split termination |
| 6 | CANL | CAN bus low | Twisted pair to bus, 120 ohm termination at each end |
| 7 | CANH | CAN bus high | Twisted pair to bus, 120 ohm termination at each end |
| 8 | Rs | GND | High-speed mode (tie to GND for 250 kbps). Optional: 10k–100k to GND for slope control to reduce EMI |

### Bus Termination

Each end of the CAN bus requires a 120 ohm termination resistor between CANH and CANL. For a 3-node network (AThermostat, Display, GoodmanHPCtrl), place termination at the two physically furthest nodes. Alternatively, split termination can be used: two 60 ohm resistors in series with a 4.7nF cap from the midpoint to GND for improved EMI.

### Schematic

```
                          SN65HVD230DR
ESP32-S3                 +---------+
                         |         |
GPIO 38 (TX) -----> D  1|         |8  Rs ---+--- GND
                         |         |        |
                   GND  2|         |7  CANH -----> CAN Bus H
                         |         |               (120R term)
            3.3V ---+--- VCC 3|         |6  CANL -----> CAN Bus L
                    |    |         |               (120R term)
                  100nF  R  4|         |5  Vref (NC)
                    |    |         |
                   GND   +---------+
                         |
GPIO 14 (RX) <-----------+
```

## Configuration

CAN mode is controlled by `can.enabled` in `/config.txt` JSON:

```json
{
  "can": { "enabled": false }
}
```

When `can.enabled = true`:
- Physical Y and O input pins are ignored
- Y/O state is derived from CAN message 0x100 (thermostat mode)
- DFT and LPS remain physical (local safety devices)
- HP state and temps are published every 2s
- Heartbeat sent every 5s

When `can.enabled = false` (default):
- Normal operation with physical Y/O/LPS/DFT pins
- No CAN bus activity

## Message IDs

### Published by GoodmanHPCtrl

#### 0x200 — HP State (8 bytes, every 2s)

| Byte | Field | Encoding |
|------|-------|----------|
| 0 | State | 0=OFF, 1=COOL, 2=HEAT, 3=DEFROST, 4=ERROR, 5=LOW_TEMP |
| 1 | Outputs | bit0=FAN, bit1=CNT, bit2=W, bit3=RV, bit4=AUX |
| 2 | Faults | bit0=LPS, bit1=lowTemp, bit2=compOT, bit3=suctionLT, bit4=rvFail, bit5=highSuct |
| 3 | Protections | bit0=defrost, bit1=dfrostTrans, bit2=dfrostCntPend, bit3=dfrostExit, bit4=startupLockout, bit5=shortCycle, bit6=manualOverride, bit7=stateValidation |
| 4-5 | Heat Runtime | uint16 big-endian, minutes |
| 6 | Inputs | bit0=LPS, bit1=DFT, bit2=Y, bit3=O |
| 7 | Band & Source | bits[1:0]=defrost band (0=Cold, 1=Mid, 2=Warm), bits[3:2]=ambient source (0=sensor, 1=weather, 2=internal) |

**State enum values:**

| Value | State | Description |
|-------|-------|-------------|
| 0 | OFF | No demand — all outputs off |
| 1 | COOL | Cooling mode (Y+O active, RV on) |
| 2 | HEAT | Heating mode (Y active, O inactive, RV off) |
| 3 | DEFROST | Software defrost active (3-phase sequencing) |
| 4 | ERROR | LPS fault — compressor shut down |
| 5 | LOW_TEMP | Ambient below threshold — compressor blocked |

**Output bit flags (byte 1):**

| Bit | Output | Description |
|-----|--------|-------------|
| 0 | FAN | Outdoor fan relay |
| 1 | CNT | Compressor contactor relay |
| 2 | W | Auxiliary/emergency heat relay |
| 3 | RV | Reversing valve relay (ON=COOL, OFF=HEAT) |
| 4 | AUX | Auxiliary heat signal output |

**Fault bit flags (byte 2):**

| Bit | Fault | Description |
|-----|-------|-------------|
| 0 | LPS | Low pressure switch fault |
| 1 | lowTemp | Low ambient temperature protection active |
| 2 | compOT | Compressor over-temperature (>240°F) |
| 3 | suctionLT | Suction line low temperature (<32°F, COOL only) |
| 4 | rvFail | Reversing valve failure (latched) |
| 5 | highSuct | High suction temp during defrost (>140°F) |

**Protection bit flags (byte 3):**

| Bit | Protection | Description |
|-----|------------|-------------|
| 0 | defrost | Software defrost flag active |
| 1 | dfrostTrans | Defrost transition Phase 1 (pressure equalization) |
| 2 | dfrostCntPend | Defrost Phase 2 (RV+W on, waiting CNT) |
| 3 | dfrostExit | Defrost exit transition active |
| 4 | startupLockout | 3-minute startup lockout |
| 5 | shortCycle | Short cycle protection delay |
| 6 | manualOverride | Manual pin override active |
| 7 | stateValidation | State validation hold timer |

**Defrost band values (byte 7, bits[1:0]):**

| Value | Band | Ambient Range | Runtime Threshold |
|-------|------|---------------|-------------------|
| 0 | Cold | ≤ 23°F | 30 min |
| 1 | Mid | 23–31°F | 60 min |
| 2 | Warm | ≥ 31°F | 90 min |

**Ambient source values (byte 7, bits[3:2]):**

| Value | Source | Description |
|-------|--------|-------------|
| 0 | Sensor | Physical OneWire AMBIENT_TEMP sensor |
| 1 | Weather | MQTT or HTTP weather data |
| 2 | Internal | ESP32 internal die temperature |

#### 0x201 — HP Temperatures (8 bytes, every 2s)

| Byte | Field | Encoding |
|------|-------|----------|
| 0-1 | AMBIENT_TEMP | int16 big-endian, value × 10 (e.g., 725 = 72.5°F) |
| 2-3 | CONDENSER_TEMP | int16 big-endian, value × 10 |
| 4-5 | SUCTION_TEMP | int16 big-endian, value × 10 |
| 6-7 | LIQUID_TEMP | int16 big-endian, value × 10 |

Invalid/unavailable sensors use the sentinel value **-9999**.

**Decoding example (C):**
```c
float ambient = (int16_t)((data[0] << 8) | data[1]) / 10.0f;
if (ambient == -999.9f) { /* sensor invalid */ }
```

#### 0x3FF — Heartbeat (5 bytes, every 5s)

| Byte | Field | Encoding |
|------|-------|----------|
| 0 | Node ID | 0x03 (HP_CTRL) |
| 1-4 | Uptime | uint32 big-endian, seconds since boot |

### Received by GoodmanHPCtrl

#### 0x100 — Thermostat State (from AThermostat, 6 bytes)

| Byte | Field | Encoding |
|------|-------|----------|
| 0 | Mode | 0=OFF, 1=HEAT, 2=COOL, 3=AUTO, 4=FAN |
| 1-2 | Heat Setpoint | int16 big-endian, value × 10 |
| 3-4 | Cool Setpoint | int16 big-endian, value × 10 |
| 5 | Flags | bit0=forceFurnace, bit1=forceNoHP, bit2=defrost |

**Mode to Y/O mapping (when CAN mode enabled):**

| Mode | Y | O | HP Behavior |
|------|---|---|-------------|
| 0 (OFF) | false | false | System off |
| 1 (HEAT) | true | false | Heating — RV off |
| 2 (COOL) | true | true | Cooling — RV on |
| 3 (AUTO) | — | — | Ignored (AThermostat resolves to HEAT or COOL) |
| 4 (FAN) | false | false | No compressor (fan-only at thermostat) |

**Flag bits:**

| Bit | Flag | Effect on HP |
|-----|------|--------------|
| 0 | forceFurnace | Not used by HP (thermostat-internal) |
| 1 | forceNoHP | Overrides all → Y=false, O=false (HP off) |
| 2 | defrost | Triggers `forceDefrost()` on HP |

## Safety Features

### CAN Timeout (10 seconds)

If no 0x100 message is received for 10 seconds, `isYActive()` returns `false` — the system transitions to OFF state (safe shutdown). This prevents the compressor from running indefinitely if CAN communication is lost.

### Physical Safety Inputs

DFT (defrost thermostat) and LPS (low pressure switch) always use physical GPIO pins regardless of CAN mode. These are local safety devices that must never be virtualized.

### Existing Protections

All existing protections remain active in CAN mode:
- 3-minute startup lockout
- 5-minute short cycle protection
- LPS fault shutdown
- Low ambient temperature protection
- Compressor over-temperature shutdown
- Suction low-temperature protection (COOL)
- State validation delay
- Output state validation (table-driven, every 10s)

## Bus Recovery

The TWAI driver monitors for bus-off conditions. If detected, automatic recovery is initiated via `twai_initiate_recovery()`. TX failures are counted and logged (except timeouts, which are normal when no other node ACKs).
