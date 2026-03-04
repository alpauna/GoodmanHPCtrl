# BUG-010: Defrost band display stuck on Warm in non-HEAT states

**Date**: 2026-03-04
**Severity**: Low (cosmetic)
**Status**: Fixed
**Affected versions**: v1.2-rc15
**Fixed in**: `fd030e4` (v1.2-rc16)

## Symptom

Dashboard and `/state` JSON report `defrostBand: "Warm"` with 90-minute
threshold while ambient temperature is 30°F — should show Mid band with
60-minute threshold. The band snaps to the correct value when the system
enters HEAT mode.

## Impact

- Cosmetic only — dashboard and OLED show wrong band and threshold in
  OFF and COOL states
- No effect on defrost behavior — `selectDefrostBand()` is called again
  when defrost actually triggers, always in HEAT mode
- Confusing to operators monitoring the system during idle periods

## Root Cause

Regression introduced by the BUG-009 fix (`932b969`). The `State::HEAT`
guard added to prevent defrost triggering in OFF state was placed **above**
the `selectDefrostBand()` call:

```cpp
// BUG-009 fix — but band update was below the guard
if (_state != State::HEAT) return;

// This only runs in HEAT now
_activeDefrostBand = selectDefrostBand();
```

`_activeDefrostBand` is initialized to `DefrostBandName::WARM` in the
constructor (line 22). With the guard in place, it was never updated
outside of HEAT mode, so the dashboard always showed Warm/90min when
the system was OFF or in COOL.

Prior to BUG-009, `selectDefrostBand()` ran unconditionally every 500ms
in `checkDefrostNeeded()`, keeping the display current in all states.

## Fix

Moved `selectDefrostBand()` above the HEAT guard so the band updates
in all states while the runtime threshold check remains gated:

```cpp
// Always update active band for dashboard display
_activeDefrostBand = selectDefrostBand();

// Only trigger new defrost while in HEAT state
if (_state != State::HEAT) return;
```

`selectDefrostBand()` is read-only (reads ambient sensor, returns enum)
and safe to call in any state.

## Affected Code

- `src/GoodmanHP.cpp`: Reordered `selectDefrostBand()` and HEAT guard
  in `checkDefrostNeeded()`

## Detection

Noticed via dashboard — ambient was 30°F but band displayed as Warm
instead of Mid immediately after deploying v1.2-rc15 (BUG-009 fix).

## Lessons Learned

- When adding early-return guards to a function, audit what else the
  function updates that may be needed regardless of the guard condition.
  `_activeDefrostBand` was a display-facing side effect that needed to
  survive the guard.
