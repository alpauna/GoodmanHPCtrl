# BUG-011: MQTT reconnect loop exhausts lwIP TCP PCBs

**Date**: 2026-03-10
**Severity**: High
**Status**: Fixed
**Affected versions**: v1.0 through v1.2-rc16
**Fixed in**: `dceef77`

## Symptom

Device becomes completely unresponsive after running for multiple days with
the MQTT broker unreachable. No WDT/panic reset — device appears frozen.
HTTP, HTTPS, MQTT, and weather fetch all stop working simultaneously.
Requires physical power cycle to recover. Ping may also fail.

## Impact

- Total loss of network connectivity — HTTP server unreachable, no remote
  monitoring or control
- Heat pump control loop continues running (outputs/inputs work), but no
  remote visibility
- No crash dump or reset reason — appears as a silent hang
- Requires physical access to recover (power cycle)

## Root Cause

The MQTT reconnect task (`_tReconnect`) attempted a new connection every
10 seconds unconditionally. When the MQTT broker was unreachable, each
`_client.connect()` call created a TCP socket that went through
SYN_SENT → TIME_WAIT states in the lwIP stack.

Over extended periods (observed: 7 days), ~60,000+ TCP connection attempts
accumulated. The lwIP TCP PCB (Protocol Control Block) pool is finite —
each failed connection leaves a PCB in TIME_WAIT for up to 120 seconds.
With connections every 10 seconds, the PCB pool eventually exhausted:

- No new TCP connections possible (MQTT, HTTP server, weather fetch)
- HTTP server cannot accept incoming connections — device unreachable
- ICMP/ping may also fail once lwIP is completely jammed
- Main loop continues running (WDT happy) — no crash, just network-dead

Log pattern observed: continuous "Connecting to MQTT..." / "Disconnected
from MQTT (reason: 0)" every ~10 seconds for 7 days straight with no
successful connection.

## Fix

Added exponential backoff to the MQTT reconnect task. Interval doubles
on each failed attempt, capped at 5 minutes. Resets to 10 seconds on
successful connection:

```cpp
// Exponential backoff: 10s → 20s → 40s → 80s → 160s → 300s (cap)
_consecutiveFailures++;
uint32_t nextInterval = _reconnectIntervalSec * 2;
if (nextInterval > RECONNECT_MAX_SEC)
    nextInterval = RECONNECT_MAX_SEC;
_reconnectIntervalSec = nextInterval;
_tReconnect->setInterval(_reconnectIntervalSec * TASK_SECOND);
```

On successful connect (`onConnect`), backoff resets:

```cpp
_consecutiveFailures = 0;
_reconnectIntervalSec = RECONNECT_MIN_SEC;
_tReconnect->setInterval(RECONNECT_MIN_SEC * TASK_SECOND);
```

Reduces TCP churn from ~60,000 attempts/week to ~2,000 (30x reduction).

## Affected Code

- `include/MQTTHandler.h`: Added backoff member variables and constants
- `src/MQTTHandler.cpp`: Backoff logic in reconnect task, reset in
  `onConnect()`

## Detection

Device at 192.168.1.138 became unreachable after 7 days (2026-03-03 to
2026-03-10). SD card log showed continuous MQTT connect/disconnect loop
at 10-second intervals for the entire session. Last log entry was a normal
"Connecting to MQTT..." followed by silence — then fresh POWERON boot
after manual power cycle.

## Lessons Learned

- Unbounded retry loops on network operations will eventually exhaust
  lwIP resources on ESP32 — always use backoff
- The failure mode is silent (no crash/panic/WDT) because the main loop
  continues running — only networking dies
- AsyncMqttClient's TCP connections consume lwIP PCBs even on failure —
  high-frequency retries are dangerous
