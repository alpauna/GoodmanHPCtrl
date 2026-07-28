# ADS131M04 SPI Current Sensor — Design Notes

**Status**: Scoping only — no hardware populated yet, no driver code written.
**Schematics**: [`SPI-Current-Sensor-Schematic.pdf`](schematics/SPI-Current-Sensor-Schematic.pdf)
(daughter board, `Board1`, v1.0, 2026-05-22) +
[`Goodman-Heatpump-Main-Board.pdf`](schematics/Goodman-Heatpump-Main-Board.pdf)
(main board, `Board1`, v1.0, updated 2026-07-27 — `H3`/`U43` are the
daughter-board interconnect)
**Motivated by**: [BUG-014](bugs/014-main-loop-stall-from-ads1115-sample-rate.md) — the ADS1115-based
current sensing needed a software calibration fudge factor (`ctRatio`), a
hand-built non-blocking polling state machine, and still samples the two
channels ~470ms apart from each other rather than simultaneously.

## Why this is a real upgrade, not just a faster ADC

| | ADS1115 (current) | ADS131M04 (this schematic) |
|---|---|---|
| Resolution | 16-bit | 24-bit |
| Channels | 2 (mux'd, one at a time) | 4 (3 wired: compressor/fan/heater), **simultaneous** |
| Interface | I2C | SPI |
| Ready signal | Poll a status bit (`conversionComplete()`) | Hardware `DRDY#` line |
| Clock | Internal RC oscillator | External 8.192MHz crystal |
| Anti-alias filtering | None (raw CT signal straight to ADC) | RC low-pass per channel before the ADC |
| DC bias / offset | Software: subtract computed mean per 60-sample window | Hardware: AC-coupled into a precision 1.65V `VBIAS` (`OP07CDR` buffer) |
| Phase reference | None — sampling starts whenever the 1s task tick fires | Zero-cross detector (`ZX`) fed into `AIN2` — sampled in the same synchronized frame as every other channel |

Every one of BUG-014's pain points traces back to a limitation of the
ADS1115 approach specifically:

- **The loop-stall bug** existed because one-shot conversions had to be
  triggered and polled one at a time, in series, for each of 60 samples ×
  2 channels. Simultaneous sampling with a `DRDY#` interrupt needs zero
  busy-polling and produces both channels' data in the same SPI frame.
- **The 860 SPS accuracy regression** happened because faster ADS1115
  sampling widened its internal filter bandwidth and let more noise
  through. The ADS131M04 has a dedicated RC anti-alias filter in the
  analog front end — filtering happens once, in hardware, independent of
  the digital data rate chosen.
- **The ~20-25% ctRatio calibration gap** was diagnosed and corrected
  empirically with a clamp meter because there was no other way to know
  the ADS1115+CT+burden signal chain's real gain. A precision buffered
  bias reference and proper anti-alias filtering won't eliminate the need
  for *some* per-channel scale calibration (CT clamps themselves have
  manufacturing tolerance), but should make the starting error much
  smaller and more consistent channel-to-channel than what we measured
  tonight.
- **Compressor/fan samples ~470ms apart**: the current round-robin
  (`GoodmanHP::tickCurrentSensors()`) samples one channel to completion
  before starting the next. Simultaneous sampling means "compressor
  current" and "fan current" are always readings from the *same instant*,
  which matters for anything that ever wants to correlate the two (it
  doesn't today, but it's a correctness gap worth not having).

## Hardware overview (from the schematic)

- **`U3` — ADS131M04IRUKR**: 4-ch, 24-bit, simultaneous-sampling SPI ADC.
  All 4 channels are used: `AIN0` (compressor), `AIN1` (fan), `AIN3`
  (heater/crankcase — not currently implemented in firmware at all;
  `crankcaseCtRatio`/`crankcaseBurdenOhms`/`crankcaseExpectedAmps` exist
  in `Config.h` but have no `CurrentSensor` instance behind them), and
  **`AIN2` carries the zero-cross reference** (`AIN2P` = `ZX`, `AIN2N` =
  `AGND` — single-ended into a differential input). There is no spare
  channel on this ADC; a 5th sensor would need a second chip.
- **`U1` — 8.192MHz oscillator** on `CLKIN`, with `RESET#`/`CS#` pulled up
  (`R1`/`R2`/`R3`, 100kΩ) so the ADC idles in a safe state if the MCU's
  GPIOs are tri-stated at boot.
- **`H1`** — 7-pin header: `CS#`, `DRDY#`, `SCLK`, `DOUT`, `DIN`, `3.3V`,
  `DGND`. `RESET#`/`SYNC#` is *not* on this header — it's pulled up to
  3.3V via `R3` on the daughter board and never reaches the ESP32 (see
  "Open questions" below, now resolved).
- **`U8` — `OP07CDR`** precision op-amp, buffering a `10k`/`10k` divider
  off 3.3V into a stable `VBIAS = 1.65V` reference, fed to the three CT
  channels' bias networks (not `AIN2`/`ZX`, which is a digital signal, not
  an AC current waveform needing bias).
