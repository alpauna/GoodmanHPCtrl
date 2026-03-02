# BUG-006: FAN validator fight during state validation window

**Date**: 2026-03-01
**Severity**: Low
**Status**: Fixed
**Affected versions**: v1.2-rc4 through v1.2-rc7 (state validation feature)
**Fixed in**: v1.2-rc8

## Symptom

Two consecutive STATE CHECK errors within 30 seconds:
```
[ERROR] [HP] STATE CHECK: FAN on in OFF — forcing OFF
[ERROR] [HP] STATE CHECK: FAN off in HEAT — forcing ON
```

The validator forced FAN off (wrong for pending state), then immediately had
to force it back on (right for actual state). Double-correction.

## Impact

- FAN toggled OFF then ON within ~30 seconds
- Validator auto-corrected both times, no equipment damage
- Cosmetic: two ERROR log entries for a transient that would have self-resolved

## Root Cause

**State validation delay** creates a window where the state label doesn't
reflect the actual operating mode.

When LOW_TEMP exits → state=OFF with 30s validation hold. During that window:

1. Y activates → `checkYAndActivateCNT()` turns FAN ON (correct behavior)
2. Validator runs, sees state=OFF → OFF table says FAN=0 → forces FAN OFF
3. Validation completes → state transitions OFF → HEAT
4. Validator runs, sees state=HEAT with CNT=ON → HEAT table says FAN=1 → forces FAN ON

The validator enforced table rules against a state label that was stale
(pending transition). The Y activation logic was correct for the pending
state (HEAT), but the validator only knew about the current label (OFF).

## Fix

Skip output validation during active state validation hold:

```cpp
if (_startupLockout || _manualOverride || _stateValidationActive) return;
```

This is consistent with existing skip logic for startup lockout and manual
override — both are transitional periods where the state label may not match
the actual operating mode. The state machine actively manages outputs during
validation, so table-driven enforcement is unnecessary and counterproductive.

## Timeline (from logs)

```
04:04:08  State check OK: HEAT [FAN=ON CNT=ON]
04:04:09  Low ambient 19.9F < 20.0F — LOW_TEMP active
04:04:09  CNT off, FAN off
04:04:14  Y drops (thermostat)
04:05:29  Ambient 20.2F >= 20.0F — exiting LOW_TEMP → OFF (30s validation)
04:05:36  Y reactivates → FAN ON
04:05:38  STATE CHECK: FAN on in OFF — forcing OFF       ← validator vs state machine
04:06:00  State validation complete: OFF → HEAT
04:06:06  CNT activated
04:06:08  STATE CHECK: FAN off in HEAT — forcing ON       ← fixing its own damage
04:06:38  State check OK: HEAT [FAN=ON CNT=ON]
```

## Affected Code

- `src/GoodmanHP.cpp`: `validateOutputStates()` — added `_stateValidationActive`
  to skip conditions
