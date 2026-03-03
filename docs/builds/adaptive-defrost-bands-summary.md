# Adaptive Defrost Bands Based on Ambient Temperature

**Date:** 2026-03-03

## Overview

Replaced the single defrost runtime threshold (default 90 min) with 3 ambient temperature bands — each with its own runtime threshold, minimum defrost time, and condenser exit temp. Colder ambient temps cause faster ice buildup, so the Cold band triggers defrost sooner with a lower exit temp.

## Band Defaults

| Band | Ambient Range | Runtime Threshold | Min Defrost | Exit Temp |
|------|--------------|-------------------|-------------|-----------|
| **Cold** | ≤ 23°F | 30 min | 2 min | 50°F |
| **Mid** | 23–31°F | 60 min | 1.5 min | 55°F |
| **Warm** | ≥ 31°F | 90 min | 1 min | 60°F |

All values configurable via config page. Two breakpoint temperatures (`coldMaxTemp`, `warmMinTemp`) define the band boundaries.

## Files Modified (11 files)

| File | Changes |
|------|---------|
| `include/Config.h` | Replaced 3 single defrost fields with 11 band fields (2 breakpoints + 3x3 band params) in `ProjectInfo` |
| `include/GoodmanHP.h` | Added `DefrostBandName` enum, `DefrostBand` struct, replaced old member vars/getters/setters with band-aware versions, added `selectDefrostBand()` |
| `src/GoodmanHP.cpp` | Constructor inits band array, `selectDefrostBand()` reads ambient temp and returns band, `checkDefrostNeeded()` uses band-specific thresholds, `startSoftwareDefrost()` snapshots band, exit criteria use snapshot, `restoreSoftwareDefrost()` snapshots on boot, `forceDefrost()` selects band |
| `src/Config.cpp` | `loadTempConfig()`: migration from old single-value fields to bands, load band sub-objects. `saveConfiguration()` + `updateConfig()`: write band structure, remove old keys |
| `src/main.cpp` | ProjectInfo literal: 3 values → 11 band values. Boot: 3 `set*` calls → `setColdMaxTempF`, `setWarmMinTempF`, 3x `setDefrostBand` |
| `src/WebHandler.cpp` | Config GET: 13 band fields. Config POST: parse/validate with lambda. `/state`: `defrostBand` + `defrostBandThresholdMin` |
| `src/HttpsServer.cpp` | Mirrored all WebHandler changes for config GET/POST and `/state` |
| `src/MQTTHandler.cpp` | Added `defrostBand` and `defrostBandThresholdMin` to MQTT state payload |
| `src/DisplayManager.cpp` | Heat runtime line: `HRT: 42/60m (Mid)` format |
| `data/www/config.html` | Replaced Defrost Controls with band table (breakpoints + 3-row table), `updateBandRanges()` for live range labels |
| `data/www/dashboard.html` | Heat runtime: `Heat RT: 42m / 60m (Mid)` format |
| `data/www/pins.html` | Heat runtime flag: includes band threshold and name |

## Key Design Decisions

- **Band snapshot on defrost start**: Parameters are frozen when defrost begins, preventing mid-cycle parameter jumps if ambient drifts
- **Backward compatibility**: Old single-value configs auto-migrate (Warm = old values, Mid/Cold derived proportionally)
- **No ambient data = Warm band**: Most conservative (longest runtime threshold before defrost)
- **Validation rules**: Breakpoints enforce gap (`coldMax < warmMin`), runtime 1–120 min, min defrost 30–600 sec, exit temp 30–120°F

## Config JSON Structure

```json
"defrost": {
    "active": false,
    "coldMaxTemp": 23.0,
    "warmMinTemp": 31.0,
    "cold": { "runtimeThresholdMs": 1800000, "minRuntimeMs": 120000, "exitTempF": 50.0 },
    "mid":  { "runtimeThresholdMs": 3600000, "minRuntimeMs": 90000, "exitTempF": 55.0 },
    "warm": { "runtimeThresholdMs": 5400000, "minRuntimeMs": 60000, "exitTempF": 60.0 }
}
```

## API Changes

### `/state` JSON (new fields)
- `defrostBand` — active band name: `"Cold"`, `"Mid"`, or `"Warm"`
- `defrostBandThresholdMin` — active band's runtime threshold in minutes

### Config GET/POST (replaced fields)
Removed: `defrostMinRuntimeSec`, `defrostExitTempF`, `heatRuntimeThresholdMin`

Added:
- `defrostColdMaxTemp`, `defrostWarmMinTemp` — breakpoint temperatures
- `defrostColdRuntimeMin`, `defrostColdMinRuntimeSec`, `defrostColdExitTempF` — Cold band
- `defrostMidRuntimeMin`, `defrostMidMinRuntimeSec`, `defrostMidExitTempF` — Mid band
- `defrostWarmRuntimeMin`, `defrostWarmMinRuntimeSec`, `defrostWarmExitTempF` — Warm band

## Migration

On config load, if old single-value fields exist (`heatRuntimeThresholdMs`, `minRuntimeMs`, `exitTempF`) and new `cold` sub-object doesn't exist:
- **Warm band** = old runtime, min defrost ×⅓, old exit temp
- **Mid band** = runtime ×⅔, min defrost ×½, exit temp −5°F
- **Cold band** = runtime ×⅓, min defrost ×⅔, exit temp −10°F

Old keys are removed on next `updateConfig()` save.

## Verification Steps

1. `pio run -e freenove_esp32_s3_wroom` — clean build, no warnings
2. OTA upload to device, upload config.html via LittleFS
3. Config page: verify band table loads with correct defaults, breakpoint changes update range labels
4. Save config → verify JSON on SD card has new band structure
5. Dashboard: verify `Heat RT: Xm / Ym (Band)` format with correct active band
6. Test band switching: use failover test to simulate different ambient temps, verify band changes
7. Migration: device with old single-value config should auto-migrate to bands on first load
