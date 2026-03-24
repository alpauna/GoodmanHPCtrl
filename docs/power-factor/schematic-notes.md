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

## Daughter Board Architecture (Power Factor Module)

The power factor monitoring circuit is implemented as a **separate daughter board**
connected to the main board via two isolated headers. This provides optimal
analog/digital separation and noise isolation.

### Board Separation Strategy

**Main Board** (noisy digital domain)
- ESP32-S3 (WiFi, SPI switching)
- Relay triac drivers (24VAC switching)
- Buck converter (1MHz switching)

**Daughter Board** (clean analog domain)
- ADS131M04 ADC
- CT clamp conditioning (3× PJ-3200A-3A jacks)
- Voltage reference divider (24VAC)
- Vbias generation (1.65V divider)
- SCT-013-000 burden resistor sockets (3× socketed, field-swappable)

**Connection:** Two isolated headers carrying only power and SPI signals

### Header 1: Analog Power & Signals

**Purpose:** Power and analog signal conditioning — isolated power domain

```
Connector: [2×3 pinheader or equivalent]

Pin 1: 24VAC (from main board transformer secondary)
Pin 2: 24VAC (return, same as Pin 1 — two conductors for current distribution)
Pin 3: GND (AGND — analog ground, isolated return)
Pin 4: CT_CH1_TIP (from PJ-3200A-3A Jack 1, tip contact)
Pin 5: CT_CH1_RNG (from PJ-3200A-3A Jack 1, ring contact)
Pin 6: CT_CH2_TIP (from PJ-3200A-3A Jack 2, tip contact)
Pin 7: CT_CH2_RNG (from PJ-3200A-3A Jack 2, ring contact)
Pin 8: CT_CH4_TIP (from PJ-3200A-3A Jack 3, tip contact)
Pin 9: CT_CH4_RNG (from PJ-3200A-3A Jack 3, ring contact)
```

**Alternative (lower pin count with internal jacks on daughter board):**
```
If jacks are mounted directly on daughter board (recommended):

Pin 1: 24VAC
Pin 2: 24VAC (return)
Pin 3: AGND (isolated return)
Pin 4: Burden Socket Pin A (Ch1) — optional, if burden headers not on daughter
Pin 5: Burden Socket Pin B (Ch1) — optional
...
```

**Burden Resistor Sockets:**
- Three 2-pin 2.54mm female headers (one per CT channel)
- Mounted directly on daughter board
- Allow field-swappable 1% precision resistors
- No connector needed — socketed resistors stay on daughter board

**Notes:**
- 24VAC has two paths for current distribution (lower impedance)
- AGND is isolated — does NOT connect to main board GND at this header
- CT signal conditioning (filtering, TVS protection) happens on daughter board
- This header carries only low-frequency signals (60Hz AC, <1kHz)

### Header 2: SPI Digital Communication

**Purpose:** Digital SPI bus only — isolated digital return path

```
Connector: [2×3 pinheader or equivalent]

Pin 1: 3.3V VDD (power for ADC DVDD and pull-up resistors)
Pin 2: SCLK (from ESP32 GPIO 1)
Pin 3: DIN / MOSI (from ESP32 GPIO 2)
Pin 4: /CS (from ESP32 GPIO 45)
Pin 5: /DRDY (to ESP32 GPIO 46, falling-edge interrupt)
Pin 6: DGND (isolated digital ground — separate return path)
```

**Notes:**
- DGND at this header is **NOT connected to main board DGND**
- Provides isolated return path for SPI currents
- Star grounding on daughter board merges AGND and DGND under ADS131M04
- All SPI pull-up/pull-down resistors on daughter board
- Traces from this header to ADC are short and isolated

### Isolation Benefit

By using two separate headers:

1. **Header 1 (Power):**
   - Carries 24VAC and isolated AGND
   - No digital switching noise injected
   - Clean analog power domain

2. **Header 2 (SPI):**
   - Carries only digital SPI signals (low current)
   - Separate DGND return path isolated from main board digital noise
   - SCLK switching confined to daughter board local loops

3. **Isolation between headers:**
   - Single star point connection under ADS131M04
   - Minimizes coupling between noisy digital and sensitive analog

### PCB Layout on Daughter Board

- **Header 1 location:** Near CT jack connectors and voltage reference circuit
- **Header 2 location:** Opposite corner from header 1 (maximizes separation)
- **AGND island:** Extends from header 1 to ADC and all analog circuits
- **DGND return:** Isolated star connection under ADC from header 2
- **SPI traces:** Route away from analog signal region (minimum 30mm)

### Mechanical Mounting

Suggested mounting scheme:
- Two mounting posts (one near each header) to keep daughter board parallel
- 2-5mm standoffs to prevent shorts
- Keying notches on headers to prevent misconnection

### Testing & Debug

