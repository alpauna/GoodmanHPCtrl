# 24V AC Input Circuit Design

**Document Version:** 1.0
**Date:** 2026-03-22
**Application:** GoodmanHP HVAC Controller (ESP32-S3)
**Inputs Protected:** LPS, DFT, Y, O

---

## Overview

All four digital inputs (LPS, DFT, Y, O) use identical 24V AC signal conditioning and protection circuits. The design provides:
- **Multi-stage RC filtering** for 60Hz AC attenuation
- **Reverse polarity protection** against wiring faults
- **Precise 3.3V GPIO clamping** with Zener regulation
- **Predictable timing** via 1MΩ bleed resistor
- **Current limiting** to protect ESP32 pins

---

## Circuit Schematic

```
24V AC Input
    │
    └─── R5 (33kΩ) ───┬─── D15 (1N4148) ─── C48 (10µF) ─── D14 (SMF3.3A) ─── R79 (2.2kΩ) ─── IO15
                       │                                                           │
                    C46 (10µF)                                                    BZT52C3V3
                       │                                                           │
                      GND                                                         GND
                                                           C47 (100nF)
                                                               │
                                                              GND
```

---

## Component Specifications

### Stage 1: Input Current Limiting & Decoupling

| Component | Value | Function | Notes |
|-----------|-------|----------|-------|
| **R5** | 33kΩ, 1/4W | Current limiting from 24V AC source | Peak current: ~0.73mA (safe) |
| **C46** | 10µF, ≥50V ceramic/film | Primary decoupling, 60Hz AC filtering | τ = 33kΩ × 10µF = 330ms |

**Performance:**
- RC time constant: **330ms** (low-pass filter for 60Hz noise)
- Peak current: 24V / 33kΩ = **0.73mA**
- Power dissipation: 24² / 33000 ≈ **17.5mW RMS** (acceptable)

---

### Stage 2: Transient Protection & Secondary Decoupling

| Component | Value | Function | Notes |
|-----------|-------|----------|-------|
| **D15** | 1N4148W-7-F | Fast switching diode (transient clamp) | 75V reverse rating; ~0.7V forward drop |
| **C48** | 10µF, ≥50V ceramic/film | Secondary decoupling after protection diode | Works with R79 for predictable discharge |

**Performance:**
- Blocks transient spikes above ~1N4148 forward bias
- Charges C48 through R79 with τ = 1MΩ × 10µF = **10 seconds** (very slow, predictable timing)

---

### Stage 3: Reverse Polarity Protection

| Component | Value | Function | Notes |
|-----------|-------|----------|-------|
| **D14** | SMF3.3A (TVS) | Reverse polarity blocking | Uni-directional; VR = 3.3V, VBR = 3.4–4.3V |
| **R79** | 1MΩ, 1/4W | Bleed resistor for capacitor discharge | Provides predictable RC timing; high-Z input friendly |

**Performance:**
- Blocks reverse polarity (24V AC applied backwards)
- Discharges C48 and C47 with τ = **10 seconds** (stable, predictable)
- Minimal steady-state current: <1µA leakage @ 3.3V

---

### Stage 4: GPIO Overvoltage Clamping & Filtering

| Component | Value | Function | Notes |
|-----------|-----------|----------|-------|
| **R (GPIO limiting)** | 2.2kΩ, 1/4W | Current limiting to GPIO | Protects pin from inrush current |
| **BZT52C3V3** | Zener ≥400mW | Clamps voltage to exactly 3.3V | Precise regulation vs wide-range TVS |
| **C47** | 100nF, 16V ceramic | Final GPIO high-frequency filter | τ = 1MΩ × 100nF = 100ms |

**Performance for 24V AC peak input:**
- Current through 2.2kΩ at 24V: (24V − 3.3V) / 2.2kΩ = **9.4mA** (safe)
- Power in Zener: 20.7V × 9.4mA ≈ **195mW** (within 400mW rating)
- **GPIO voltage clamped to 3.3V ± 0.2V** (Zener tolerance)

---

## Design Rationale

### Two-Stage Decoupling (C46 + C48)

The 24V AC input contains 60Hz fundamental and harmonics. Two capacitors in series with protection stages provide:
1. **Early attenuation** at the input (C46) before signal conditioning
2. **Final buffering** before GPIO (C48) after protection diodes
3. **Predictable charge/discharge timing** via the 1MΩ resistor

### 1MΩ Bleed Resistor (R79)

ESP32 GPIO inputs are high-impedance (~10MΩ). Without a bleed path:
- Capacitors would hold charge indefinitely
- Timing would be unpredictable and temperature-dependent

The 1MΩ resistor provides:
- Controlled discharge path: τ = 1MΩ × ~10µF ≈ **10 seconds**
- Consistent RC behavior across temperature/tolerance variations
- Negligible DC power: at 3.3V, I = 3.3V / 1MΩ = **3.3µA**

### Current-Limited Zener Clamp (R + BZT52C3V3)

A high-voltage TVS (SMF24/SMF30) would clamp at ~20–30V, still above ESP32 safe voltage. Instead:

- **2.2kΩ resistor** limits inrush: (24V − 3.3V) / 2.2kΩ = 9.4mA (manageable)
- **BZT52C3V3 Zener** clamps at precise **3.3V** (matches GPIO max)
- **Result:** GPIO pin safe from all overvoltage conditions

---

## Protection Features

