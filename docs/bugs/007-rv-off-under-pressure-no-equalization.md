# BUG-007: RV relay turned off under pressure without equalization delay

**Date**: 2026-03-02
**Severity**: Medium (equipment protection)
**Status**: Fixed
**Affected versions**: v1.0 through v1.2-rc8
**Fixed in**: v1.2-rc9

## Symptom

When the system transitions from COOL → OFF (or any state where RV and CNT
are both active to OFF), the reversing valve solenoid is de-energized
immediately while the refrigerant system is still under compressor discharge
pressure. The valve attempts to switch position against high-pressure
differential, causing mechanical stress and potential valve damage.

## Impact

- Reversing valve switches under full system pressure on every COOL → OFF
  transition, every Y-drop during defrost Phase 3, every LOW_TEMP activation
  from COOL mode, and several other abort paths
- Repeated pressure-switching can shorten reversing valve lifespan and cause
  the valve to fail to seat properly
- No immediate equipment failure, but cumulative mechanical wear on the
  most expensive single component in the heat pump system

## Root Cause

The existing code had proper pressure equalization sequencing for **planned
mode transitions** (COOL→HEAT, HEAT→COOL, defrost exit) — these all use
multi-phase sequences that wait `_rvShortCycleMs` (default 30s) after CNT
turns off before switching RV.

However, **9 code paths** turned RV off immediately with no delay:

1. `updateState()` — direct transition to OFF or HEAT when no sequenced
   transition applies
2. `checkAmbientTemp()` — LOW_TEMP protection activation
3. Y drop during active defrost (Phase 3, CNT running)
4. Y drop during defrost exit
5. COOL transition abort (Y or O drops mid-transition)
6. HEAT transition abort (Y drops mid-transition)
7. RV fail detection (shuts down defrost)
8. Y inactive safety abort in `checkDefrostNeeded()`
9. `stopSoftwareDefrost()` with Y inactive

All of these called `rv->turnOff()` directly, bypassing pressure
equalization. The common pattern was CNT being turned off in the same
function call (or moments before), followed immediately by RV off — giving
zero time for the refrigerant pressures to equalize.

## Fix

Added `safeRvOff()` helper method that checks CNT timing before switching RV:

```cpp
void GoodmanHP::safeRvOff() {
    OutPin* rv = getOutput("RV");
    if (rv == nullptr || !rv->isOn()) return;

    OutPin* cnt = getOutput("CNT");
    uint32_t cntOffElapsed = millis() - cnt->getOffTick();

    if (!cnt->isOn() && cntOffElapsed >= _rvShortCycleMs) {
        rv->turnOff();  // Pressure equalized — safe
    } else {
        _rvHoldActive = true;  // Hold RV on until equalized
    }
}
```

`checkRvHold()` runs every 500ms in `update()` and turns RV off once CNT
has been off for `_rvShortCycleMs`. The hold auto-cancels if a sequenced
transition takes over RV management (cool/heat transition, defrost phases).

All 9 immediate-off call sites replaced with `safeRvOff()`. Two existing
call sites that were already inside sequenced transitions (HEAT transition
Phase 1→2, defrost exit Phase 1→2) kept as direct `rv->turnOff()` since
they already waited the full equalization period.

Additional safety:
- CNT activation blocked during `_rvHoldActive` (prevents compressor running
  with RV in wrong position)
- Output state validator skips RV enforcement during hold (`expRV = -1`)
- Hold cancelled if any transition flag becomes active

## Affected Code

- `src/GoodmanHP.cpp`: Added `safeRvOff()`, `checkRvHold()`, replaced 9
  `rv->turnOff()` calls
- `include/GoodmanHP.h`: Added `_rvHoldActive` member, method declarations,
  public accessors
- `src/WebHandler.cpp`: Added `rvHold`/`rvHoldRemainSec` to `/state` JSON
  and MQTT state
- `src/HttpsServer.cpp`: Mirrored `rvHold`/`rvHoldRemainSec` in HTTPS
  `/state` handler
- `data/www/dashboard.html`: Added "Hold" pill with countdown on RV indicator
- `data/www/pins.html`: Added "RV Hold (Pressure EQ)" flag
- `src/DisplayManager.cpp`: Added "RV Hold" to OLED protections page
