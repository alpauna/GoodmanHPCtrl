# BUG-009: Defrost triggers in OFF state when runtime threshold reached

**Date**: 2026-03-04
**Severity**: Low (self-correcting)
**Status**: Fixed
**Affected versions**: v1.0 through v1.2-rc9
**Fixed in**: `932b969` (v1.2-rc15)

## Symptom

Log at 21:08:38:
```
[INFO ] [HP] Heat runtime 79 min >= 60 min threshold (band=Mid), starting defrost
[INFO ] [HP] Starting defrost transition (band=Mid, minRT=90 s, exit=55.0F, 30 s RV short cycle)
[WARN ] [HP] Y inactive during defrost/exit sequence, all outputs OFF
```

Defrost initiated while system was in OFF state (Y dropped 32 minutes earlier
at 20:36). `startSoftwareDefrost()` set transition flags, then the Y-inactive
safety abort in `checkDefrostNeeded()` immediately cancelled the transition on
the next update cycle. `_softwareDefrost` stayed latched, causing defrost to
resume from Phase 1 when Y reactivated 5 minutes later at 21:13.

## Impact

- No equipment damage — the Y-inactive safety gate (BUG-001 fix) caught the
  invalid state within 500ms and shut everything down
- Defrost ultimately completed successfully at 21:13–21:17 via the
  `_softwareDefrost` resume path
- Unnecessary WARN log entry and one wasted start/abort cycle
- Outputs were already OFF during the abort, so no relay switching occurred
  (CNT, FAN, W all logged `State: 0 Requested State: 0`)

## Root Cause

`checkDefrostNeeded()` in `GoodmanHP.cpp` had **no state guard** before the
runtime threshold check at line 1870. The function was called unconditionally
from `update()` every 500ms regardless of system state.

The execution flow in `update()`:
```
accumulateHeatRuntime()   // line 256 — correctly guards on State::HEAT
updateState()             // line 257 — Y inactive, stays OFF
checkDefrostNeeded()      // line 258 — NO state guard, runtime check fires
```

`accumulateHeatRuntime()` correctly only accumulates when
`_state == State::HEAT && cnt->isOn()`, so runtime wasn't growing during OFF.
However, the accumulated runtime from previous HEAT cycles (79 min across
~6 short thermostat cycles) persisted because:
1. System never entered COOL (which resets runtime)
2. DFT stayed active (so the DFT-off 30s debounce reset never triggered)

When `checkDefrostNeeded()` reached the runtime threshold check, it saw
79 min >= 60 min (Mid band) and called `startSoftwareDefrost()`. This set
`_defrostTransition = true` and `_softwareDefrost = true`. On the next update
cycle (same log second), the Y-inactive check at line 1740 saw the transition
flags with Y inactive and aborted.

```cpp
// Line 1740 — Y-inactive safety abort
if (!yActive && (_defrostExiting || _defrostTransition || _defrostCntPending)) {
    // ... turn off all outputs, clear transition flags
    // _softwareDefrost stays set for resume
    return;
}

// Line 1870 — runtime threshold check (NO state guard)
_activeDefrostBand = selectDefrostBand();
const DefrostBand& band = _defrostBands[(int)_activeDefrostBand];
if (_heatRuntimeMs >= band.runtimeThresholdMs) {
    startSoftwareDefrost();  // BUG: fires in OFF state
}
```

## Fix

Added state guard before the runtime threshold check in `checkDefrostNeeded()`:

```cpp
// Only trigger new defrost while in HEAT state (Y active, compressor running)
if (_state != State::HEAT) return;

// Select band from current ambient temperature
_activeDefrostBand = selectDefrostBand();
const DefrostBand& band = _defrostBands[(int)_activeDefrostBand];
```

The guard is placed after all ongoing defrost/exit transition handling (phases,
Y-abort) so those continue to function regardless of state. Only the new-defrost
trigger is gated.

State::HEAT is sufficient because:
- HEAT requires Y active (that's how we enter HEAT)
- OFF transitions bypass state validation (high priority), so Y drop →
  immediate OFF before next `checkDefrostNeeded()` call
- `updateState()` runs before `checkDefrostNeeded()` in `update()`, so state
  is always current

## Affected Code

- `src/GoodmanHP.cpp`: Added `if (_state != State::HEAT) return;` before
  band selection and runtime threshold check in `checkDefrostNeeded()`

## Timeline (from logs)

```
20:36:26  Y input deactivated, CNT turned off
20:36:26  State: HEAT -> OFF
          ... 32 minutes in OFF state, runtime frozen at 79 min ...
21:08:38  Heat runtime 79 min >= 60 min threshold (band=Mid), starting defrost    ← BUG
21:08:38  Starting defrost transition (band=Mid, minRT=90s, exit=55.0F)
21:08:38  CNT State: 0 Req: 0  (already off — no relay switch)
21:08:38  FAN State: 0 Req: 0  (already off — no relay switch)
21:08:38  Y inactive during defrost/exit sequence, all outputs OFF                ← abort
          ... _softwareDefrost stays latched ...
21:13:23  Y change detected (active), validating in 2000ms
21:13:26  State: OFF -> DEFROST                                                   ← resume
21:13:26  Defrost resuming, restarting transition from Phase 1
21:13:56  Phase 1 complete, engaging RV+W
21:14:26  Phase 2 complete, engaging CNT — defrost fully active
21:16:56  Defrost complete: condenser 60.3F >= 55.0F (band=Mid)
21:16:56  Defrost complete, starting exit transition
21:17:27  Exit Phase 1 complete, RV off
21:17:57  Exit Phase 2 complete, CNT+FAN on — back in HEAT mode
21:18:28  Config saved (defrost=0, runtime=0 ms)
```

## Detection

Discovered via log review — the `[WARN] Y inactive during defrost/exit
sequence` message flagged an unexpected abort. Cross-referencing timestamps
confirmed the system was in OFF state when defrost triggered.

## Lessons Learned

- `checkDefrostNeeded()` handles both ongoing transitions and new triggers
  in the same function. Guards for new triggers (state-dependent) must be
  separate from guards for ongoing transitions (state-independent).
- The defense-in-depth from BUG-001 (Y-inactive safety gate) prevented this
  from becoming a real issue — the abort path worked exactly as designed.
- Heat runtime persisting across thermostat cycles is correct behavior, but
  the threshold check must only fire when the system is actually running.
