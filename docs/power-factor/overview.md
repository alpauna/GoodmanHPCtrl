# Power Factor Monitoring

## Overview

Power factor (PF) measurement for compressor and fan motors using simultaneous
voltage and current sampling. Replaces the I2C ADS1115 daughter board with an
on-board SPI ADC (ADS131M04) that provides true RMS current, real power,
apparent power, reactive power, and power factor — all computed from raw
waveform samples.

## Why Power Factor

- **Motor health**: A dropping PF on the compressor indicates winding
  degradation, bearing problems, or mechanical loading issues before they
  become catastrophic failures
- **Locked rotor detection**: A severely lagging PF during startup confirms a
  stalled motor versus normal inrush
- **Efficiency monitoring**: Real power (W) vs apparent power (VA) shows how
  much energy is wasted as reactive power
- **Diagnostic data**: PF changes correlate with refrigerant charge issues,
  iced coils, and airflow restrictions

## Architecture

### ADC: ADS131M04

| Parameter | Value |
|-----------|-------|
| Manufacturer | Texas Instruments |
| Channels | 4 independent, **simultaneous-sampling** |
| Resolution | 24-bit delta-sigma |
| Sample rate | Up to 32 kSPS per channel |
| Interface | SPI (up to 25 MHz) |
| Input mode | Differential per channel |
| PGA | 1x–128x internal programmable gain |
| Reference | 1.2V internal |
| DRDY | Active-low data-ready interrupt |
| Package | TQFP-32 or QFN-24 |
| LCSC | C2904283 (TQFP-32) |

**Why simultaneous sampling matters:** Multiplexed ADCs (like the ADS1115)
sample channels sequentially. At 60Hz, even a 1ms delay between voltage and
current samples introduces ~21° of phase error — enough to make PF
measurements meaningless. The ADS131M04 samples all 4 channels at the exact
same instant, eliminating inter-channel phase error entirely.

### Channel Allocation

| Channel | Differential Pair | Function |
|---------|-------------------|----------|
| Ch1 | AIN1P / AIN1N | Compressor CT clamp (SCT-013-030, 30A range) |
| Ch2 | AIN2P / AIN2N | Fan CT clamp (SCT-013-030, 30A range) |
| Ch3 | AIN3P / AIN3N | 24VAC voltage reference (resistor divider) |
| Ch4 | AIN4P / AIN4N | Crankcase heater CT clamp (SCT-013-005, 5A range) |

**Crankcase heater monitoring (Ch4):** Scroll compressor heat pumps have a
resistive crankcase heater (typically 40–80W at 240V, drawing 0.17–0.33A)
that keeps compressor oil warm during off-cycles to prevent liquid refrigerant
migration. A failed heater leads to oil dilution and compressor slugging on
cold-weather startup — potentially destroying scroll valves.

The SCT-013-030 (30A range) has poor resolution at sub-amp levels. Use an
**SCT-013-005** (5A range, 1V output) for the heater channel. Same 3.5mm TRS
jack, same circuit. Set ADS131M04 PGA to 4x or 8x for optimal sensitivity.