- **Per current channel** (compressor/fan/heater identical): CT clamp jack
  (`PJ-3200A-3A`, standard 3.5mm CT connector) → `1k`/`10nF` RC low-pass
  → `10µF` AC-coupling caps into the ADC's differential input pair, biased
  to `VBIAS` through `10k` resistors on both legs.
- **`ZX`** — a 2-pin header (`U2` on the daughter board, `U43` on the main
  board) carries the zero-cross signal from the main board's isolated
  detector (`U33`/`AT3H4B-CuH-S`) directly into `AIN2P` on `U3`, with
  `AIN2N` grounded to `AGND`. It does **not** go to an ESP32 GPIO at all —
  see the "Zero-cross synchronized windows" section below for what that
  means for firmware.

## Main-board interconnect

The main board (`Board1`, CPU page) has two connectors dedicated to this
daughter board, `H3` and `U43`, both matching the daughter board's own
connectors by pin count and part number:

**`H3` (`PM254V-11-07-H85`, 7-pin) ↔ daughter board `H1` (7-pin)** —
**confirmed pin-for-pin off the live schematic** (a straight ribbon/cable
connection — `H3` pin *N* wires directly to `H1` pin *N*, not matched by
signal-name position, which is what an earlier PDF-text-based guess got
wrong):

| `H3` pin # | Net (main board) | `H1` pin # | Net (daughter board) | Function |
|---|---|---|---|---|
| 1 | GND | 1 | DGND | ground |
| 2 | 3.3V | 2 | 3.3V | power |
| 3 | GPIO2 | 3 | DIN | SPI MOSI → ADC |
| 4 | GPIO42 (`MTMS`) | 4 | DOUT | SPI MISO ← ADC |
| 5 | GPIO1 | 5 | SCLK | SPI clock |
| 6 | GPIO46 | 6 | DRDY# | data-ready interrupt |
| 7 | GPIO45 | 7 | CS# | chip select |

`GPIO42` = `MTMS` on ESP32-S3, consistent with this repo's own prior fix
(`836cca4`, "GPIO 42 is used for DOUT (MISO)") — this board repurposes
JTAG pins as general GPIO.

No pin sharing on this bus: `SW1` → `RESET`/`CHIP_PU`, `SW2` → `GPIO0`
(the standard ESP32 boot-mode-select pin, present but unused so far on
this board) — both confirmed unrelated to `GPIO1`/`GPIO2`/`GPIO45`/
`GPIO46`/`GPIO42`. All 5 signal pins on `H3` are dedicated to this bus.

**`U43` (`ZX-PM2.54-1-2PY`, 2-pin: `ZX`/`AGND`)** — separate from `H3`,
matching the daughter board's own 2-pin `ZX`/`AGND` header. `ZX` originates
on the triacs page at `U33` (`AT3H4B-CuH-S`), an opto-isolated zero-cross
detector sensing the 24VAC line directly (isolated ground domain
`E-GND`), with its output pulled up to 3.3V through `R28` (33kΩ) — a
clean, isolated 3.3V logic-level square wave. **Confirmed destination**:
this signal does *not* go to an ESP32 GPIO — it's wired straight into
`AIN2P` on the daughter board's ADC (`AIN2N` tied to `AGND`), so the
ADS131M04 samples it as a 4th channel in the same synchronized frame as
the current channels. No interrupt, no GPIO, no separate timing domain.

## Firmware architecture (proposed)

### Split acquisition from accumulation

Today, `CurrentSensor` owns both the ADC I/O (`beginSample()`/`tick()`
talking to `Adafruit_ADS1115`) and the RMS math. That made sense for a
mux'd single-channel-at-a-time ADC where each sensor genuinely does its
own independent acquisition. It doesn't fit a simultaneous-sampling ADC,
where one `DRDY#` event produces data for *all* channels at once.

Proposed split:

- **New `SPICurrentADC` class** (or similar) owns the ADS131M04: SPI
  transport, `DRDY#` interrupt handling, register configuration
  (gain/OSR/data rate), and zero-cross synchronization. On each `DRDY#`
  event it reads one frame (all channel words in one SPI transaction) and
  pushes `{channel, rawCounts}` samples out.
- **`CurrentSensor` simplifies** to a pure accumulator: keep the existing
  `sumV`/`sumSquares`/`peak`/`nonZeroCount` accumulation and the
  AC-RMS math (mean-subtract, variance, sqrt, ×`ctRatio`) and
  `checkProtections()`, but drop `beginSample()`/`tick()`'s ADC-talking
  responsibilities — it just receives pushed samples from
  `SPICurrentADC` and finalizes when its window is full.
