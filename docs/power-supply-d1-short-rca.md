# Power Supply Failure RCA: D1 Failed Shorted

**Date**: 2026-07-28
**Severity**: Critical (bench hardware damage; would have been a field failure)
**Status**: Root cause confirmed, fix applied (D1 replaced)
**Board**: Main board, `Board1`, Power page ([schematic](schematics/Goodman-Heatpump-Main-Board.pdf))
**Affected part**: `D1` (`SM4007PL`, half-wave rectifier diode, 24VAC input)

## Symptom

After roughly an hour of bench operation, the power supply stopped working.
`F1` (PTC resettable fuse, `2920L050/90GR`) was hot and throttling current;
`C5` (main reservoir capacitor) was hot. The board had recently been changed
from a bridge rectifier to a single-diode half-wave rectifier, with `COM` and
digital `GND` combined into one ground domain — both deliberate design
changes, not the actual fault (see Investigation).

## Root Cause

**`D1` (the rectifier diode) had failed shorted.** Confirmed with an
out-of-circuit diode check: continuity with a beep, zero drop, in *both*
directions — a hard internal short, not a marginal/leaky part.

With D1 shorted, the circuit provides **no rectification at all**. The raw
bidirectional 24VAC (measured 27.1VAC, ≈38.3V peak) was applied directly to
the `F1`/`C5`/`D3`/LGS5145-VIN node instead of just the intended positive
half-cycles. This explains every observed symptom simultaneously:

- **`C5` hot**: it's a polarized electrolytic capacitor. Reverse voltage is a
  genuinely destructive condition for one (tolerates roughly ~1-1.5V reverse
  before internal breakdown/gassing); with D1 shorted, C5 was getting a full
  negative swing every other half-cycle.
- **`F1` hot/throttling**: carrying full bidirectional AC current instead of
  the modest one-way recharge pulses a working rectifier would produce.
  Consistent with a PTC fuse's self-limiting behavior (heats up, resistance
  rises, throttles current) rather than a classic blown fuse.
- **LGS5145 (buck regulator) died when `F1` was deliberately shorted during
  testing**: with D1 already shorted and F1 also bypassed, there was nothing
  left between the raw ±38V AC and the regulator's VIN pin — trivially
  exceeds its absolute max ratings.
- **Why it took ~1 hour**: consistent with cumulative stress/degradation
  (electrolytic cap damage under sustained reverse bias, TVS/fuse
  self-heating) building toward a hard failure, rather than an instant fault.

D1 itself (`SM4007PL`, `VRRM` = 1000V) failing is **not explained by exceeding
its reverse voltage rating** — see "Theories ruled out" below. Most likely a
manufacturing defect/infant mortality, or cumulative forward-surge-current
fatigue from repeated cold power-cycling during bench debugging (see
`SM4007PL-datasheet.pdf`: capacitive-load current must be derated 20% from
the base rating, and the peak forward surge current rating derates further
with repeated cycles — both point at a capacitor-input rectifier being
harder on this diode than a simple average-current calculation suggests).

## Investigation — Theories Considered and Ruled Out

The path to the actual root cause went through several plausible-looking
theories, each eliminated by working through the actual numbers or a direct
measurement rather than accepting a plausible story. Recorded here because
the ruled-out theories are still useful engineering context, not because
they were wrong to consider.

### 1. Capacitor ripple-current rating mismatch (ruled out)

The board's cap had recently been changed from 50V/1000µF to 63V/1000µF.
Datasheet comparison (`1000uF-50V-AiSHi-RS-datasheet.pdf` vs.
`1000uF-63V-ERG-datasheet.pdf`) showed the 63V part has roughly half the
rated ripple current of the 50V part in the same case size (1380mA vs.
2490-2920mA) — a real, datasheet-confirmed effect, and true in general.
**However**: calculating the buck converter's actual input-side current
(`P_in = P_out/efficiency`, `I_in = P_in/V_in`) for this specific circuit
gives roughly **~100mA** even at generous peak load — nowhere near either
capacitor's rated ripple current. The rating comparison was real but
irrelevant to this failure; neither cap was ever close to its ripple limit.

### 2. Ground loop from combining COM and GND (weakened, then ruled out)

The board was also changed to tie `COM` (24VAC transformer common) directly
to digital `GND`, which was flagged as a possible source of unintended
current via some external earth-reference path. Ruled out once confirmed
the failure occurs **on the bench**, never in real equipment — there is no
other earth-referenced device in an isolated bench 24VAC setup to create a
loop with.

### 3. D3 (SMAJ36CA TVS) sitting too close to its standoff voltage (real effect, not the root cause)

Measuring the actual bench transformer output (27.1VAC, not the nominal 24V
the design assumed) showed the half-wave DC bus peak (~37.3-37.6V) sits
*above* D3's 36V standoff voltage — a real design margin concern, likely
causing some genuine continuous TVS leakage. This is still worth addressing
(see Follow-ups) but does not explain the magnitude of what was observed;
the shorted-D1 finding fully accounts for the symptoms on its own.

