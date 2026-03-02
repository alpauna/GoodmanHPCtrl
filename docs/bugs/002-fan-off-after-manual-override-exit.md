# BUG-002: FAN not restored after manual override exit

**Date**: 2026-03-01
**Severity**: Low
**Status**: Fixed
**Affected versions**: v1.1 through v1.2-rc7
**Fixed in**: v1.2-rc8

## Symptom

State validator logged:
```
[ERROR] [HP] STATE CHECK: FAN off in HEAT — forcing ON
```

After disabling manual override, FAN remained OFF while in HEAT mode with
CNT active. The state validator caught and corrected it.

## Impact

- FAN was off for up to 10 seconds (one validator cycle) after override exit
- Validator auto-corrected, no equipment damage
- Brief period of compressor running without condenser airflow — minimal risk
  given the short duration

## Root Cause

When manual override exits (`setManualOverride(false)`), all outputs are
turned OFF and `_cntActivated` is reset. However, `_yWasActive` was NOT
reset. Since Y was still active the entire time, the Y activation edge
detection in `checkYAndActivateCNT()` never re-fired:

```cpp
if (yActive && !_yWasActive) {   // _yWasActive still true — edge never fires
    fan->turnOn();                // FAN never restored
}
```

CNT was immediately reactivated (30s already elapsed from original Y
activation), but FAN stayed off because only the Y activation edge turns
FAN on.

## Fix

Reset `_yWasActive = false` when manual override exits. This causes the
Y activation edge to fire naturally in the next `update()` cycle, turning
FAN on and restarting the normal activation sequence.

## Timeline (from logs)

```
04:09:33  MANUAL OVERRIDE enabled
04:09:54  Manual toggle: RV ON, W ON
04:10:06  MANUAL OVERRIDE disabled, all outputs OFF
04:10:06  Y active for 30s, CNT activated           ← CNT restored
04:10:06  STATE CHECK: FAN off in HEAT — forcing ON  ← FAN was NOT restored
04:10:36  State check OK: HEAT [FAN=ON CNT=ON]       ← validator fixed it
```

## Affected Code

- `src/GoodmanHP.cpp`: `setManualOverride()` — added `_yWasActive = false`
