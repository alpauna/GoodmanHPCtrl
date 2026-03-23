# 24VAC/240V Triac Output Design

**Document Version:** 1.0
**Date:** 2026-03-23
**Application:** GoodmanHP HVAC Controller (ESP32-S3)
**Outputs Controlled:** LT (24VAC, open-drain), CNT (24VAC), W (24VAC), O-RV/FAN (240V)

---

## Overview

All four output channels use isolated optocoupler drivers with gate resistors and phantom loads. Key design features:

- **Optocoupler isolation** between ESP32 (3.3V) and high-voltage AC circuits
- **Inverted logic** on LT output (normally-closed relay behavior via MOSFET)
- **Double isolation** on 240V FAN circuit (MOSFET + zero-cross optocoupler) for safety
- **Differentiated phantom loads** based on output stage current capacity
- **Gate resistors** for controlled triac gate current limiting
- **Snubber capacitors** for EMI suppression

---

## Circuit Architecture

### Output Stages

| Output | Voltage | Driver IC | Gate Resistor | Logic | Isolation | Current Limit |
|--------|---------|-----------|---|---|---|---|
| **LT** | 24VAC | MOC3041SM | R30 (4.7kΩ phantom) | Inverted (Q1 MOSFET) | Single | ~50mA (opto open-drain) |
| **CNT** | 24VAC | KMOC3021S | R25 (180Ω) | Normal | Single | Higher (external gate drive) |
| **W** | 24VAC | KMOC3021S | R44 (180Ω) | Normal | Single | Higher (external gate drive) |
| **FAN** | 240V | MOC3041SM | R35 (390Ω) | Normal | **Double (Q2 MOSFET)** | Higher + isolation |

---

## Circuit Details

### LT Output (24VAC Open-Drain, Normally-Closed Logic)

**Topology:**
```
IO3 (3.3V) → R22 (180Ω) → Q1 (MOSFET gate)
                             │
                    Q1 inverts signal
                             │
                    U29 (MOC3041SM LED)
                             │
                    Optocoupler output (open-drain)
                             │
           Phantom Load: R30 (4.7kΩ) + C30 (470nF)
                             │
                      External system
```

**Design Rationale:**
- **Inverted logic:** MOSFET Q1 pulls opto LED to ground when IO3 is HIGH, implementing normally-closed relay behavior (HIGH = OFF)
- **Lower current capacity:** MOC3041SM is open-drain output (~50mA max)
- **Lighter phantom load:** C30 = 470nF (not 2.2µF) to avoid overloading the low-current opto output
  - Xc at 60Hz = 5.65kΩ (impedance-matched to low-capacity output)
  - Phantom load: 4.7kΩ || 5.65kΩ ≈ 2.65kΩ effective impedance

**Component Values:**
| Component | Value | Function |
|-----------|-------|----------|
| R22 | 180Ω | GPIO current limiting (3.3V input) |
| R23 | 180Ω | MOSFET source resistor (not clearly visible in schematic) |
| R26 | 10kΩ | MOSFET pull-up to 3.3V |
| Q1 | L2N7002SLLT1G | Logic inverter (HIGH on IO3 = Q1 ON = LED pulled LOW) |
| U29 | MOC3041SM | Zero-cross optocoupler for 24VAC |
| R30 | 4.7kΩ | Phantom load resistor (reduced value due to low output current) |
| C30 | 470nF | Phantom load capacitor (lower value, Xc = 5.65kΩ @ 60Hz) |
| C32 | 100nF | Snubber across 24VAC output |

---

### CNT/W Outputs (24VAC with Gate Resistor Drive)

**Topology:**
```
IO5/IO6 (3.3V) → R43/R46 (180Ω) → U27/U34 (KMOC3021S)
                                      │
                              Triac driver output
                                      │
                  Gate Resistor: R25/R44 (180Ω)
                                      │
                        External triac gate
                                      │
           Phantom Load: R31/R47 (4.7kΩ) + C33/C38 (2.2µF)
```

