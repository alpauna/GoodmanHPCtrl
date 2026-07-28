# BUG-016: High-ambient-HEAT lockout set state to OFF but left CNT/FAN (and RV) running

**Date**: 2026-07-28
**Severity**: High (compressor kept running against the system's own OFF decision, with no self-correction)
**Status**: Fixed, verified live on bench (`192.168.1.125`); not yet deployed to production
**Affected versions**: `45e2696` (introduced the high-ambient-HEAT lockout) through `8a52a19`
**Fixed in**: pending commit (this session)

## Symptom

On the bench controller, with `Y` active (a heat call) and ambient above the
`highAmbientLockout` threshold (default 70°F), `/state` reported
`"state": "OFF"` while `CNT`, `FAN`, and (transiently) `RV` all showed `true`
— the compressor and fan kept running for minutes with no sign of
self-correcting, even though the state machine had already decided the
system should be off. Reproduced live by dropping `O` while ambient was
above the lockout threshold — exactly the scenario the lockout exists to
prevent from running as HEAT.

`/state` also showed `"stateValidating": true, "stateValidationRemainSec": 0`
holding indefinitely (well past the configured 30s window), instead of
clearing.

## Root Cause

Three independent gaps, all in `src/GoodmanHP.cpp`, that only compound into
a visible problem once something (the high-ambient-HEAT lockout, added in
`45e2696`) can force `_state = State::OFF` while `Y` is still active. Before
that feature existed, `State::OFF` only ever happened with `Y` inactive, so
none of these had a chance to matter — `checkYAndActivateCNT()` never tries
to energize anything when `Y` is off, so a disabled safety net was harmless.

### 1. The OFF-transition never told CNT/FAN to turn off

`updateState()`'s per-transition output-control block (lines ~1088-1189)
explicitly manages RV (`safeRvOff()`) and W for every new state, and CNT+FAN
as the first step of the COOL/HEAT/DEFROST *sequenced* transitions — but
`State::OFF` has no sequenced transition of its own, so nothing in that
block ever told CNT or FAN to turn off when landing in OFF. They simply kept
whatever state they were in from the prior COOL/HEAT run.

### 2. `validateOutputStates()`'s corrective sweep never got a chance to run

That function is the documented safety net — it runs every 10s and has a
table-driven expected-output row for every state, including `OFF`
(`expFAN=0, expCNT=0, expRV=0, expW=0`) with auto-correction. It should have
caught gap #1 within 10 seconds. But it unconditionally skips its entire
check whenever `_stateValidationActive` is true — and that flag is only
ever cleared inside `canTransitionToNormalState()`, which `updateState()`
only calls when *attempting* a transition to HEAT/COOL/DEFROST
(`newState != State::OFF && newState != State::ERROR`). Transitions into
OFF/ERROR are deliberately high-priority and bypass that gate on *entry* —
but `startStateValidation()` still arms the flag on *every* transition,
including into OFF. With `Y` active and ambient staying above threshold,
`updateState()` keeps recomputing `OFF` on every tick and never attempts a
HEAT/COOL/DEFROST transition again, so `canTransitionToNormalState()` is
never called, and the flag — and the safety net it disables — stays stuck
forever.

### 3. `checkYAndActivateCNT()` doesn't know the lockout exists

This is a second, independent control path (runs *before* `updateState()`
in `update()`, `GoodmanHP.cpp:567` vs `570`) that energizes CNT/FAN purely
off `isYActive()`, gated by a hardcoded list of fault flags
(`_lpsFault`, `_lowTemp`, `_fanFault`, etc.) — a list that has no entry for
`_highAmbientHeatLockout`. The Y-edge handler inside the same function also
unconditionally turns FAN on the moment `Y` becomes active (gated only on
`_state != State::DEFROST`). Neither path checks whether the state machine
has actually decided to run. Fixing #1 and #2 alone stops the *steady-state*
symptom but not this: on every Y-active cycle, this function would
re-energize CNT/FAN moments after the OFF-transition or validator turned
them off, fighting the other two fixes indefinitely.

## Fix

- **`updateState()`** (OFF-transition block): added explicit `CNT`/`FAN`
  shutdown when `newState == State::OFF`, matching the pattern already used
  for `W`.
- **`validateOutputStates()`**: added a self-expiry check —
  `_stateValidationActive` now clears itself once `_stateValidationMs` has
  elapsed, independent of whether another transition attempt happens to
  call `canTransitionToNormalState()`. Preserves the original "hold before
  allowing another normal-priority change" semantics (still time-gated
  the same way) while guaranteeing the corrective sweep can't stay
  disabled indefinitely just because the system is parked in OFF/ERROR.