| Fault Condition | Mechanism | Outcome |
|-----------------|-----------|---------|
| **Reverse 24V AC** | D14 reverse polarity blocking | Blocks reverse current; GPIO protected |
| **24V AC overshoot** | 2.2kΩ + BZT52C3V3 clamping | GPIO held at 3.3V; Zener dissipates power |
| **60Hz AC ripple** | C46 + C48 RC filtering | Attenuated to sub-mV levels |
| **Transient spikes** | D15 (1N4148) fast diode | Clamps to ~0.7V forward bias |
| **DC short to ground** | R5 (33kΩ) current limiting | Limits fault current to <1mA |
| **High-frequency noise** | C47 (100nF) GPIO filter | Attenuates EMI above ~1kHz |

---

## PCB Layout Recommendations

1. **C46 and C48 placement:**
   - Mount C46 close to R5 input junction
   - Mount C48 close to D14 output junction
   - Keep traces short to minimize inductance

2. **GND connections:**
   - All capacitor GND pins tie to main GND plane
   - Use multiple vias for GND return

3. **GPIO pin:**
   - BZT52C3V3 cathode directly to ESP32 pin (minimal trace)
   - C47 cathode to GND plane near GPIO

4. **Thermal:**
   - During 24V overvoltage fault, BZT52C3V3 dissipates ~195mW
   - Ensure adequate copper area around Zener (no thermal island)
   - SMD Zener will reach ~80°C but is rated to 150°C

---

## Bill of Materials (Per Input)

| Qty | Reference | Value | Package | Supplier | Notes |
|-----|-----------|-------|---------|----------|-------|
| 1 | R5 | 33kΩ 1/4W | 0603 | LCSC | Current limiting |
| 1 | C46 | 10µF 50V | 0805 | LCSC | Stage 1 decoupling |
| 1 | D15 | 1N4148W-7-F | SOD-123FL | LCSC | Fast diode |
| 1 | C48 | 10µF 50V | 0805 | LCSC | Stage 2 decoupling |
| 1 | D14 | SMF3.3A | SOD-123FL | LCSC | Reverse polarity TVS |
| 1 | R79 | 1MΩ 1/4W | 0603 | LCSC | Bleed resistor |
| 1 | BZT52C3V3 | Zener 3.3V 400mW | SOD-123 | LCSC | Overvoltage clamp |
| 1 | C47 | 100nF 16V | 0603 | LCSC | GPIO filter |

**Total per input:** ~$0.15 in component cost

---

## Testing & Verification

### Functional Test (DC)
1. Apply **+24V DC** to input → GPIO reads **HIGH**
2. Apply **0V (GND)** → GPIO reads **LOW**
3. Remove input (floating) → GPIO reads **HIGH** (pull-up via R79)
4. Verify no GPIO overvoltage with multimeter (should see ~3.3V)

### Reverse Polarity Test (DC)
1. Reverse input polarity: **−24V** to input, **+24V to GND**
2. GPIO should remain **LOW** (D14 blocks reverse current)
3. No damage or current draw observed

### AC Functional Test (24V AC, 60Hz)
1. Apply **24V AC source** to input
2. GPIO toggles at 60Hz (rectified waveform)
3. Measure GPIO voltage with oscilloscope: **0V to +3.3V peak** (no overshoot)
4. Verify clamping voltage with AC signal applied (BZT52C3V3 should be conducting)

### Thermal Test
1. Apply continuous 24V AC overvoltage to input
2. Measure BZT52C3V3 temperature with IR thermometer
3. Should stabilize <80°C under nominal fault conditions

---

## Design Trade-offs

| Trade-off | Choice | Reason |
|-----------|--------|--------|
| **Single vs. two-stage decoupling** | Two-stage (C46 + C48) | Better 60Hz attenuation; predictable timing |
| **Zener vs. high-voltage TVS** | BZT52C3V3 Zener | Precise 3.3V clamp; no overvoltage to GPIO |
| **Bleed resistor value** | 1MΩ | High-Z friendly; ~10s discharge time; 3.3µA steady-state |
| **Series resistor before Zener** | 2.2kΩ | Limits fault current to 9.4mA; 195mW dissipation acceptable |

---

## Known Limitations

1. **Power dissipation:** Under sustained 24V overvoltage (DC short), the 2.2kΩ resistor and Zener dissipate ~195mW each. This is safe for brief faults but will damage components under prolonged overcurrent (>seconds). **Recommendation:** Use input validation logic to detect and alert on persistent faults.

2. **Temperature drift:** BZT52C3V3 Zener voltage drifts ~0.1%/°C. Under worst-case temperature (−55°C to +150°C), clamp voltage can vary ±0.2V. This is within ESP32 GPIO margin.

3. **AC ripple residual:** At 60Hz input, even with two 10µF caps, some ripple appears on the GPIO after rectification. Software debounce (currently 2s) masks this.

---

## References

- **BZT52C3V3 Zener Datasheet:** Common generic Zener, available from LCSC (C160222)
- **SMF3.3A TVS Datasheet:** See `docs/TVS-SMF3-3-1103.pdf`
- **1N4148W-7-F Diode:** Fast switching diode, 75V reverse rating
- **ESP32-S3 GPIO Specifications:** 3.3V max input, internal ESD protection

---

## Revision History

| Date | Version | Author | Change |
|------|---------|--------|--------|
| 2026-03-22 | 1.0 | Design | Initial circuit finalization for all 4 inputs (LPS, DFT, Y, O) |