**Design Rationale:**
- **Higher current capability:** KMOC3021S is optimized for triac gate current (~10-50mA typical)
- **Standard gate resistor:** 180Ω limits gate current to ~26mA @ 5V (safe for triacs)
- **Proper phantom load:** 4.7kΩ + 2.2µF matches design spec
  - Xc at 60Hz = 1.2kΩ (balanced impedance matching)
  - Prevents external triac from floating high when control signal is removed

**Component Values:**
| Component | Value | Function |
|-----------|-------|----------|
| R43/R46 | 180Ω | GPIO current limiting |
| U27/U34 | KMOC3021S | Instantaneous-trigger optocoupler-triac driver |
| R25/R44 | 180Ω | Triac gate current limiting (~26mA @ 5V) |
| R31/R47 | 4.7kΩ | Phantom load resistor |
| C33/C38 | 2.2µF | Phantom load capacitor (Xc = 1.2kΩ @ 60Hz) |
| C31/C37 | 100nF | Snubber across 24VAC output |

---

### FAN Output (240V with Double Isolation)

**Topology:**
```
IO4 (3.3V) → R39 (180Ω) → Q2 (MOSFET gate)
                            │
              Q2 switches opto LED input
                            │
              U31 (MOC3041SM zero-cross)
                            │
              Gate Resistor: R35 (390Ω)
                            │
                  External FAN circuit
                            │
         Phantom Load: R42 (4.7kΩ) + C35 (2.2µF)
         Snubber: C60 (220nF)
```

**Design Rationale:**
- **Double isolation:** MOSFET Q2 provides first barrier, zero-cross optocoupler provides second
- **Higher gate resistor:** R35 = 390Ω (vs 180Ω for 24VAC)
  - Provides additional voltage isolation on the 240V side
  - Reduces gate current to ~10mA @ 5V (conservative for 240V safety)
- **Zero-cross triggering:** MOC3041SM waits for AC zero-crossing before triggering
  - Reduces EMI on 240V circuit (critical at high voltage)
  - Slower response (~10ms) acceptable for FAN application
- **Heavier snubber:** C60 = 220nF (vs 100nF for 24VAC)
  - 240V has higher dV/dt (faster voltage transients)
  - Better EMI suppression needed for main power circuits

**Component Values:**
| Component | Value | Function |
|-----------|-------|----------|
| R39 | 180Ω | GPIO current limiting (3.3V side) |
| Q2 | L2N7002SLLT1G | MOSFET isolation barrier (240V side protection) |
| U31 | MOC3041SM | Zero-cross optocoupler for 240V isolation |
| R35 | 390Ω | Gate resistor with enhanced isolation (~10mA @ 5V) |
| R42 | 4.7kΩ | Phantom load resistor |
| C35 | 2.2µF | Phantom load capacitor (Xc = 1.2kΩ @ 60Hz) |
| C60 | 220nF | **Heavy snubber** for 240V EMI suppression |

---

## Phantom Load Design Philosophy

Phantom loads are sized based on **output stage current capacity**, not a one-size-fits-all approach:

### LT (Low-Capacity Output)
- **Optocoupler open-drain:** ~50mA max
- **Phantom load:** 470nF (Xc = 5.65kΩ) — lighter to avoid overload
- **Total impedance:** ~2.65kΩ effective

### CNT, W, FAN (Higher-Capacity Output)
- **Triac gate drive:** 10-50mA typical
- **Phantom load:** 2.2µF (Xc = 1.2kΩ) — standard impedance matching
- **Total impedance:** ~1.2kΩ effective

**Design Benefit:** Each output stage operates within safe current limits while maintaining proper AC impedance matching for the driven loads.

---

## Gate Resistor Selection

| Circuit | Gate Resistor | Current @ 5V | Voltage Drop | Purpose |
|---------|---|---|---|---|
| **CNT/W (24VAC)** | 180Ω | ~26mA | 4.7V | Standard triac gate limiting |
| **FAN (240V)** | 390Ω | ~10mA | 3.9V | Enhanced isolation, reduced current for safety |

