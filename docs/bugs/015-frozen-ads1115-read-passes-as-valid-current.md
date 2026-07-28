# BUG-015: Loose ADS1115 connection froze current readings that still passed as `valid` — false FAN_FAULT trips shut down production compressor every ~3.5 min

**Date**: 2026-07-28
**Severity**: High (production equipment actively short-cycled by a false safety trip)
**Status**: Mitigated on production (physical reseat); **root-cause code gap still open** — no fix applied yet
**Affected versions**: all versions through `45e2696` (current as of this report), including both the pre- and post-BUG-014 `CurrentSensor` implementations
**Board**: production controller, `192.168.0.49`

## Symptom

Production controller was latching `ERROR` state roughly every 3.5 minutes
while in `COOL` mode with the compressor and fan both confirmed physically
running by direct observation. `/log` showed a clean, repeating
`checkFanFault()` trip cycle:

```
FAN_FAULT: FAN current low (0.00A < 0.30A) while CNT on, FAN pin already ON — watching 20s
FAN_FAULT: still low current 0.00A at ...ms/20000ms of grace window
FAN_FAULT: TRIP — FAN current 0.00A < 0.30A for 20000ms ... — shutting down compressor
FAN_FAULT: CNT shut down after ~18s of runtime (no airflow, fanAmps=0.00)
FAN_FAULT: ENTER ERROR state for 3 min lockout
```

`/state` confirmed both `COMPRESSOR_CURRENT` and `FAN_CURRENT` reading a
flat `0.0A` the entire time, despite the compressor drawing its normal
~7A and the fan running. The OLED display had also stopped updating, and
`LIQUID_TEMP` (MCP9600 I2C thermocouple fallback) had been frozen at
exactly `32.0°F` all session — both suggestive of a shared-I2C-bus problem,
though the MCP9600 turned out to be a separate, likely pre-existing issue
(see Investigation).

The user physically reseated the ADS1115 breakout board; both current
channels immediately began reading live, moving values again
(`COMPRESSOR_CURRENT` ~7.0-7.2A, `FAN_CURRENT` ~0.8A across successive
polls), and the `ERROR` cycling stopped.

## Root Cause

**A loose/marginal physical connection to the ADS1115 board**, combined
with a library-level gap: `Adafruit_ADS1X15::readRegister()` /
`writeRegister()` (vendored, unmodified — `.pio/libdeps/*/Adafruit
ADS1X15/Adafruit_ADS1X15.cpp`) call `m_i2c_dev->write()` /
`m_i2c_dev->read()` and **discard the returned success/failure status**:

```cpp
uint16_t Adafruit_ADS1X15::readRegister(uint8_t reg) {
  buffer[0] = reg;
  m_i2c_dev->write(buffer, 1);
  m_i2c_dev->read(buffer, 2);
  return ((buffer[0] << 8) | buffer[1]);
}
```

When the physical connection is marginal, these I2C transactions fail, but
`buffer` simply keeps whatever it held from the last successful read —
`readRegister()` returns **the same stale, non-zero raw ADC code every
single call**, with no exception, no error return, nothing to distinguish
it from a genuine fresh reading.

This defeated the one guard `CurrentSensor::tick()` (added in BUG-014) does
have against bad I2C data:

```cpp
// Reject "all zeros" reads (I2C NACK / bus contention signature) ...
if (_nonZeroCount == 0) {
    _valid = false;
    ...
}
```

A frozen-but-nonzero raw value has `_nonZeroCount > 0` (passes the guard),
but because all 60 accumulated samples are *identical*, `variance =
mean(v²) - mean(v)² = 0` exactly — so `_rmsAmps` comes out to precisely
`0.00A` and `isValid()` is `true`. `checkFanFault()` (`src/GoodmanHP.cpp:411`)
only skips its trip logic when `!fanCur->isValid()`; a validly-flagged
`0.00A` reading looks identical to a real "fan isn't moving air" fault, so
the safety logic did exactly what it's designed to do — it just did it on
corrupted sensor data instead of a real fault.

This is a **latent gap that predates BUG-014**: the old blocking
`readRMS()` used the same `readRegister()`/`writeRegister()` calls and
would have frozen identically under a loose connection. BUG-014's
non-blocking rewrite changed *how* samples are collected, not whether a
frozen I2C read is detected — so tonight's `CurrentSensor` changes are not
the cause here, they just happened to be the most recently-touched code
in the vicinity.

## Investigation notes (ruled out / inconclusive)