### 4. Bridge-vs-single-diode voltage drop difference (real, but small)

A bridge rectifier drops two diode junctions (~1.4V) vs. one (~0.7V) for a
single diode — so the single-diode design's DC bus sits ~0.7V higher for
the same transformer output. Real, but only ~2% of the bus voltage; not
large enough on its own to explain a hard fault.

### 5. D1 reverse-voltage transient exceeding its rating (checked, doesn't fit)

Once D1's failure was confirmed, the natural next question was *why* D1
failed. A transient exceeding `VRRM` (1000V) was considered, but the actual
peak reverse voltage D1 experiences in this capacitor-input half-wave
rectifier is only `~2 × Vpeak(AC) ≈ 2 × 38.3V ≈ 77V` — about 13x margin below
the 1000V rating. An external transient large enough to matter here would
need to be extreme (lightning-coupled surge, major fault feeding the line)
— implausible on an isolated bench 24VAC supply. Forward-surge-current
fatigue from repeated cold power-cycling, or a simple defective part, are
both far more consistent with the actual numbers (see Root Cause).

## Fix

Replace `D1` with a fresh `SM4007PL`. No design change required — the
diode's own ratings have enormous margin (1000V vs. ~77V actual peak
reverse stress) once the part is actually functioning.

## Verification

Pending: reassemble with `F1`, `C5`, `D3`, `D2` restored, power up cold, and
confirm DC bus voltage settles near the expected ~37V (27.1VAC peak minus
one diode drop) with `F1`/`C5` staying at room temperature under sustained
operation.

## Follow-ups (not required for this fix, worth tracking separately)

1. **D3 margin**: confirm whether 27.1VAC is representative under normal
   (loaded) conditions vs. an unloaded/light-load reading, and consider a
   TVS with more standoff margin (e.g. 39V or 43V) if the real transformer
   output runs meaningfully above the nominal 24V the original design
   assumed.
2. **Reservoir cap value**: `docs/power-factor/overview.md` (pre-existing
   design doc, predates this session) already specifies **1000µF/50V** for
   this exact half-wave-rectifier design, with the reasoning fully worked
   out (half-wave needs a larger cap to offset ~2× ripple; 50V is sufficient
   because D3 clamps to ~58V max and the fuse limits sustained fault
   current). The board had drifted to 63V/1000µF, and bench discussion
   during this investigation leaned toward reverting to the *original*
   470µF/63V instead. Worth reconciling which of the three (470µF/63V,
   1000µF/50V, 1000µF/63V) actually gets built, since the 1000µF/50V option
   is what the project's own prior design work already settled on and
   justified.
3. **Architecture divergence**: `docs/power-factor/overview.md` describes a
   4-channel design using a resistor-divider voltage reference on `AIN3`
   (no zero-cross detector), while the actual populated daughter board
   (`SPI-Current-Sensor-Schematic.pdf`, this session's
   `ads131m04-current-sensor-design.md`) uses a 3-CT-channel design with
   `AIN2` dedicated to a zero-cross detector signal instead. These are two
   different power-factor computation strategies (direct instantaneous
   `V×I` integration vs. phase-angle `cos(θ)` estimation) and the
   documentation should be reconciled with whichever one the real hardware
   actually implements before more design work builds on either.

## Lessons Learned

- **A hard component failure can hide behind several plausible-but-wrong
  design-margin stories.** Ripple current, ground loops, TVS margin, and
  diode-drop differences were all real, checkable engineering
  considerations — and all either irrelevant or secondary to a simple
  shorted diode. Each was ruled out by doing the actual calculation or
  taking a direct measurement, not by how plausible the story sounded.
- **"Shorting X to test it" can remove a protection mechanism you didn't
  know was there.** `F1`'s PTC throttling was accidentally protecting the
  downstream buck regulator from the real fault; bypassing it during testing
  removed that protection and let the fault kill the regulator. Worth
  keeping in mind before defeating a fuse/protection device to "test" past
  it — confirm what it might currently be protecting against first.
- **An out-of-circuit component check is definitive; in-circuit reasoning
  from schematic analysis is not.** Everything pointed at design-margin
  explanations until the diode was actually pulled and checked directly.
  When a component is cheap to check and pull, do that before continuing to
  reason about it in-circuit.
- **Check whether a design decision was already made and documented before
  re-deriving it from scratch.** `docs/power-factor/overview.md` already
  contains the correct reservoir-cap sizing rationale (1000µF/50V) and an
  independently-matching GPIO pin assignment for the ADS131M04 SPI bus —
  both cross-validate this session's from-scratch analysis, and the cap
  value discrepancy would have been caught immediately by checking existing
  project docs first.