- **`GoodmanHP` drops the round-robin** (`tickCurrentSensors()`,
  `_currentSampleIt`/`_currentSamplingActive`) entirely — that machinery
  exists solely because the ADS1115 can only look at one channel at a
  time. With simultaneous sampling there's nothing to round-robin.

### `DRDY#`-driven acquisition, not polling

Attach an ISR (or a very short debounce + flag pattern, matching how
`InputPin` already does ISR → queued-event → scheduler-task handling in
this codebase) to `DRDY#`'s falling edge. The ISR should do the minimum
possible (set a flag / bump a counter) and let a normal-priority
scheduler task do the actual SPI transaction — same pattern already used
for `_isrEvent`/`_tGetInputs` in `GoodmanHP`, so this fits the existing
architecture rather than introducing a new one.

### Zero-cross synchronized windows

No GPIO interrupt involved — `ZX` isn't wired to the ESP32 at all. It's
fed into `AIN2P` (`AIN2N` = `AGND`) on the ADC itself, so every `DRDY#`
frame that delivers compressor/fan/heater samples *also* delivers a
zero-cross-channel sample, already time-aligned with the others by the
ADC's simultaneous-sampling architecture. This is a cleaner mechanism
than a second interrupt would have been: no ISR latency, no separate
timing domain to reconcile against the current channels, nothing to
synchronize — it's synchronized by construction.

Firmware-side, zero-cross detection becomes a software job on the
`AIN2` sample stream instead of a hardware edge interrupt: watch
consecutive `AIN2` readings for a sign change (or a threshold crossing,
depending on what `ZX`'s actual signal shape looks like once the board's
in hand — likely a clean square wave given the opto-isolated source, in
which case this is a simple "did the sign flip since last frame" check).
`SPICurrentADC` (or `CurrentSensor` for the `AIN2` channel specifically)
starts/stops each RMS accumulation window on those detected transitions
instead of on an arbitrary task-tick boundary — same goal as originally
described (windows covering an exact integer number of half-cycles), just
achieved by watching a sampled channel rather than timestamping a GPIO
edge.

One consideration for when the board's in hand: `ZX` is a 3.3V-ish
digital square wave feeding an ADC input whose common-mode/PGA range may
not be designed for a full-rail digital swing — worth confirming `AIN2`
reads sensibly (not clipped/saturated) rather than assuming it "just
works" because the signal is clean at the source.

### Calibration

The 3-point clamp-meter calibration tool added in `config.html` tonight
(`toggleCtCalib()`/`captureCtRow()`/`computeCtCalibration()`) should carry
over with no changes — it operates on whatever `ctRatio` the backend
reports, independent of which ADC is producing the underlying reading.
`AIN2` is committed to the zero-cross reference (not available for a 4th
current sensor), so a `heaterCtRatio`/third `CurrentSensor` for the
now-wired crankcase channel (`AIN3`) is still the only remaining channel
to add — the ADC has no free channel beyond that.

## Open questions

1. ~~**`ZX` signal characteristics and destination**~~ — **resolved**:
   clean, isolated 3.3V logic square wave from an opto-isolated
   zero-cross detector (`U33`/`AT3H4B-CuH-S`) on the main board, fed
   directly into `AIN2P` on the ADC (`AIN2N` = `AGND`) — not a GPIO at
   all. See "Zero-cross synchronized windows" above for the firmware
   implication (software edge-detection on a sampled channel, not a
   hardware interrupt).
2. ~~**`RESET#`/`SYNC#` wiring**~~ — **resolved**: not part of `H3`'s 7
   pins, pulled up to 3.3V via `R3` (100kΩ) on the daughter board only —
   never reaches the ESP32. No software control over the physical pin;
   an in-field reset would need a power cycle or the ADS131M0x's
   SPI-based RESET command.
3. ~~**ESP32 pin assignment**~~ — **resolved**, confirmed pin-for-pin off
   the live schematic (not inferred from PDF text): GPIO2 (DIN), GPIO42
   /`MTMS` (DOUT), GPIO1 (SCLK), GPIO46 (DRDY#), GPIO45 (CS#). No pin
   sharing — `SW2` is on `GPIO0` (boot-select), not this bus.
4. **Exact SPI frame/register protocol** — general architecture above is
   accurate to how the ADS131M0x family works, but exact command opcodes,
   register addresses, and frame word-length configuration need to be
   verified against the TI datasheet directly before writing real driver
   code, not assumed from memory.
5. **Whether the ADS1115 stays populated as a fallback**, or this fully
   replaces it — affects whether `CurrentSensor`/`GoodmanHP` need to
   support both ADC types simultaneously (config-selectable) or if this
   is a clean cutover.

## Next steps

Nothing to implement yet — this doc is the scoping artifact. Once
`Board1` is populated and the open questions above are answered, the
concrete first step is a minimal SPI bring-up sketch (read the ADS131M04
ID/status register over SPI, confirm `DRDY#` toggles) before writing any
of the `SPICurrentADC` class described above.
