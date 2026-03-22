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
24VAC ── 33kΩ ──┬── 100nF ── AIN3N (COM)
                │
               AIN3P
                │
               1kΩ
                │
COM ───────────┘
```

- Ratio: 1:34 → 24V RMS becomes 0.71V RMS (1.0V peak)
- Within ADS131M04 ±1.2V input range at PGA=1x
- 100nF across 1kΩ for HF noise rejection
- **Phase accuracy**: Resistive divider introduces no phase shift (unlike a
  transformer-based reference). The voltage sample is phase-true to the
  actual line voltage.

## CT Clamp Interface (Ch1, Ch2, Ch4)

Identical circuit to the existing ADS1115 daughter board. Three 3.5mm TRS
jacks: two SCT-013-030 (30A, compressor + fan) and one SCT-013-005 (5A,
crankcase heater).

```
                    2.5V Vbias
                       │
                   ┌─ 10kΩ ─┐
                   │         │
CT tip ── 10μF ──┬─┤         ├─┬── 10μF ── CT ring
                 │ │         │ │
               AINxP      AINxN
                   │         │
                   └─ 10kΩ ─┘
                       │
                    2.5V Vbias
```

**DC blocking caps on both sides:** 10μF on both tip (AINxP) and ring (AINxN)
for symmetric DC blocking. At 60Hz with 10kΩ bias resistors, the high-pass
cutoff is 1.6Hz — transparent to the 60Hz signal. Going smaller than 4.7μF
risks attenuation (0.1μF + 10kΩ = 159Hz cutoff, would attenuate 60Hz).

**Vbias generation:**
```
5V ── 1kΩ ──┬── 1kΩ ── GND
            │
           100nF + 10μF
            │
          2.5V out
```

If the board runs at 3.3V only (no 5V rail), use 2× 1kΩ from 3.3V for
1.65V bias instead. Adjust PGA gain accordingly.

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
