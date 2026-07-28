# BUG-014: Main loop stalled ~1s per pass — blocking ADS1115 current-sensor reads

**Date**: 2026-07-27
**Severity**: High
**Status**: Fixed
**Affected versions**: `f9f836a` through `45e2696` (~16 days in production)
**Fixed in**: `9870f65` (loop stall + CPU load metric), `3c202f7` (accuracy regression from the interim fix)

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

## Fix attempt #1 (`9870f65`) — bumped to 860 SPS, caused an accuracy regression

The first fix was minimal: explicitly call `ads->setDataRate(RATE_ADS1115_860SPS)`,
matching what the code comment already assumed. At 860 SPS the same 60-sample
blocking read drops from ~936ms to ~140ms for both sensors combined — loop
iteration rate went from ~1/sec to ~660+/sec, and `cpuLoad1` (now measured
correctly) settled at a believable ~45%.

This looked like a full fix until a clamp meter was put on the compressor and
fan leads next to the board and compared against the live readings at the
same moment:

| | Device (860 SPS) | Clamp meter | Error |
|---|---|---|---|
| Compressor | ~8.16A | 7.14A | **+1.02A (+14.3%)** |
| Fan | 0.9A | 0.54A | **+0.36A (+66.7%)** |

Both channels read high, and the fan channel — the smaller signal — was hit
much harder proportionally. That pattern (same sensor type, same `ctRatio`,
very different % error) doesn't fit a simple ratio/calibration problem; it
fits a noise floor that's a fixed fraction of full scale regardless of the
actual signal. The ADS1115's internal digital filter bandwidth widens as
data rate increases, so 860 SPS lets more line noise and motor/switching
harmonics through into each individual sample than 128 SPS does — inflating
the RMS calculation, and inflating it worst on the channel where the noise
is a bigger fraction of the real signal.

**This was a real regression, live on production current-sensing hardware
that feeds overcurrent and locked-rotor safety logic**, introduced by fix
attempt #1 in the course of fixing the loop stall.

## Fix attempt #2 (`3c202f7`) — non-blocking sampling, back to 128 SPS

Bumping the data rate traded accuracy for speed on a single knob. The actual
problem was never the *sample rate* — it was that the code sampled the ADC
*synchronously*, busy-waiting through all 60 conversions in one call. The
correct fix decouples the two: keep 128 SPS's better accuracy, but stop
blocking the loop while waiting for each conversion.

`CurrentSensor` was converted from a single blocking `readRMS()` call into a
non-blocking state machine:

- `beginSample(ads)` — resets accumulators, sets gain + 128 SPS, starts one
  ADS1115 one-shot conversion (`startADCReading(mux, /*continuous=*/false)`)
  and returns immediately.
- `tick(ads)` — call every `loop()` pass. Checks `ads->conversionComplete()`;
  if not ready, returns `false` and does nothing else this pass (no busy
  wait — other scheduler work runs in the gap). If ready, accumulates the
  sample and either starts the next conversion (still 59 to go) or, once all
  60 are in, runs the same AC-RMS math as before and returns `true`.

Since the ADS1115 is one shared I2C ADC, only one sensor can be
mid-acquisition at a time. `GoodmanHP` round-robins:

- `readCurrentSensors()` (still triggered once/sec by `tReadCurrent`) now
  only *starts* a round — `beginSample()` on the first sensor.
- `tickCurrentSensors()` — called every `loop()` pass — advances whichever
  sensor is currently converting. When a sensor's `tick()` returns `true`,
  its `checkProtections()` runs and the round-robin moves to the next
  sensor; when the last one finishes, sampling goes idle until the next
  1-second trigger.

Net effect: a ~7.8ms conversion no longer blocks anything — it's spread
across however many `loop()` passes happen to occur while it completes.

Re-verified against the clamp meter after this fix:

| | Device (128 SPS, non-blocking) | Clamp meter | Error |
|---|---|---|---|
| Fan | 0.6A | 0.54A | +0.06A (+11%) |

Fan-channel error dropped from +67% (860 SPS) to +11% (128 SPS,
non-blocking) — within plausible clamp-meter/CT tolerance. (Compressor
current wasn't directly comparable at this checkpoint since real compressor
draw had already shifted between checks — 5.4A vs. the earlier 7.14A/8.16A
reading — but the fan-channel result alone confirms the noise-bandwidth
hypothesis and that reverting to 128 SPS recovers the original accuracy.)

## Affected Code

- `src/main.cpp`: `idleHookCore0/1` → single `idleHookCore0` +
  `_busyUsCore1` busy-time accounting, `onCalcCpuLoad()`, `loop()`,
  `_loopIterCount`/`getLoopItersPerSec()`, `hpController.tickCurrentSensors()`
  call added to `loop()`
- `src/WebHandler.cpp`: `GET /heap` — added `loopItersPerSec` field
- `include/CurrentSensor.h` / `src/CurrentSensor.cpp`: `readRMS()` replaced
  with `beginSample()` + `tick()` non-blocking state machine (128 SPS)
- `include/GoodmanHP.h` / `src/GoodmanHP.cpp`: `readCurrentSensors()`
  changed to start-a-round semantics; new `tickCurrentSensors()` round-robin
  driver, `_currentSampleIt`/`_currentSamplingActive` state

## Verification (production, `192.168.0.49`)

| | Before | After busy-time fix (revealed the stall) | Fix #1: 860 SPS (blocking) | Fix #2: 128 SPS (non-blocking) |
|---|---|---|---|---|
| `loopItersPerSec` | not measured | **1** | ~660–673 | ~390–840 (varies with real workload) |
| `cpuLoad1` | `0` (always, artifact) | ~99% (real, caused by the stall) | settles ~45% | settles ~38–42% |
| `cpuLoad0` | `0` | `0` | ~3–6% | ~2–3% |
| Current-sensor accuracy vs. clamp meter | not checked | not checked | fan +67% high | fan +11% high |

Each stage deployed via OTA (`/update` + `/apply`, which auto-backs up the
running firmware first). Device came back with a clean `SW_RESET`,
`crashBootCount:0`, `safeMode:false` after every deploy. Compressor-channel
accuracy at the final fix wasn't re-verified against a simultaneous clamp
reading (real compressor draw had shifted between checks) — worth another
spot check next cycle.

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
- **The fastest fix for a blocking-call problem (crank up the hardware
  speed) can trade away accuracy you didn't know you were relying on.**
  Sample rate and filter bandwidth are coupled on a sigma-delta ADC; "make
  it faster" and "keep it accurate" are two different knobs, and only one
  of them (making the read non-blocking) actually solves the scheduling
  problem without a tradeoff.
- **Verify safety-relevant sensor changes against a real reference, not
  just "does it still report a number."** The 860 SPS fix passed every
  check available at the time (builds, boots cleanly, current values look
  plausible, no invalid/zero reads) and still had a 67% error on one
  channel. A clamp meter on the actual wire caught what none of the
  software-side checks could.
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
