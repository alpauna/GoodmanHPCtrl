# CT Clamp Current Sensing — Schematic & Parts

## Overview

Two SCT-013-030 split-core CT clamps measure AC current on the compressor and fan
circuits. A custom PCB with an ADS1115 16-bit I2C ADC reads the CT output voltages
in differential mode and reports RMS current to the ESP32. I2C level shifting is
handled by the existing PCA9306DCUR on the main board (3.3V ↔ 5V).

**EasyEDA project**: `SCH_Schematic1_2026-02-28` (Board1, V1.0)

## Important: SCT-013-030 vs SCT-013-000

| Model | Output | Burden Resistor | Max Input |
|-------|--------|-----------------|-----------|
| **SCT-013-030** | 0–1V AC (voltage) | Built-in (~62 ohm) | 30A |
| SCT-013-000 | 0–50mA AC (current) | **External required** | 100A |

The **SCT-013-030 has a built-in burden resistor** — it outputs voltage directly.
No external burden resistor is needed.

## Circuit Design

### Why Differential + DC Bias?

The CT clamp outputs **AC voltage centered at 0V**, swinging positive and negative.
The ADS1115 inputs must stay between GND and VDD (5V). A DC bias circuit at
VDD/2 (2.5V) shifts the signal into the valid input range.

Differential mode (A0-A1, A2-A3) rejects common-mode noise from the high-current
AC environment and reads the true CT signal regardless of the bias point.

### ADS1115 Configuration

- **VDD**: 5V (via PCA9306 level-shifted I2C from 3.3V ESP32)
- **Gain**: GAIN_TWO (±2.048V full-scale differential)
- **Resolution**: 0.0625 mV/bit (2.048V / 32768)
- **Current resolution**: ~1.9 mA (0.0625mV × 30 A/V)
- **I2C address**: 0x48 (ADDR pin to GND)
- **Channels**: A0-A1 differential = compressor, A2-A3 differential = fan

### Signal Levels (SCT-013-030 at 5V VDD)

At 30A (full scale): 1.0V RMS → 1.414V peak
- Vbias = 2.5V (R7+R8 divider)
- AIN swings: 2.5V ± 0.707V = **1.79V to 3.21V** (within 0–5V)
- Differential: ±1.414V peak (within ±2.048V GAIN_TWO range)

## Schematic

```
                         VDD (5V)
                           |
                        [R7 1k]
                           |
                   Vbias --+-- [C5 100nF] -- GND
                   (2.5V)  +-- [C6 10uF] --- GND
                           |
                        [R8 1k]
                           |
                          GND


    COMPRESSOR CT (SCT-013-030)            FAN CT (SCT-013-030)
    ┌─────────────────────┐                ┌─────────────────────┐
    │  3.5mm jack (U2):   │                │  3.5mm jack (U3):   │
    │  Tip    = CT wire + │                │  Tip    = CT wire + │
    │  Sleeve = CT wire - │                │  Sleeve = CT wire - │
    │  PJ-3200A-3A        │                │  PJ-3200A-3A        │
    └─────────────────────┘                └─────────────────────┘

      CT+ (tip)     CT- (sleeve)             CT+ (tip)     CT- (sleeve)
        |               |                      |               |
     [C3 10uF]          |                   [C4 10uF]          |
        |               |                      |               |
        |  [R5 10k]     |  [R9 10k]           |  [R6 10k]     |  [R10 10k]
        |   to Vbias    |   to Vbias           |   to Vbias    |   to Vbias
        |               |                      |               |
       AIN1            AIN0                   AIN3            AIN2
        |               |                      |               |
        +-------+-------+                      +-------+-------+
                |                                       |
          Differential 0_1                        Differential 2_3
          (Compressor)                            (Fan)


                ADS1115IRUGT (U1)
              ┌─────────────────────────┐
              │                         │
    AIN0 ─────┤ A0 (4)   VDD (8) ── 5V │── [C1 100nF] ── GND
    AIN1 ─────┤ A1 (5)   GND (3) ── GND│── [C2 10uF] ── GND
    AIN2 ─────┤ A2 (6)   SCL (10) ─────│── H1 pin 4
    AIN3 ─────┤ A3 (7)   SDA (9) ──────│── H1 pin 3
              │ ADDR (1) ── GND         │ (address 0x48)
              │ ALRT (2)    NC          │
              └─────────────────────────┘


              I2C Header (H1)
              ┌───────────────┐
              │ 1: GND        │
              │ 2: 5V         │
              │ 3: SDA        │── to PCA9306 → ESP32 GPIO8
              │ 4: SCL        │── to PCA9306 → ESP32 GPIO9
              └───────────────┘
              2.54mm 4-pin female
```

### Per-Channel Detail

