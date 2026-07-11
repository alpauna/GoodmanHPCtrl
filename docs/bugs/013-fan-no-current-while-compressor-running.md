# BUG-013: FAN not moving air while compressor is energized (no airflow safety)

**Date**: 2026-07-11
**Severity**: Critical
**Status**: Fixed
**Affected versions**: v1.0 through v1.3-rc?
**Fixed in**: `f9f836a`

## Symptom

Compressor (CNT) energized while the FAN output was OFF, or FAN output ON
but the fan motor drawing no current (seized bearing, blown capacitor,
disconnected wire, tripped internal thermal). Compressor continued to run
against a static coil, driving head pressure and discharge temperature
rapidly upward with no protective shutdown from the controller. Only the
compressor's own internal overtemp and the LPS switch would (eventually)
save the unit.

Additionally, the `FAN_CURRENT` reading occasionally reported `0.00A`
during normal fan operation, producing false-positive log noise and
undermining any downstream safety logic that trusted the reading.

## Impact

- **Compressor damage risk** — running a heat pump with no condenser (or
  evaporator, in HEAT) airflow is one of the fastest ways to destroy a
  compressor. Head pressure climbs, LPS may not trip on the low side, and
  by the time compressor overtemp (240°F) hits, permanent damage is likely
- Silent failure — no fault, no MQTT event, dashboard shows CNT green
- Current-sensor accuracy problem made a data-driven watchdog untrustworthy
  until the sensor itself was fixed
- Affected every operating mode that runs CNT: HEAT, COOL, and any
  post-defrost restart

## Root Cause

Two independent problems combined into one safety gap:

### 1. Missing CNT⇒FAN invariant and no airflow watchdog

`GoodmanHP` treated FAN and CNT as independently-driven outputs. The
state machine set them correctly on entry to each state, but several
transition paths could leave CNT ON while FAN drifted OFF:

- HEAT→COOL cancel path (mode-change race) toggled RV and re-evaluated
  outputs without re-asserting FAN
- CNT (re)activation after short-cycle timeout only wrote CNT, trusting
  a stale FAN value
- Periodic `updateState()` output validation checked each output against
  the state table independently, so a FAN=OFF glitch would be "validated"
  as correct if the table happened to allow it at that instant

There was **no** check whatsoever that the fan motor was actually drawing
current when the controller believed it was running. A blown run
capacitor, seized bearing, tripped thermal, or broken wire produced zero
protective response.

### 2. `CurrentSensor::readRMS()` false-zero reads

The old RMS implementation had three subtle bugs that combined to emit
`0.00A` reads on a healthy fan channel:

- **Only 20 samples over ~23 ms.** At 60 Hz that's ~1.4 line cycles, and
  I2C jitter from WiFi / AsyncTCP / SD log-flush interrupts routinely
  bunched samples near a zero crossing, producing a near-zero sum-of-squares
- **`sqrt(mean(v²))` with no DC-mean subtraction.** The differential input
  is not perfectly centered on 0 V (ADS1115 offset drift, non-symmetric
  sample windows), so the "RMS" formula was actually computing
  `sqrt(AC² + DC²)` — occasionally producing a spurious near-zero result
  when signed samples happened to cancel
- **All-zero I2C read treated as `0.00A`.** When the ADS1115 NACKed under
  bus contention every sample came back as `0`, and the sensor cheerfully
  published `0.00 A` instead of marking the reading invalid

Any watchdog built on top of this sensor would fire false alarms.

## Fix

### CurrentSensor (`src/CurrentSensor.cpp`)

- Increased sample count from 20 → **60** (~70 ms, ~4 full 60 Hz cycles)
  to average out I2C-induced sample bunching
- Proper AC-RMS: subtract DC mean before squaring
  (`variance = mean(v²) − mean(v)²`, then `sqrt(variance)`)
- Reject all-zero reads as **invalid** (`_valid = false`) with a
  `CURRENT` warn log — never publish a fake `0.00 A`
- Guard against FP round-off producing negative variance

### GoodmanHP airflow invariant