With isolated headers:
1. Daughter board can be tested standalone (apply 24VAC + 3.3V to headers)
2. Main board can be tested independently (no analog circuits affected)
3. If ADC noise issues arise: daughter board can be replaced or reworked without disturbing main board
4. Easier to isolate EMI sources (24VAC switching vs ESP32 noise)

---

## SPI Bus Wiring Summary (Main Board)

```
ESP32-S3              Header 2 (Daughter Board)
────────              ──────────────────────────
GPIO 1  ────────────→ SCLK
GPIO 2  ────────────→ DIN  (MOSI)
(GPIO 42 not used — daughter board provides DOUT internally)
GPIO 45 ────────────→ /CS
GPIO 46 ←──────────── /DRDY

3.3V    ────────────→ VDD
GND     ────────────→ DGND (isolated digital return)
```

**Notes:**
- DGND return at Header 2 is isolated from main board GND
- No analog signals cross the bridge (only SPI + isolated DGND)
- SCLK speed: 10–25 MHz (ADS131M04 supports up to 25 MHz)
- /DRDY interrupt: Configure GPIO 46 as falling-edge (data ready)

## VBIAS Reference Generation

The VBIAS reference is generated from the **ADS131M04's internal 1.2V voltage reference**
using an OP07 precision op-amp configured as a non-inverting amplifier with gain = 1.375.

**Circuit topology:**

```
ADS131M04 CAP pin (1.2V internal ref)
    │
    ├─── 220nF ── AGND  (required per ADS131M04 datasheet)
    │
    └─── [OP07 pin 3 (+input)]
         │
         │     ┌────── Rf: 15kΩ ──────┐
         │     │                      │
    ┌────┤+3V  OP07      [Out] ────┬──┴──► VBIAS = 1.65V
    │    │     (pins 8,4)  │       │
    │    │                 │       ├─ C (10µF) ── AGND
    │    ├─ Rg: 40kΩ ─ AGND
    │    │
    │    └─ Ref: AGND
    │
3.3V ────┴──► OP07 pin 8 (V+ supply)
        100nF bypass cap ── AGND

Gain = 1 + Rf/Rg = 1 + 15k/40k = 1.375
VBIAS_out = 1.2V × 1.375 = 1.65V
```

**Component specifications:**

| Component | Value | Function |
|-----------|-------|----------|
| U_ref | OP07 | Ultra-low offset op-amp (Vos ±0.3mV) |
| Rf | 15kΩ 1% metal film | Feedback resistor for gain setting |
| Rg | 40kΩ 1% metal film | Ground reference resistor |
| C_bypass (OP07 pin 8) | 100nF ceramic 0603 | Power supply bypass |
| C_out | 10µF ceramic 0805 | Output load capacitor |
| CAP decoupling | 220nF ceramic 0603 | Required by ADS131M04 datasheet |

**Advantages over external voltage divider:**

1. **Temperature matching:** VBIAS inherits the ADC's internal reference temperature drift
   (20 ppm/°C). Both drift together → no relative error.

2. **Superior accuracy:** ±0.1% internal reference vs ±1-2% supply-dependent divider

3. **Ultra-low offset:** OP07 Vos ±0.3mV ensures accurate 1.65V ± 3mV output

4. **Low noise:** OP07 noise floor (25 nV/√Hz) provides clean VBIAS to all bias networks

5. **No supply noise:** Reference is internal to ADC, not coupled through 3.3V rail

6. **Full ADC range utilization:** ±1.2V input range fully exploited (vs ±0.6V if using 1.2V directly)

**Power supply requirements:**

- OP07 single-supply mode: +3.3V (pin 8) and AGND (pin 4)
- Must add 100nF bypass capacitor on +3.3V near pin 8
- OP07 can swing output close to rails (within ~0.2V of supplies)
- Output swing: 0V to 3.3V, sufficient for 1.65V VBIAS

## Voltage Reference Input (Ch3)

The 24VAC from the furnace transformer secondary is used as the phase-true voltage reference
for power factor angle measurement. This signal is attenuated to match the ADC input range via
a 1:34 voltage divider and TVS protection.

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
- **Phase accuracy**: Resistive divider introduces no phase shift. The voltage sample is
  phase-true to the actual line voltage at the AC unit input.

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

## Analog Power Supply (AVDD)

The ADS131M04 analog supply can operate at either 3.3V or 5V (rated 2.4–5.5V).
**Recommended: 3.3V** from the existing digital LDO (U22 TLV76133DCYR).

**Why 3.3V:**
- Input signal range ±1.2V is supply-independent (internal reference spec)
- Voltage reference (24VAC → 0.71V RMS, 1.0V peak) fits ±1.2V ✓
- CT clamp signals (1.65V ± 1.0V) fit ±1.2V ✓
- Single clean supply (LDO is cleaner than ferrite bead filtering)
- Lower power draw (~20mA vs 30mA @ 5V)
- Simpler schematic (no separate 5V rail needed)