```
                            Vbias (2.5V)
                               |
                     ┌─── [10k] ────┐
                     |               |
    CT Tip ─[10uF]──+               +──────── CT Sleeve
                     |               |
                    AIN1            AIN0       (compressor: R5, R9)
                    AIN3            AIN2       (fan: R6, R10)
                     |               |
                     └───── ADS1115 differential pair ─────┘

The 10uF coupling capacitor in series with the CT tip blocks any DC offset
from the CT coil and passes only the AC current signal. Both sides of each
differential pair are biased to Vbias (2.5V) through 10k resistors to keep
the ADS1115 inputs within the valid 0–5V range.
```

### CT Clamp Installation

The CT clamp clips around a **single conductor** (hot OR neutral, not both).
Clamping around both wires in a cable will read zero (the currents cancel).

```
    ┌──────────────────┐
    │   Circuit Breaker │
    │   Panel           │
    │                   │
    │   Compressor ─────┼──── HOT ──╶ ╴──╶ ╴── To compressor
    │   Breaker    ─────┼──── NEU ────────────── contactor
    │                   │         ↑
    │                   │    CT clamp clips
    │                   │    around HOT only
    │   Fan        ─────┼──── HOT ──╶ ╴──╶ ╴── To fan motor
    │   Breaker    ─────┼──── NEU ──────────────
    │                   │         ↑
    │                   │    CT clamp clips
    │                   │    around HOT only
    └──────────────────┘
```

## Bill of Materials (PCB)

All SMD components sourced from LCSC for JLCPCB assembly.

| Ref | Value | Package | Manufacturer Part | LCSC # | Qty |
|-----|-------|---------|-------------------|--------|-----|
| U1 | ADS1115IRUGT | X2QFN-10 | TI ADS1115IRUGT | C701611 | 1 |
| U2, U3 | PJ-3200A-3A | TH audio jack | PJ-3200A-3A | C19100325 | 2 |
| R5, R6, R9, R10 | 10k | 0402 | YAGEO RT0402BRD0710KL | C190095 | 4 |
| R7, R8 | 1k | 0402 | YAGEO RT0402BRD071KL | C852624 | 2 |
| C1, C5 | 100nF | 0805 | YAGEO CC0805KRX7R9BB104 | C49678 | 2 |
| C2, C3, C4, C6 | 10uF | 0805 | Samsung CL21B106KOQNNNE | C95841 | 4 |
| H1 | 4-pin female header | 2.54mm TH | 2.54-1×4P | C2718488 | 1 |

**Total: 7 unique parts, 16 components**

### Additional Parts (not on PCB)

| Part | Spec | Source | Approx. Price |
|------|------|--------|---------------|
| SCT-013-030 (×2) | 30A/1V, split-core, 3.5mm plug | [Amazon (2-pack)](https://www.amazon.com/SCT-013-030-Non-invasive-Current-Transformer-Sensor/dp/B07MJJRNSW) | ~$12/pair |
| SCT-013-030 (×2) | 30A/1V, split-core, 3.5mm plug | [eBay](https://www.ebay.com/itm/253063690870) | ~$5 each |

## Software Configuration

After hardware installation, configure via the web config page:

| Setting | Default | Description |
|---------|---------|-------------|
| Compressor Overcurrent (A) | 0 (disabled) | Threshold in amps — shuts down CNT |
| Fan Overcurrent (A) | 0 (disabled) | Threshold in amps — shuts down CNT |
| Overcurrent Delay (s) | 5 | Seconds current must exceed threshold |
| Locked Rotor Threshold (A) | 0 (disabled) | Inrush current threshold after CNT start |
| Locked Rotor Timeout (s) | 5 | Seconds before latching locked rotor fault |

### Typical Values for Goodman Heat Pumps

These are starting points — measure your actual equipment and set 120% of normal:

| Parameter | 2-ton | 3-ton | 5-ton |
|-----------|-------|-------|-------|
| Compressor RLA | ~10A | ~14A | ~22A |
| Compressor LRA | ~50A | ~65A | ~95A |
| Fan FLA | ~1.5A | ~2A | ~3A |
| Overcurrent threshold | ~12A | ~17A | ~26A |
| Locked rotor threshold | ~25A | ~30A | ~28A |

RLA = Rated Load Amps (normal running), LRA = Locked Rotor Amps (stall),
FLA = Full Load Amps. Values from equipment nameplate — always use your
unit's actual ratings.

## Notes

- The I2C bus is shared with the MCP9600 thermocouple at 0x67. The ADS1115 at
  0x48 coexists without conflict on GPIO8 (SDA) / GPIO9 (SCL).
- I2C level shifting (3.3V ESP32 ↔ 5V ADS1115) handled by PCA9306DCUR on the
  main board. No additional level shifting needed on the current monitor PCB.
- CT clamps are galvanically isolated — safe to clip onto live conductors
  without breaking the circuit.
- For circuits >30A, use the SCT-013-000 (100A current output) with a calculated
  burden resistor: R_burden = V_desired / (I_max / turns_ratio). For 30A range
  with SCT-013-000 (2000:1): R = 1V / (30A/2000) = 66.7 ohm.
