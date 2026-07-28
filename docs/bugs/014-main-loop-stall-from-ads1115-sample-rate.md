# BUG-014: Main loop stalled ~1s per pass — ADS1115 defaulted to 128 SPS instead of 860 SPS

**Date**: 2026-07-27
**Severity**: High
**Status**: Fixed
**Affected versions**: `f9f836a` through `45e2696` (~16 days in production)
**Fixed in**: `9870f65`

## Symptom

The dashboard's "CPU 0" and "CPU 1" tiles always read `0%`, regardless of
system state or load. Investigating that cosmetic issue led to a much more
serious discovery: the Arduino main loop (`loop()` in `src/main.cpp`) was
completing only **~1 iteration per second**, instead of the several hundred
per second a cooperative-scheduler design like this expects.

## Impact

Every task driven off `loop()` → `ts.execute()` was running far behind its
configured cadence for the entire ~16 days this was live:

- `_tGetInputs` (500ms input-debounce validation) — running at roughly half
  its intended rate
- CNT short-cycle protection, defrost phase transition timers (`_rvShortCycleMs`,
  `_cntShortCycleMs`) — all timed off `millis()` checks inside tasks that
  could only be evaluated once a second instead of continuously
- WebSocket log streaming, HTTP/API responsiveness — degraded, since the
  main loop (not AsyncTCP) is where scheduled work executes
- Masked entirely from monitoring, because the CPU-load metric that should
  have shown this (a pegged-high core 1) was itself broken and always
  reported `0%` — see Root Cause #1

No safety fault fired because nothing in the state machine checks its own
scheduling latency; the system kept functioning, just with much coarser
timing precision than intended on every sub-second protection.

## Root Cause

Two independent problems, discovered in sequence:

### 1. CPU load metric always read 0% (masked the real bug)

`main.cpp` measured per-core load via FreeRTOS idle hooks: accumulate
microseconds between consecutive idle-task invocations, `load = 100 - idle%`.
This had already been "fixed" twice before (see git history on
`idleHookCore0/1`) — an earlier version starved the idle hooks under load
(read 90%+ always), and the fix for that added an unconditional
`vTaskDelay(1)` at the end of every `loop()` pass specifically so idle hooks
could fire on both cores.

That delay overcorrected: since real work per pass is normally much shorter
than one FreeRTOS tick, the forced 1-tick sleep dominated the loop period.
The idle-hook accumulator couldn't distinguish "genuinely idle" from "idle
because we just forced ourselves to sleep," so it pegged at the 0%-load
reading (100% "idle") every single second — including during this incident,
when the loop was in fact almost fully consumed by real work.

### 2. `CurrentSensor::readRMS()` blocked the loop for ~900ms/sec

`src/CurrentSensor.cpp` samples the ADS1115 60 times per current sensor to
compute RMS current (`COMPRESSOR_CURRENT`, `FAN_CURRENT` — 2 sensors on this
deployment), called once per second via `tReadCurrent`. The code's own
comment claimed:

> "ADS1115 at 860 SPS in continuous mode gives ~1.16ms per sample"

but `readRMS()` never called `ads->setDataRate(...)`, so it silently ran at
the Adafruit_ADS1X15 library's **default of 128 SPS** — roughly 7.8ms per
blocking one-shot sample (`readADC_Differential_0_1/2_3()` busy-polls
`conversionComplete()` until the conversion finishes), not 1.16ms.

60 samples × ~7.8ms × 2 sensors ≈ 936ms — called synchronously, once per
second, from inside `ts.execute()` on the Arduino loop task. That's why
`loop()` could only complete ~1 pass/sec: nearly the entire second was
spent blocked inside ADC polling for two current sensors.

## Detection

Investigating why the dashboard showed a suspicious "ESP32 internal temp"
fallback value led to a broader look at the dashboard's system-stats tile,
which turned up `CPU 0`/`CPU 1` both reading exactly `0%` — live-verified
against the production device (`/state` polled repeatedly, never moved off
`0`). Tracing the CPU-load code through its git history (three prior
attempts at the same idle-hook approach) pointed at the `vTaskDelay(1)`
added in a previous fix as the likely culprit.