- **MCP9600 (`LIQUID_TEMP`) frozen at exactly 32.0°F**: unlike the ADS1115
  driver, `TempSensor::update()`'s MCP9600 path (`src/TempSensor.cpp:60-104`)
  uses raw `Wire` calls with explicit error checking
  (`Wire.endTransmission(false)` return code, `Wire.requestFrom()` byte
  count). Both were succeeding (no "MCP9600 write failed"/"read short"
  messages), meaning the MCP9600 itself was ACKing and returning genuine
  register data — just a hot-junction register reading of raw `0` (0°C)
  every time. This is **not proof of a bus-wide lockup**; it's a separate,
  likely pre-existing MCP9600/thermocouple issue (open thermocouple lead,
  or the chip itself) that happened to be present throughout this
  session and deserves its own look, but isn't part of this bug's root
  cause. Note also that this driver's error messages go to `Serial.printf()`,
  **not** the `Logger`/ring-buffer — they would not have shown up in a
  remote `/log` fetch even if they had fired, which is why they didn't
  appear during remote diagnosis.
- **OLED going dark**: plausibly the same physical jostle affected a
  nearby connector/header shared with the ADS1115 board; not independently
  confirmed, and the SSD1306 library path wasn't audited for the same
  ignore-the-I2C-error pattern as part of this report.
- **A current "spike" observed 8:30-9:00 AM** on both channels, followed by
  the flatline, is consistent with the connection going from fully-seated
  to intermittent (noisy contact) to fully stuck-stale over that window,
  though the exact failure progression wasn't captured.

## Fix Applied

**Physical only**: reseating the ADS1115 board resolved the immediate
production hazard. No code was changed.

## Fix Not Yet Applied (open)

`CurrentSensor::tick()` should detect a frozen/stale read the same way it
already detects an all-zero read, since both are I2C-failure signatures
that can otherwise pass as a legitimate low-current reading:

- Track the raw ADC value (or a hash/sum of the 60-sample window) across
  consecutive sampling rounds per sensor.
- If the raw values are bit-for-bit identical across N consecutive rounds
  (e.g. 3), mark `_valid = false` and log `"%s read invalid: frozen value
  (I2C stale/disconnected?)"` instead of publishing a fake `0.00A`.
- This gives `checkFanFault()` (and the overcurrent/locked-rotor logic,
  which shares the same `isValid()` gate) the same protection against a
  frozen sensor that it already has against a dead one.

This is a firmware-side mitigation for a hardware-connection problem — it
won't fix a loose connector, but it will stop a loose connector from
silently masquerading as "the fan actually stopped" and shutting down the
compressor.

## Affected Code

- `include/CurrentSensor.h` / `src/CurrentSensor.cpp` — `tick()`'s
  validity check doesn't catch a frozen (non-zero, no-variance) read;
  needs the stale-value detector described above.
- `.pio/libdeps/*/Adafruit ADS1X15/Adafruit_ADS1X15.cpp` (vendored
  library, not project code) — `readRegister()`/`writeRegister()` ignore
  `Adafruit_BusIO_Device::write()`/`read()` return values entirely. Not
  something to patch in a vendored library; the defense belongs in
  `CurrentSensor::tick()` instead.
- `src/GoodmanHP.cpp:411` (`checkFanFault()`) — correctly skips its trip
  logic on `!isValid()` today; will automatically benefit once `isValid()`
  can distinguish frozen from fresh.

## Lessons Learned

- **"Valid but wrong" is a worse failure mode than "invalid."** The
  existing all-zero guard proves the team already knew I2C reads could
  fail silently — but a stuck-nonzero failure slipped through the same
  gap because the guard checked "is the raw value zero," not "did the
  raw value actually change." Any check written against one specific
  failure signature should be paired with the question "what if this
  fails a different way?"
- **A physical fix and a code fix are both real fixes, and neither
  substitutes for the other.** Reseating the board stopped tonight's harm
  immediately and correctly; it doesn't mean the software gap that let a
  frozen sensor drive a safety shutdown should stay open; the same loose
  connection (or a similar one) will recur eventually.
- **Debug logging that only reaches `Serial` is invisible during remote
  diagnosis.** The MCP9600 path's error handling is more careful than the
  ADS1115 path's (it checks return codes) but its failures would never
  have been seen in this investigation because they don't go through
  `Logger`. Worth routing all I2C-adjacent error paths through the same
  ring-buffer-backed logger the rest of the system uses.