- Every code path that turns CNT ON now also asserts FAN ON in the same
  block — HEAT→COOL cancel path, short-cycle re-activation,
  `updateState()` output validation
- New periodic `checkFanFault()` task inspects `FAN_CURRENT` whenever CNT
  is energized

### GoodmanHP FAN current watchdog (`checkFanFault`)

New protection with three tunable constants in `GoodmanHP.h`:

```cpp
static constexpr float FAN_MIN_RUNNING_AMPS = 0.3f;              // Below this = FAN not running
static const uint32_t FAN_NO_CURRENT_TIMEOUT_MS = 20UL * 1000;   // 20s to see FAN current
static const uint32_t FAN_FAULT_ERROR_MS       = 3UL * 60 * 1000; // 3 min ERROR lockout
static const uint8_t  FAN_LOW_CONSECUTIVE_REQUIRED = 3;          // Debounce
```

Behavior:

- Only armed when CNT is ON (compressor actually needs airflow)
- Requires **3 consecutive** low reads (~3 s at 1 Hz current-sensor
  polling) before starting the 20 s timer — filters single-sample I2C
  glitches
- If FAN current stays below `FAN_MIN_RUNNING_AMPS` for
  `FAN_NO_CURRENT_TIMEOUT_MS` while CNT is on: shut down CNT, latch
  `ERROR` state for `FAN_FAULT_ERROR_MS` (3 min lockout)
- Blocks CNT re-activation while lockout is active
- Auto-recovers after 3 min; watchdog re-arms on the next CNT activation

### Grep-friendly logging

All watchdog events emit `FAN_FAULT:` and `FAN_SAFETY:` tagged lines
including `Y=`, `O=`, `state=`, `amps=`, `pin=`, and `elapsed=` so a
post-mortem grep against the SD log tells the full story of a shutdown.

## Affected Code

- `include/GoodmanHP.h`: watchdog constants, new state fields
  (`_fanFault`, `_fanFaultStartTick`, `_fanNoCurrentStartTick`,
  `_fanLowConsecutive`), `checkFanFault()` declaration, public
  accessors (`isFanFaultActive()`, `getFanFaultRemainingMs()`,
  `clearFanFault()`)
- `src/GoodmanHP.cpp`: `checkFanFault()` implementation, CNT⇒FAN
  invariant asserted at every CNT-on site, task scheduling, richer log
  tags
- `src/CurrentSensor.cpp`: 60-sample RMS with DC-mean subtraction,
  all-zero-read invalidation

## Detection

Field report of `0.00 A` on `FAN_CURRENT` while the fan was audibly
running led to inspecting `CurrentSensor::readRMS()`. Code review of
`GoodmanHP` transitions then revealed CNT-on paths that did not assert
FAN, and the total absence of any current-based airflow check. Bench
verification: forcing FAN OFF via manual override with CNT commanded ON
now shuts CNT down within 20 s and latches ERROR for 3 min; disconnecting
the fan run wire reproduces the same shutdown with FAN output still
electrically ON.

## Lessons Learned

- **Commanding an output ON is not evidence that a load is running.**
  Any safety-critical output that drives a motor needs a current-based
  liveness check, not just a GPIO state
- **RMS is not `sqrt(mean(v²))`** unless you can prove the signal is
  DC-free. On a real ADC with real offset drift, always subtract the
  measured DC mean per sample window
- Sample **enough** line cycles to swamp scheduler jitter. On an ESP32
  with WiFi + AsyncTCP + SD I/O sharing cores, 1–2 cycles is not enough
- **Invalid ≠ zero.** A sensor that can't get a good read must say
  "invalid" — publishing `0.00A` from an I2C NACK poisons every
  downstream safety check
- **Invariants belong at every write site, not just at state entry.**
  "CNT on ⇒ FAN on" has to be re-asserted anywhere CNT is written, or a
  future edit will silently break the invariant
- **Debounce safety watchdogs.** A single 0-A sample is a glitch;
  three in a row is a signal. Require consecutive confirmations before
  arming a timer that will shut down the compressor