```
3.3V LDO ──┬── AVDD (ADS131M04)
(U22)      │
           ├── 10μF (bulk bypass)
           ├── 100nF (high-frequency bypass)
           │
          GND
```

**Component placement:**
- 100nF bypass cap within 5mm of AVDD pin (high-frequency decoupling)
- 10μF bulk cap nearby (transient response)
- Single ground tie point under the ADC (star ground)

**Signal voltage specifications (unchanged):**
- Voltage reference Ch3: 0.71V RMS (1.0V peak) — within ±1.2V ✓
- CT clamp Ch1/2/4: biased 1.65V ± 1.0V swing — within ±1.2V ✓
- Input PGA = 1x, differential mode (±1.2V full-scale input range)

## 3.3V Supply (U22 TLV76133DCYR) — Serves Both Digital and Analog

The single 3.3V LDO output feeds both the ESP32 digital supply (DVDD) and the
ADS131M04 analog supply (AVDD). This creates a single, clean 3.3V domain
across the entire system.

**Two recommended changes to the 3.3V LDO section:**

**1. Increase C12 from 100nF to at least 1μF (recommend 10μF)**

The TLV76133 datasheet specifies 1μF minimum output capacitance for
regulator stability. At 100nF the LDO can oscillate under load transients.
With the ESP32-S3 drawing 300–500mA spikes during WiFi TX, a 10μF output
cap provides better transient response and keeps the regulator stable.

**2. Add bulk capacitance (22–47μF) on 3.3V near the ESP32**

WiFi TX bursts cause sharp current spikes that sag the 3.3V rail. Without
bulk capacitance close to the ESP32 power pins, the voltage can dip below
the brownout detector threshold (~3.0V) causing spurious resets. A 22μF or
47μF ceramic cap placed as close as possible to the ESP32 VDD pins absorbs
these transients.

```
Current:   5V → U22 → C12 (100nF) → 3.3V

Suggested: 5V → U22 → C12 (10μF) → 3.3V ──┬── DVDD (ESP32)
                                           │
                                           ├── C_bulk (22-47μF near ESP32)
                                           │
                                           └── AVDD (ADS131M04)
```

**Benefit of unified 3.3V:**
- Single clean supply simplifies noise management
- No need for separate analog/digital supply rails
- Reduces component count and PCB routing complexity
- LDO PSRR provides excellent ripple rejection for both digital and analog

Schematic: `docs/schematics/SCH_Schematic1_2-Power_2026-03-22.pdf`

## PCB Layout Considerations

### Ground Plane Separation (Critical for 24-bit ADC SNR)

**Why separate grounds:**
- ESP32-S3 WiFi/SPI switching injects noise into shared ground plane
- ADS131M04 (24-bit) is extremely sensitive to ground noise
- Unshielded digital return currents couple into analog signal paths
- Result: degraded SNR, noisy CT clamp readings, poor power factor accuracy

**Recommended stackup (4-layer minimum):**
```
Layer 1: Signal + Power (top)
Layer 2: AGND (analog ground plane, isolated region)
Layer 3: DGND (digital ground plane, full coverage)
Layer 4: Signal (bottom)
```

**Ground plane layout:**

```
┌─────────────────────────────────────────┐
│  CT Clamps │ Voltage Ref │ AGND Island  │
│             (left/analog region)         │
├──────────────────────────────────────────┤
│                                          │
│         ADS131M04 (★ star point)        │
│                                          │
│              ★ AGND ↔ DGND              │  ← Single via
│                                          │     (0Ω jumper
├──────────────────────────────────────────┤     or direct)
│  ESP32-S3 │ SPI Bus │ DGND (full)       │
│        (right/digital region)            │
└──────────────────────────────────────────┘
```

**Ground plane implementation:**

1. **AGND Island (Layer 2):**
   - Isolated copper region under ADS131M04 and CT circuits
   - Includes: ADC, CT jacks, voltage reference circuit, Vbias divider
   - Surrounds ADS131M04 with at least 10mm border (no digital traces/vias)
   - **Single via connection** to DGND directly under ADC

2. **DGND Plane (Layer 3):**
   - Full continuous copper plane
   - Provides return path for all digital circuits (ESP32, SPI, buck converter)
   - High copper density = low impedance for return currents

3. **Star Ground Connection:**
   - **Location:** Directly under ADS131M04 center
   - **Method:** Single via (not a trace bridge) or 0Ω jumper resistor
   - **Size:** At least one 12mil via (or two 10mil vias in parallel)
   - **Purpose:** Merges AGND island and DGND at the point of highest analog sensitivity

### Signal Trace Routing

