# Power Factor Board Integration — Schematic Notes

## Context

These notes document the integration of power factor monitoring into the
Goodman HP Controller board revision, replacing the separate I2C ADS1115
daughter board with an on-board ADS131M04 SPI ADC.

## Triac Phantom Signal Fix (R79–R86)

The v2.0 furnace/HP controller board uses KMOC3021S triac optocouplers for
24VAC output switching. Triac off-state leakage can hold downstream relay
coils partially energized ("phantom" signals), causing false Y/O inputs to the
HP controller.

**Root cause observed 2026-03-22:** Thermostat went idle at 11:31 CDT but the
HP controller's Y input stayed active for 56 additional minutes. Compressor
ran with no indoor airflow, suction temp climbed to 130°F+. Confirmed via
Home Assistant thermostat history vs controller logs.

**Fix per channel (R79–R86 on furnace board, similar on HP board):**

```
Triac output ──┬── 4.7kΩ ── COM     (resistive bleeder, 123mW @ 24VAC)
               └── 470nF X2 ── COM  (capacitive bleeder, ~0W real dissipation)
```

- **4.7kΩ resistor**: Handles DC leakage path. 123mW fits 1206 or 2512
  package with margin — no thermal issues unlike the original 2kΩ (288mW)
  that desoldered SMD parts off the board.
- **470nF X2 cap**: Handles AC sneak paths. ~4.3mA reactive current at 60Hz,
  near-zero real power. X2 safety rating for across-the-line AC use.
- Combined impedance at 60Hz: ~560Ω — provides strong loading to prevent
  triac leakage from energizing relay coils.

## SPI Bus Wiring (ADS131M04)

Dedicated SPI bus, physically separate from the SD card SPI (GPIO 10–13).

```
ESP32-S3          ADS131M04
─────────         ─────────
GPIO 1  ────────  SCLK
GPIO 2  ────────  DIN  (MOSI)
GPIO 42 ────────  DOUT (MISO)
GPIO 45 ────────  /CS
GPIO 46 ────────  /DRDY

3.3V    ────┬───  AVDD
            ├───  DVDD
            │
           100nF + 10μF bypass (each supply pin)
            │
GND     ────┴───  AGND, DGND
```

**Notes:**
- GPIO 45 is a strapping pin (VDD_SPI voltage select) — reads at boot only,
  safe for CS use in normal operation. Default pulls set 3.3V VDD_SPI.
- GPIO 46 is a strapping pin (ROM boot log) — same, safe for DRDY interrupt.
- SPI clock: 10–25 MHz. ADS131M04 supports up to 25 MHz.
- DRDY pulses low when new data is available. Configure as falling-edge
  interrupt on the ESP32.

## Voltage Reference Input (Ch3)

Tap the 24VAC transformer secondary directly — already available on the HP
board as the 24VAC/COM bus.

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
              220Ω (surge limiting)
                │
COM ───────────┘
```

TVS = PESD3V3L2BT (dual bidirectional, same as CT channels)

- Ratio: 1:34 → 24V RMS becomes 0.71V RMS (1.0V peak)
  (220Ω COM resistor is negligible vs 33kΩ — no impact on divider ratio)
- Within ADS131M04 ±1.2V input range at PGA=1x
- 10μF coupling cap blocks DC offset from transformer asymmetric loading
  (high-pass cutoff ~16Hz with 1kΩ — transparent to 60Hz)
- 100nF across 1kΩ for HF noise rejection
- 220Ω series resistor on COM limits surge current into TVS_B
  (33kΩ already limits TVS_A high-side current to < 1mA at peak 24VAC)
- TVS clamps transients from transformer inrush and line surges
- **Phase accuracy**: Resistive divider introduces no phase shift (unlike a
  transformer-based reference). The voltage sample is phase-true to the
  actual line voltage.

## CT Clamp Interface (Ch1, Ch2, Ch4)

Three universal 3.5mm TRS jacks supporting both voltage-output CTs
(SCT-013-005/010/030) and current-output CTs (SCT-013-000). Each jack has
a 2-pin 2.54mm female header across tip-to-ring for socketed burden resistor
placement.

- **Voltage-output CT** (e.g., SCT-013-005/030): Leave burden header **empty**.
  CT has internal burden; outputs voltage directly.
- **Current-output CT** (SCT-013-000, 2000:1 ratio, 50mA @ 100A): Insert a
  through-hole resistor (1% tolerance or better) bent into a 2-pin male header
  into the burden socket. Output voltage = I_secondary × R_burden.

The socketed design allows field-swapping burden values without soldering —
just pull the old resistor and insert a new one to change the effective
measurement range.

### Universal Jack Circuit

```
              R_burden (2.54mm header socket, empty for voltage-output CTs)