Rewriting core 1's measurement to use direct busy-time accounting (timestamp
real work instead of relying on idle hooks) produced a new, unexpected
reading: ~99% load, sustained. That was surprising enough on its own —
walking a ~15-task scheduler list should take microseconds, not most of a
second — to warrant one more diagnostic: a `loopItersPerSec` counter
(incremented once per `loop()` pass, exposed via `/heap`). That counter
read exactly **1** on the live device, which pointed directly at whatever
single per-second task was blocking: `tReadCurrent`, and from there,
`CurrentSensor::readRMS()`'s undeclared sample rate.

## Fix

### CPU load measurement (`src/main.cpp`)

- Core 0 (WiFi/system tasks, not app-controlled): unchanged, still
  idle-hook timing — not affected by this bug since it's a different core
  than the one running `loop()`.
- Core 1 (Arduino loopTask): replaced the idle hook entirely with direct
  busy-time accounting — timestamp before/after the real work block
  (`ftpSrv.handleFTP()` + `ts.execute()`) each pass with
  `esp_timer_get_time()`, sum busy microseconds over the 1-second sampling
  window. `vTaskDelay(1)` still runs at the end of `loop()` (core 0 still
  needs it) but is now excluded from core 1's measurement window, so it can
  no longer swamp the reading.
- Added `loopItersPerSec` (`_loopIterCount`, `getLoopItersPerSec()`),
  exposed via `GET /heap`, as a permanent diagnostic — cheap, and the single
  most direct signal for "is the main loop actually running normally."

### Current sensor sample rate (`src/CurrentSensor.cpp`)

- `readRMS()` now explicitly calls `ads->setDataRate(RATE_ADS1115_860SPS)`
  before sampling, matching what the existing code comment already assumed
  was happening.

## Affected Code

- `src/main.cpp`: `idleHookCore0/1` → single `idleHookCore0` +
  `_busyUsCore1` busy-time accounting, `onCalcCpuLoad()`, `loop()`,
  `_loopIterCount`/`getLoopItersPerSec()`
- `src/WebHandler.cpp`: `GET /heap` — added `loopItersPerSec` field
- `src/CurrentSensor.cpp`: `readRMS()` — explicit `setDataRate()` call

## Verification (production, `192.168.0.49`)

| | Before | After busy-time fix (revealed the stall) | After datarate fix |
|---|---|---|---|
| `loopItersPerSec` | not measured | **1** | **~660–673** |
| `cpuLoad1` | `0` (always, artifact) | ~99% (real, but caused by the stall) | settles ~45% |
| `cpuLoad0` | `0` | `0` | ~3–6% (normal) |

Deployed via OTA (`/update` + `/apply`, which auto-backs up the running
firmware first). Device came back with a clean `SW_RESET`,
`crashBootCount:0`, `safeMode:false`, and current-sensor readings
(`COMPRESSOR_CURRENT`/`FAN_CURRENT`) still reporting cleanly. The system
was `OFF` at verification time (both readings 0.0A as expected); a
non-zero-load spot check is still owed the next time the compressor cycles
on, to confirm current-sensing accuracy held up at the faster sample rate.

## Lessons Learned

- **A metric that always reads a suspiciously round number (0%, 100%) is
  usually broken, not informative.** Three prior attempts tuned the same
  idle-hook approach without questioning whether idle-hook timing was the
  right technique at all for a core that never voluntarily blocks except
  via a delay added *for the metric's own sake*.
- **A code comment stating an assumption ("860 SPS gives ~1.16ms/sample")
  is not the same as code that enforces it.** The library's default (128
  SPS) silently won because nothing called `setDataRate()`. Assumptions
  about hardware config belong in an assertion or an explicit call, not
  just a comment.
- **Wall-clock busy-time accounting can conflate "genuinely executing" with
  "blocked in a busy-poll."** The fix here happened to reveal the real bug
  because the blocking ADC poll *is* wall-clock CPU-bound (a tight
  `while(!conversionComplete());` loop), but the same technique would
  overcount if a task instead blocked on something that lets other
  same/higher-priority work run in the gap (e.g. AsyncTCP, priority 10,
  unpinned). A `loopItersPerSec` counter alongside the load percentage is
  what made it possible to tell "really busy" apart from "frequently
  interrupted" without more invasive instrumentation.
- **Fixing an unrelated cosmetic dashboard bug can surface a much more
  serious latent one.** The internal-temp offset question that started this
  session had nothing to do with the current-sensor stall, but following
  the CPU-load thread all the way through was what found it.