**Heater diagnostics:**
- Heater ON + compressor OFF = normal (oil warming)
- Heater OFF + compressor OFF = **failed heater** — log warning/fault
- Heater ON + compressor ON = wasted energy (some units don't switch it off)
- Gradual current drop over time = element degradation

### GPIO Pin Assignment (ESP32-S3)

Dedicated SPI bus, separate from the SD card SPI (GPIO 10–13).

| GPIO | Function | ADS131M04 Pin |
|------|----------|---------------|
| 1 | SPI SCLK | SCLK |
| 2 | SPI MOSI (DIN) | DIN |
| 42 | SPI MISO (DOUT) | DOUT |
| 45 | SPI CS | /CS |
| 46 | DRDY interrupt | /DRDY |

**Voltage zero-crossing (optional, if using optocoupler instead of Ch3):**

| GPIO | Function |
|------|----------|
| 47 | Voltage ZC from SN74HC14DR channel 5Y |

Using the lowest available GPIOs first. GPIO 47 is only needed if the voltage
zero-crossing is handled by the existing optocoupler + Schmitt trigger circuit
instead of ADC channel 3. With the ADS131M04 sampling voltage on Ch3, GPIO 47
is not required — zero crossings are detected from the sampled waveform.

### Existing GPIO Map (for reference)

| GPIO | Current Function |
|------|-----------------|
| 3 | AUX output relay |
| 4 | FAN output relay |
| 5 | CNT output relay |
| 6 | W output relay |
| 7 | RV output relay |
| 8 | I2C SDA (OLED, ADS1115) |
| 9 | I2C SCL |
| 10–13 | SD card SPI (CS, MOSI, MISO, CLK) |
| 14 | CAN RX (TWAI) |
| 15 | LPS input |
| 16 | DFT input |
| 17 | Y input |
| 18 | O input |
| 21 | OneWire bus |
| 38 | CAN TX (TWAI) |
| 39–41 | Unused (formerly MAX6675 SPI CLK/CS/DO) |

**Unavailable:** 0 (boot), 19–20 (USB), 26–37 (flash/PSRAM), 43–44 (Serial).

**Remaining free after PF addition:** GPIO 47, 48 (or 48 only if using
optocoupler ZC on 47).

## CT Clamp Interface

Universal 3.5mm TRS jacks supporting both voltage-output CTs (SCT-013-005/
010/030 with internal burden) and current-output CTs (SCT-013-000 with
external burden resistor). Each jack has a 2-pin 2.54mm female header across
tip-to-ring for socketed burden resistor placement — leave empty for
voltage-output CTs, insert appropriate through-hole resistor (1% or better)
for current-output CTs. No soldering required for burden changes.

### Per-Channel Circuit

```
              R_burden (2.54mm header socket, empty for voltage-output CTs)
CT tip ──┬──────[■ ■]────────────┬── CT ring
         │                        │
        10μF                     10μF
         │                        │
     10kΩ ── Vbias            10kΩ ── Vbias
         │                        │
       TVS ── GND              TVS ── GND
         │                        │
       AINxP                   AINxN
```

  TVS = PESD3V3L2BT (dual bidirectional, one per channel)

- **Vbias**: 1.65V from resistor divider (2× 1kΩ from 3.3V) + 100nF + 10μF
  filter caps. Provides DC operating point for the AC-coupled CT signal.
- **10kΩ bias resistors**: On both differential inputs, sets DC operating
  point at Vbias (1.65V)
- **10μF coupling caps**: AC-couple on **both** differential inputs for
  symmetric DC blocking. At 60Hz with 10kΩ bias, high-pass cutoff is 1.6Hz
  — transparent to the 60Hz signal
- **R_burden**: Optional external burden resistor for current-output CTs
  (SCT-013-000, 2000:1 turns ratio). R = V_target × 2000 / I_max. Use 1%
  tolerance or better (standard 1/4W through-hole axial, bent to 2.54mm pin
  spacing). See burden table in `schematic-notes.md` for common values.
- **PGA setting**: Use 1x for full range, 2x or 4x for higher sensitivity on
  low-current loads (fan motor, crankcase heater)
- **TVS protection**: PESD3V3L2BT (Nexperia, C55440) on each ADC input to
  GND. Clamps transients from motor inrush, hot-plugging, and ESD

### CT Clamp Compatibility

| CT Model | Type | Output | R_burden | Use Case |
|----------|------|--------|----------|----------|
| SCT-013-030 | Voltage | 1V @ 30A | none | Compressor (default) |
| SCT-013-010 | Voltage | 1V @ 10A | none | Small compressor |
| SCT-013-005 | Voltage | 1V @ 5A | none | Fan, crankcase heater |
| SCT-013-000 | Current | 50mA @ 100A | 66Ω → 30A range | Compressor (high precision) |
| SCT-013-000 | Current | 50mA @ 100A | 400Ω → 5A range | Fan (high precision) |
| SCT-013-000 | Current | 50mA @ 100A | 2kΩ → 1A range | Crankcase heater (high precision) |

Software CT ratio is configured per-channel via the config page dropdown.
When using SCT-013-000 with burden, set the dropdown to match the effective
range (e.g., 66Ω burden = 30A effective → select SCT-013-030).

### Voltage Reference Circuit (Ch3)

24VAC from the furnace transformer, divided down to ±1V range for ADC input.
COM is tied directly to digital GND (see power supply design note below),
so no isolation is needed.

```
24VAC ── 33kΩ ──┬── TVS_A ── GND
                │
              10μF (DC blocking)
                │
              AIN3P
                │
              100nF (HF filter)
                │
              AIN3N
                │
               1kΩ
                │
              TVS_B ── GND
                │
COM/GND ────────┘
```

TVS = PESD3V3L2BT (dual bidirectional, same as CT channels)

- Divider ratio: 1k / (33k + 1k) ≈ 1:34
- 24V RMS → 0.71V RMS at ADC input (1.0V peak) — within ±1.2V PGA=1x range
- COM = digital GND — single ground domain, no isolation required
- 10μF coupling cap blocks DC offset from transformer asymmetric loading
  (high-pass cutoff ~16Hz with 1kΩ — transparent to 60Hz)
- 100nF across 1kΩ for HF noise rejection
- TVS clamps transients from transformer inrush and line surges
- **Phase accuracy**: Resistive divider introduces no phase shift (unlike a
  transformer-based reference). The voltage sample is phase-true to the
  actual line voltage

### Power Supply Design Note

The main board power supply uses a **half-wave rectifier** (single diode)
from 24VAC, with COM tied directly to digital GND. This creates a single
ground domain across the entire system:

- **No isolation needed** for voltage reference (Ch3) — COM = GND
- **No isolated ground** required for inter-board digital buses (CAN, I2C)
- **Simplified analog front end** — all ADC channels share the same ground
- **Simplified 24VAC inputs** — optocouplers (AT3H7C) and Schmitt triggers
  (SN74HC14DR) replaced by passive divider + diode + TVS + filter cap
  directly into ESP32 GPIO. See `schematic-notes.md` for circuit detail

Tradeoff vs full-wave bridge (MB10S): half-wave produces ~2× ripple on the
input filter cap (16.7ms hold-up vs 8.3ms at 60Hz). Mitigated by using a
larger input filter cap (1000μF 50V vs 470μF). 50V rating is sufficient —
SMAJ36CA TVS clamps transients to ~58V max, and the upstream fuse limits
sustained fault current. The LGS5145 buck converter's wide input range
handles the additional ripple without issue.

## Software Design

### Sampling

The ADS131M04 DRDY pin triggers an ISR on GPIO 46. The ISR reads all 4
channels via SPI in a single transaction (~4μs at 25MHz SPI clock). Samples
are stored in a circular buffer sized for one complete 60Hz cycle.

**Target sample rate**: 4 kSPS (4000 samples/sec per channel)
- 66.7 samples per 60Hz cycle — sufficient for accurate RMS and PF
- 250μs between samples
- DRDY ISR + SPI read takes ~10μs — negligible CPU overhead

### Computation (per cycle)

Performed in a TaskScheduler task (e.g., every 500ms = 30 complete cycles
averaged):

```
V_rms = sqrt(mean(v[n]²))
I_rms = sqrt(mean(i[n]²))
P_real = mean(v[n] * i[n])          // instantaneous power, averaged
P_apparent = V_rms * I_rms
P_reactive = sqrt(P_apparent² - P_real²)
PF = P_real / P_apparent
```

**No zero-crossing detection needed.** With simultaneous V and I samples,
true power is computed directly from the instantaneous product V×I averaged
over complete cycles. PF falls out of the ratio of real to apparent power.
This is more accurate than zero-crossing phase measurement, which only works
for pure sinusoidal waveforms.

### Data Output

New fields in `/state` JSON and MQTT `goodman/state` payload:

```json
{
  "compressorPF": 0.87,
  "compressorWatts": 1850,
  "compressorVA": 2126,
  "compressorVAR": 1048,
  "fanPF": 0.72,
  "fanWatts": 285,
  "fanVA": 396,
  "fanVAR": 275,
  "voltageRMS": 24.1,
  "crankcaseHeaterAmps": 0.29,
  "crankcaseHeaterOn": true
}
```

Dashboard: Power card with PF gauge, real/apparent/reactive power display.
TempHistory: Additional chart sensors for PF and power tracking over time.

### Existing Code Changes

| File | Change |
|------|--------|
| `src/CurrentSensor.cpp` | Replace `readRMS()` ADS1115 logic with ADS131M04 SPI reads. RMS computed from sample buffer instead of per-call ADC reads |
| `include/CurrentSensor.h` | Add PF, watts, VA, VAR members and getters. Remove `Adafruit_ADS1X15.h` dependency |
| `src/main.cpp` | Initialize dedicated SPI bus on GPIO 1/2/42/45. Configure ADS131M04 registers. DRDY ISR on GPIO 46. Remove ADS1115 init |
| `src/WebHandler.cpp` | Add PF/power fields to `/state` JSON |
| `src/HttpsServer.cpp` | Mirror PF/power fields in HTTPS `/state` handler |
| `src/MQTTHandler.cpp` | Add PF/power fields to `publishState()` |
| `data/www/dashboard.html` | Power card with PF display, power charts |
| `platformio.ini` | Remove `Adafruit ADS1X15` lib dep, add ADS131M04 driver (or custom) |

### ADS131M04 vs ADS1115 Comparison

| Feature | ADS1115 (current) | ADS131M04 (new) |
|---------|-------------------|-----------------|
| Channels | 2 differential | 4 differential |
| Sampling | Multiplexed | **Simultaneous** |
| Resolution | 16-bit | 24-bit |
| Max SPS | 860 | 32,000 |
| Interface | I2C (400kHz) | SPI (25MHz) |
| PGA | 2/3x–16x | 1x–128x |
| Thread safety | I2C bus shared | Dedicated SPI bus |
| Level shifter | Required (3.3V↔5V) | Not needed (3.3V native) |
| Board | Separate daughter PCB | Integrated on main board |
| Power factor | Not possible | Yes (simultaneous V×I) |

## Bill of Materials (additions to HP board)

| Ref | Part | Package | LCSC | Qty | Notes |
|-----|------|---------|------|-----|-------|
| U_ADC | ADS131M04IRSMR | TQFP-32 | C2904283 | 1 | 4-ch simultaneous ADC |
| R_bias1–6 | 10kΩ | 0402 | — | 6 | CT clamp bias resistors (2 per channel × 3 CT channels) |
| R_divH | 1kΩ (2×) | 0402 | — | 2 | Vbias 1.65V divider (from 3.3V) |
| R_vdivH | 33kΩ | 0402 | — | 1 | Voltage divider high side |
| R_vdivL | 1kΩ | 0402 | — | 1 | Voltage divider low side |
| C_bias | 100nF + 10μF | 0402/0805 | — | 2 | Vbias filter |
| C_couple | 10μF | 0805 | — | 7 | AC coupling / DC blocking (2 per CT channel × 3 + 1 voltage Ch3) |
| D_tvs1–4 | PESD3V3L2BT,215 | SOT-23 | C55440 | 4 | ADC input TVS protection (3 CT + 1 voltage) |
| C_vfilt | 100nF | 0402 | — | 1 | Voltage input HF filter |
| H_burd1–3 | 2-pin 2.54mm female | THT | — | 3 | CT burden resistor socket (empty for voltage-output CTs) |
| C_bypass | 100nF + 10μF | 0402/0805 | — | 2 | ADC power supply bypass |
| J_CT1 | 3.5mm TRS jack | — | — | 1 | Compressor CT (universal) |
| J_CT2 | 3.5mm TRS jack | — | — | 1 | Fan CT (universal) |
| J_CT3 | 3.5mm TRS jack | — | — | 1 | Crankcase heater CT (universal) |

All three CT jacks are universal — supporting both voltage-output CTs
(SCT-013-005/010/030, burden header empty) and current-output CTs
(SCT-013-000, burden resistor inserted into header). Each jack has a
2-pin 2.54mm female header across tip-to-ring. For SCT-013-000, insert
a standard 1/4W through-hole axial resistor (1% or better tolerance)
bent to 2.54mm pin spacing into the socket. Default: empty (voltage-output
CTs). See CT compatibility table above and burden table in
`schematic-notes.md` for values when using SCT-013-000.

## References

- [ADS131M04 datasheet](https://www.ti.com/lit/ds/symlink/ads131m04.pdf)
- [TI Power Measurement app note (SBAA336)](https://www.ti.com/lit/an/sbaa336/sbaa336.pdf)
- [SCT-013-030 CT clamp datasheet](https://en.yhdc.com/product/SCT013-401.html)
- Existing current sensing design: `docs/current-sensing.md`