**Analog signal traces (AGND reference):**
- AINxP/AINxN differential pairs: routed on Layer 1, return on AGND plane
- Matched length ±5mm tolerance (balance capacitive coupling)
- Trace width: 10mil minimum
- Keep away from switching signals (ESP32 GPIO, SPI clock)
- Spacing from digital traces: ≥15mil minimum

**Voltage reference trace (24VAC divider):**
- Route directly from transformer tap to AGND plane
- Minimize stub length before 33kΩ resistor
- Return current path on AGND only (never cross to DGND)

**SPI bus traces (DGND reference):**
- SCLK, DIN, DOUT, /CS routed on Layer 1, return on DGND plane
- Keep away from analog signal region (minimum 25mm separation)
- Ground vias near SPI connector (fast return path)
- Trace width: 8mil acceptable for SPI speeds (<25MHz)

**Power traces:**
- AVDD: routed on Layer 1, return on AGND via short vias to AGND island
- DVDD: routed on Layer 1, return on DGND via short vias to DGND plane
- Keep AVDD and DVDD traces separated (no parallel running)

### Decoupling Capacitor Placement

**ADS131M04 bypass (most critical):**
```
        3.3V
          │
       ┌──┴──┐
       │10μF │  ← 5mm from AVDD pin
       └──┬──┘
          │
          ├──┬──┐
          │100nF│  ← 3mm from AVDD pin (HF filter)
          └──┴──┘
          │
        AGND
```
- 10μF bulk cap: 5mm away from AVDD pin
- 100nF HF cap: 3mm away from AVDD pin (closer for higher frequency response)
- Both return directly to AGND via short vias (<5mm)
- No shared return traces with digital circuits

**ESP32-S3 bulk capacitance:**
- 22–47μF ceramic as close as possible to VDD pins (<10mm)
- Returns to DGND via short vias
- Placed on opposite corner of board from ADS131M04 (isolates noise)

**Vbias divider capacitors (CT clamp biasing):**
- 10μF + 100nF placed adjacent to ADC input pins
- Both return to AGND (not DGND)
- Minimize trace length to AINxP/AINxN inputs

### Via Strategy

**AGND vias (to analog island):**
- Use multiple smaller vias (10mil) rather than one large via
- Spacing: ≤15mm between vias
- Cluster vias near component grounds (capacitor pads, ADC pins)
- **Never use AGND vias for digital signal returns**

**DGND vias (to digital plane):**
- Use 12–15mil vias for return currents (lower impedance)
- Place close to component pins (SPI, ESP32)
- Spacing: ≤10mm between vias under high-current areas
- Allow high-current return paths (WiFi TX spikes)

**Star point via:**
- Central location under ADS131M04
- Size: 12mil minimum (or two 10mil in parallel)
- Connects AGND island to DGND plane
- No other vias allowed in this region (exclusive connection point)

### Component Placement

**Critical region (analog island):**
- ADS131M04 at center
- CT jack connectors within 50mm
- Voltage reference circuit (33kΩ, divider, capacitors) within 30mm of ADC
- Vbias divider within 20mm of ADC
- TVS protection devices (PESD3V3L2BT) directly at input pins

**Isolation zones:**
- Keep all digital logic >50mm from ADS131M04
- ESP32-S3 on opposite corner of board
- SPI bus routed away from analog region
- 24VAC power traces: ≥30mm from analog circuits

### Layer Stackup Reference (4-layer example)

```
Layer 1 (top):      Analog signals | Digital signals | Power
Layer 2 (inner 1):  AGND island + star point ↔ DGND
Layer 3 (inner 2):  DGND continuous plane + DVDD pour
Layer 4 (bottom):   Return signals | ground vias
```

**Via requirements by layer:**
- L1→L2: AGND vias (analog signals) — use 10mil, ≤15mm spacing
- L1→L3: DGND vias (digital signals) — use 12mil, ≤10mm spacing
- L2↔L3: Star point via (one location under ADC) — use 12mil min
- L4: Back-side ground vias where needed for return paths

### Grounding Checklist

- [ ] AGND and DGND planes created and separated on Layer 2/3
- [ ] Star point via placed directly under ADS131M04
- [ ] AGND island isolated with ≥10mm border, no digital traces
- [ ] CT differential pairs routed with matched length ±5mm
- [ ] 10μF + 100nF bypass on AVDD within 3–5mm of pin
- [ ] ADS131M04 surrounded by AGND vias (≤15mm spacing)
- [ ] ESP32-S3 on opposite side of board from ADS131M04
- [ ] SPI traces >25mm away from analog signal traces
- [ ] Vbias capacitors return to AGND only
- [ ] All CT clamp signal returns on AGND (never cross to DGND)
- [ ] Power traces (AVDD/DVDD) kept separate, both short to respective ground
- [ ] No digital return currents flowing through AGND traces
