# BUG-001: W relay turned ON during defrost Phase 1

**Date**: 2026-03-01
**Severity**: Medium
**Status**: Fixed
**Affected versions**: v1.1 through v1.2-rc6
**Fixed in**: v1.2-rc7

## Symptom

State validator logged:
```
[ERROR] [HP] STATE CHECK: W on in DEFROST Ph1 — forcing OFF
```

W (auxiliary heat) relay was momentarily energized during defrost Phase 1
(pressure equalization), when all outputs should be OFF.

## Impact

- W relay turned ON for up to 10 seconds during Phase 1 before the state
  validator caught and corrected it
- No equipment damage — the state validator auto-corrected the output
- Phase 1 is only 30 seconds, so W was on for a fraction of that window
- Functionally low risk: W energizes the backup heat strips, which is harmless
  during the brief pressure equalization period

## Root Cause

**Race condition in `updateState()` block ordering** (`GoodmanHP.cpp`).

When entering DEFROST from another state (e.g., OFF or HEAT), the W control
block ran BEFORE the Phase 1 restart block:

```
// Block A (ran first): W control
if (newState == DEFROST && !_defrostTransition) {  // _defrostTransition still false!
    w->turnOn();  // BUG: W turned on prematurely
}

// Block B (ran second): Phase 1 setup
if (newState == DEFROST && _softwareDefrost && oldState != DEFROST) {
    _defrostTransition = true;  // Now it's true, but too late
}
```

The `!_defrostTransition` guard on the W control was intended to prevent W
from turning on during Phase 1. However, `_defrostTransition` was only set
to `true` by the Phase 1 restart block, which executed AFTER the W control
block. At the time W was evaluated, `_defrostTransition` was still `false`.

## Fix

Moved the Phase 1 restart block BEFORE the W control block in `updateState()`.
Now `_defrostTransition` is `true` when W is evaluated, and the
`!_defrostTransition` guard correctly prevents W from turning on.

W is properly turned on later by `checkDefrostNeeded()` when Phase 1 completes
and Phase 2 begins (line 1548: RV + W on).

## Affected Code

- `src/GoodmanHP.cpp`: `updateState()` — reordered blocks at ~line 716

## Detection

Caught by the table-driven output state validator (`validateOutputStates()`),
which runs every 10 seconds and checks all outputs against expected values for
the current state and transition phase. The validator auto-corrected W to OFF
and logged the ERROR.

## Timeline (from logs)

```
18:52:11  State check OK: OFF [FAN=OFF CNT=OFF RV=OFF W=OFF]
18:52:41  STATE CHECK: W on in DEFROST Ph1 — forcing OFF     ← BUG
18:53:11  State check OK: DEFROST [FAN=OFF CNT=OFF RV=ON W=ON]  ← Phase 2 (correct)
18:53:41  State check OK: DEFROST [FAN=OFF CNT=ON RV=ON W=ON]   ← Phase 3 (correct)
18:56:11  State check OK: HEAT [FAN=OFF CNT=OFF RV=OFF W=OFF]   ← Exit transition
18:56:41  State check OK: HEAT [FAN=ON CNT=ON RV=OFF W=OFF]     ← Normal HEAT
```