**Design Trade-off:**
- Lower resistance (180Ω) = faster triac triggering, higher gate current
- Higher resistance (390Ω) = slower triggering, lower gate current, safer for 240V isolation
- 240V circuit uses higher resistance for belt-and-suspenders safety

---

## Snubber Capacitor Design

| Circuit | Snubber Value | Xc @ 60Hz | Purpose |
|---------|---|---|---|
| **LT, CNT, W (24VAC)** | 100nF | 26.5kΩ | Standard EMI suppression for 24VAC |
| **FAN (240V)** | 220nF | 12kΩ | **Heavier snubber** for 240V higher dV/dt |

**Rationale:**
- 240V has higher switching transients than 24VAC
- Larger snubber needed to suppress dV/dt-induced noise
- 220nF provides ~2x the capacitance for better high-frequency attenuation

---

## Optocoupler Selection

### MOC3041SM (Zero-Cross, used on LT & FAN)
- **Triggering:** Waits for next AC zero-crossing after LED illumination
- **Benefits:** Reduced EMI, lower capacitive load
- **Drawback:** ~10ms max delay (one-half cycle at 60Hz)
- **Use case:** LT (low-current open-drain), FAN (high-voltage 240V)

### KMOC3021S (Instantaneous, used on CNT & W)
- **Triggering:** Immediate when LED is illuminated
- **Benefits:** Fast response, simple drive
- **Drawback:** Higher EMI potential
- **Use case:** CNT/W (standard 24VAC triac gates, needs fast response)

---

## Test Recommendations

### Static Tests
1. **Verify optocoupler isolation** (100V insulation test)
2. **Measure gate resistor voltage drop** under load
3. **Check phantom load impedance** at 60Hz with oscilloscope

### Dynamic Tests
1. **Measure gate drive pulse width** (should be <2ms for triac latching)
2. **Verify snubber attenuation** with 240V triac switching transients
3. **Check for false triggers** with high-frequency noise injection (EFT test)

### Functional Tests
1. **LT normally-closed logic:** IO3 HIGH = output OFF, IO3 LOW = output ON
2. **CNT/W normal logic:** IO5/IO6 HIGH = output ON
3. **FAN zero-cross triggering:** Measure ~10ms delay on rising edge

---

## Design Trade-offs & Decisions

| Design Choice | Rationale | Alternative Considered |
|---|---|---|
| **Differentiated phantom loads (470nF vs 2.2µF)** | Match output current capacity | Uniform 2.2µF (would overload low-current LT) |
| **Zero-cross triggers (MOC3041SM) on LT & FAN** | Reduce EMI (especially for 240V) | Instantaneous (faster but noisier) |
| **Double isolation on 240V (MOSFET + opto)** | Maximum safety for main power | Single optocoupler (adequate but less safe) |
| **Higher gate resistor on 240V (390Ω vs 180Ω)** | Voltage isolation margin | 180Ω (simpler, but less isolation) |
| **Heavier snubber on 240V (220nF vs 100nF)** | Suppress higher dV/dt | 100nF (simpler, but more EMI) |

---

## References

- **KMOC3021S Datasheet:** Optocoupler-Triac Driver (instantaneous trigger)
- **MOC3041SM Datasheet:** Zero-Cross Optocoupler (zero-crossing trigger)
- **L2N7002SLLT1G Datasheet:** Small-signal MOSFET for logic inversion/isolation
- **Input circuit design:** See `docs/input-circuit-design.md`
- **Output schematic:** See `docs/output-triacs-schematic.pdf`

---

## Revision History

| Date | Version | Author | Change |
|------|---------|--------|--------|
| 2026-03-23 | 1.0 | Design | Initial triac output design documentation; analyzed phantom load differentiation, gate resistor selection, and 240V double-isolation architecture |

