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
board as the 24VAC/COM bus. COM is tied directly to digital GND (half-wave
power supply design), so no isolation is needed.

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

- Ratio: 1:34 → 24V RMS becomes 0.71V RMS (1.0V peak)
- Within ADS131M04 ±1.2V input range at PGA=1x
- COM = digital GND — single ground domain, no isolation required
- 10μF coupling cap blocks DC offset from transformer asymmetric loading
  (high-pass cutoff ~16Hz with 1kΩ — transparent to 60Hz)
- 100nF across 1kΩ for HF noise rejection
- TVS clamps transients from transformer inrush and line surges
- **Phase accuracy**: Resistive divider introduces no phase shift (unlike a
  transformer-based reference). The voltage sample is phase-true to the
  actual line voltage

### Power Supply: Half-Wave Rectifier Design

The main board uses a half-wave rectifier (single diode from 24VAC) instead
of a full-wave bridge, tying COM directly to digital GND. This creates a
single ground domain across the entire system, eliminating the need for
galvanic isolation on the voltage reference and inter-board digital buses
(CAN, I2C).

Tradeoff: ~2× ripple vs full-wave bridge (16.7ms hold-up at 60Hz vs 8.3ms).
Mitigated by using 1000μF 50V input filter cap (vs 470μF 63V). 50V rating
is sufficient — SMAJ36CA TVS upstream clamps transients to ~58V max and the
fuse limits sustained fault current. The LGS5145 buck converter's wide input
range handles the additional ripple without issue.

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

## Simplified 24VAC Inputs (LPS, DFT, Y, O)

With the half-wave power supply design (COM = GND), the 24VAC input signals
are already ground-referenced. The AT3H7C optocouplers and SN74HC14DR Schmitt
triggers from the v2.0 board are no longer needed — replaced by a passive
resistor divider, series diode, unidirectional TVS clamp, and filter cap.

**Parts eliminated per input**: AT3H7C optocoupler, 3× 1.8kΩ resistors,
180Ω resistor, SMF5.0A TVS, 10μF filter cap, SN74HC14DR channel.

**Parts eliminated board-wide**: SN74HC14DR (U27) IC entirely (6 channels,
4 used for inputs, 2 spare).

### Per-Input Circuit

```
24VAC input ── 33kΩ ── 10μF ── 1N4148 ──┬── GPIO (ESP32)
                    (DC block)  (anode→) │
                                   PESD3V3L1BA (unidirectional, cathode to junction)
                                         │
                                       100nF
                                         │
                                    COM/GND
```

- **33kΩ**: Current limiting. At 34V peak: (34V - 0.7V - 3.3V) / 33kΩ =
  0.9mA — safe for both TVS and GPIO
- **10μF**: DC blocking cap. All input signals should be AC — blocks any DC
  offset from triac leakage, wiring faults, or asymmetric transformer loading.
  High-pass cutoff with 33kΩ: ~0.5Hz (transparent to 60Hz)
- **1N4148**: Blocks negative half-cycle. Only positive half reaches GPIO
- **PESD3V3L1BA**: Unidirectional TVS, clamps positive to 3.3V. Protects
  ESP32 GPIO from overvoltage transients
- **100nF**: Low-pass filter for noise rejection. With 33kΩ source impedance,
  cutoff ~48Hz — filters HF switching noise while passing the 60Hz envelope.
  Signal appears as ~3.3V DC (with 8.3ms ripple) when 24VAC is present

### Signal Levels

| Condition | GPIO Voltage | ESP32 Reads |
|-----------|-------------|-------------|
| 24VAC present | ~3.3V (clamped) | HIGH |
| 24VAC absent | 0V (pulled low by 100nF + internal pulldown) | LOW |
| Negative half-cycle | 0V (1N4148 blocks) | LOW |
| Transient spike | 3.3V (TVS clamps) | HIGH (protected) |

### Input Pin Configuration

No change to software — `InputPin` class still reads GPIO with `INPUT_PULLDOWN`
and debounce/validation delay. The 100nF + 33kΩ time constant (3.3ms) is fast
enough for the 500ms polling interval.

| Input | GPIO | Function |
|-------|------|----------|
| LPS | 15 | Low pressure switch (active LOW = fault) |
| DFT | 16 | Defrost thermostat |
| Y | 17 | Compressor call |
| O | 18 | Reversing valve / cool mode |

### BOM (input section, ×4 channels)

| Ref | Part | Package | Qty | Notes |
|-----|------|---------|-----|-------|
| R_in1–4 | 33kΩ | 0402 | 4 | Current limiting (1 per input) |
| C_indc1–4 | 10μF | 0805 | 4 | DC blocking (1 per input) |
| D_in1–4 | 1N4148 | SOD-323 | 4 | Negative half-cycle blocking |
| D_tvsin1–4 | PESD3V3L1BA | SOD-323 | 4 | Unidirectional 3.3V TVS clamp |
| C_in1–4 | 100nF | 0402 | 4 | Input filter cap |

**Total: 20 components** replacing 28+ components (4× optocouplers, 16×
resistors, 4× caps, 4× TVS, 1× SN74HC14DR).

## Analog Power Supply (5VPF)

The 5VPF rail feeds the ADS131M04 analog supply. L2 on the main power
schematic filters 5V → 5VPF, with C7/C8 (1μF) and C9 (22μF) output caps
and D4 (SMF5.0A) TVS protection.

**Replace L2 (10μH inductor) with a ferrite bead** (~600Ω @ 100MHz, low
DCR). A 10μH inductor can resonate with the output capacitance (C7/C8/C9),
creating a tank circuit that amplifies noise at the resonant frequency. A
ferrite bead is lossy by design — it absorbs HF switching noise from the
LGS5145 buck converter as heat instead of ringing.

```
5V ── [ferrite bead] ──┬── 5VPF
                       │
                 C7 1μF + C8 1μF + C9 22μF
                       │
                      GND
```

**Important**: L2 is on the main 5V supply path — must be rated for at
least 1A. Use a power-rated ferrite bead such as BLM31PG601SN1L (Murata,
1206, 600Ω @ 100MHz, 1.5A rated, ~0.08Ω DCR) or equivalent. Small-signal
beads (0402/0603) are not suitable — they saturate at high current and lose
their filtering properties. High impedance at LGS5145 switching frequency
(~1MHz), resistive absorption above 10MHz. Combined with the existing
output caps, forms a proper lossy low-pass filter with no resonance risk.

## PCB Layout Considerations

- Place ADS131M04 close to the CT clamp jacks to minimize analog trace length
- Keep SPI traces away from 24VAC power traces and relay switching traces
- Separate analog ground (AGND) from digital ground (DGND), tie at a single
  point under the ADC
- Vbias decoupling caps as close to the ADC input pins as possible
- 100nF bypass caps on AVDD and DVDD within 5mm of supply pins
- CT clamp differential pairs should be routed as matched-length traces