CT tip ──┬──────[■ ■]────────────┬── CT ring
         │                        │
        10μF                     10μF
         │                        │
     10kΩ ── Vbias            10kΩ ── Vbias
         │                        │
       TVS_A                    TVS_B     ← PESD3V3L2BT (dual bidir, SOT-23)
         │                        │
       AINxP                   AINxN
```

### Burden Resistor Selection (SCT-013-000, 2000:1)

For 1.0V output at full-scale current:

| Target Range | R_burden | V @ max | Use Case |
|-------------|----------|---------|----------|
| 100A | 20Ω | 1.0V | Large commercial |
| 50A | 40Ω | 1.0V | Large compressor |
| 30A | 66Ω | 1.0V | Compressor (3-5 ton) |
| 15A | 133Ω | 1.0V | Small compressor |
| 5A | 400Ω | 1.0V | Fan motor |
| 1A | 2kΩ | 1.0V | Crankcase heater |

Formula: R_burden = V_target / (I_primary / turns_ratio) = V_target × 2000 / I_max

**Note:** R_burden must be 1% tolerance or better for accurate readings. Power
dissipation is negligible (< 1mW for all values above). Use standard 1/4W
through-hole axial resistor bent to 2.54mm pin spacing and inserted into the
burden header socket. No soldering required for burden changes.

### Default Configuration

| Channel | Default CT | R_burden | Notes |
|---------|-----------|----------|-------|
| Ch1 (Compressor) | SCT-013-030 (voltage) | unpopulated | Or SCT-013-000 + 66Ω |
| Ch2 (Fan) | SCT-013-005 (voltage) | unpopulated | Or SCT-013-000 + 400Ω |
| Ch4 (Crankcase) | SCT-013-005 (voltage) | unpopulated | Or SCT-013-000 + 2kΩ |

**DC blocking caps:** 10μF on both sides (tip and ring) for symmetric DC
blocking. At 60Hz with 10kΩ bias resistors, the high-pass cutoff is 1.6Hz —
transparent to the 60Hz signal. Going smaller than 4.7μF risks attenuation
(0.1μF + 10kΩ = 159Hz cutoff, would attenuate 60Hz).

**TVS input protection:** PESD3V3L2BT,215 (Nexperia, LCSC C55440) — dual
bidirectional TVS in SOT-23 package. One device per CT channel, clamps both
AINxP and AINxN to ±3.3V. The 10kΩ series resistance limits fault current
to < 1mA at clamp. Protects against motor inrush transients, hot-plugging
CT jacks under load, and ESD from handling TRS connectors.

**Vbias generation (1.65V from 3.3V rail):**
```
3.3V ── 1kΩ ──┬── 1kΩ ── GND
              │
             100nF + 10μF
              │
            1.65V out
```

- 1.65V mid-rail bias from 3.3V supply via equal resistor divider
- CT signal swings symmetrically around 1.65V
- Within ADS131M04 ±1.2V input range at PGA=1x (1.65V ± 1.0V peak)

## SN74HC14DR Spare Channels

The existing SN74HC14DR (U27) has 6 Schmitt trigger inverter channels.
4 are used (Y, DFT, LPS, O inputs). 2 are spare (channels 5 and 6).

With the ADS131M04 sampling voltage on Ch3, the spare Schmitt trigger
channels are not needed for zero-crossing detection. They remain available
for future use (e.g., additional digital inputs, status indicators).

If a hardware voltage ZC is desired as a backup or for independent timing:
- Duplicate the existing input circuit (3× 1.8kΩ + SMF5.0A + 180Ω +
  AT3H7C optocoupler) but **omit the 10μF filter cap** (use 100nF or none)
- Output through SN74HC14DR channel 5 → GPIO 47
- Produces 120Hz square wave (ZC on both half-cycles due to anti-parallel
  LEDs in AT3H7C)
- ESP32 ISR timestamps falling edges with `micros()`

## PCB Layout Considerations

- Place ADS131M04 close to the CT clamp jacks to minimize analog trace length
- Keep SPI traces away from 24VAC power traces and relay switching traces
- Separate analog ground (AGND) from digital ground (DGND), tie at a single
  point under the ADC
- Vbias decoupling caps as close to the ADC input pins as possible
- 100nF bypass caps on AVDD and DVDD within 5mm of supply pins
- CT clamp differential pairs should be routed as matched-length traces