- **`checkYAndActivateCNT()`**: added `_state == State::OFF` to the
  fault-flag blacklist that blocks CNT activation, and added the same
  check to the Y-edge FAN-on trigger. `update()` calls `updateState()`
  immediately after this function on every tick, so `_state` reflects the
  previous tick's authoritative decision — using it as a gate doesn't
  introduce a race against legitimate HEAT/COOL activation, since by the
  time CNT would actually try to activate (after the 30s/5min short-cycle
  timers), `_state` has long since settled to whatever `updateState()`
  actually decided.

## Verification (bench, `192.168.1.125`)

Reproduced live: `Y` active, `O` dropped, ambient 74°F (> 70°F lockout).

| | Before fix | After fix |
|---|---|---|
| `state` | `OFF` | `OFF` |
| `stateValidating` | stuck `true` (4+ min observed) | `false` within 30s, self-clears |
| `CNT` / `FAN` / `RV` | all `true`, indefinitely | all `false`, held steady 40+s across 8 polls |

Deployed via OTA (`/update` + `/apply`); device came back with a clean
`SW_RESET`, `crashBootCount: 0`, `safeMode: false`, all outputs verified OFF
at boot. Log confirmed no fight between `checkYAndActivateCNT()` and the
OFF-transition/validator after the fix — CNT/FAN simply stayed off.

**Not yet deployed to production** (`192.168.0.49`) — bench-only as of this
report.

## Affected Code

- `src/GoodmanHP.cpp`:
  - `updateState()` — added CNT/FAN shutdown block for `newState == State::OFF`
  - `validateOutputStates()` — added `_stateValidationActive` self-expiry
  - `checkYAndActivateCNT()` — added `_state == State::OFF` gate to both the
    CNT-activation fault blacklist and the Y-edge FAN-on trigger

## Lessons Learned

- **A state label and the outputs it implies can drift apart the moment a
  new code path can *force* the label without going through every place
  that reacts to it.** The high-ambient-HEAT lockout only touched
  `updateState()`'s `newState` computation — it had no way to know that two
  other functions (`checkYAndActivateCNT()`, and `updateState()`'s own
  transition-output block) each independently assumed "OFF" was a state
  that could only be reached with `Y` inactive, and hardcoded their safety
  logic around that assumption instead of the general case.
- **A safety net that can be permanently disabled by the exact condition
  it exists to catch is not a safety net.** `validateOutputStates()`'s
  `_stateValidationActive` gate was designed to prevent it from
  double-correcting mid-transition — reasonable in isolation — but nothing
  guaranteed the gate would ever re-open once the system settled into a
  state that never attempts another transition. Any "pause the checker"
  flag needs its own unconditional expiry, not just an expiry that's
  contingent on something else happening to try again.
- **Two independent control paths driving the same output invite exactly
  this kind of bug.** `checkYAndActivateCNT()` and `updateState()` both
  decide when CNT/FAN should run, from different trigger conditions
  (`isYActive()` directly vs. the computed state machine result), and nothing
  enforced that they agree. Gating the independent path on the
  authoritative one's own decision (`_state == State::OFF`) is the fix here,
  but the deeper lesson is to be suspicious whenever the same relay has
  more than one function that can turn it on.
- **Reproduce the exact scenario the user is testing, live, before writing
  up the fix.** The first two fixes alone looked complete on paper and
  would have shipped without catching gap #3 — only re-running the OTA
  cycle and polling `/state` against the same "Y active, O dropped, ambient
  above threshold" test the user was already running caught that CNT/FAN
  were still coming back on.
