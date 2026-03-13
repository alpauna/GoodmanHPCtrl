# BUG-012: Log ring buffer race condition freezes networking

**Date**: 2026-03-13
**Severity**: High
**Status**: Fixed
**Affected versions**: v1.0 through v1.2-rc16
**Fixed in**: `0ad4a02`

## Symptom

Accessing the log page causes it to show "loading..." and never return
results. Within seconds, all HTTP and HTTPS endpoints become unresponsive.
MQTT disconnects and weather HTTP fetches begin failing. Heat pump control
loop continues running normally — inputs, outputs, and SD card logging all
work. Requires physical power cycle to recover.

## Impact

- Total loss of network connectivity — identical symptom to BUG-011 but
  different root cause
- Triggered by user action (opening log page), not by time-based
  resource exhaustion
- Heat pump control continues but no remote monitoring or control
- No crash dump or reset reason — silent network death

## Root Cause

The `Logger` ring buffer (`std::vector<String>`, 500 entries) has no
thread synchronization. Three separate FreeRTOS tasks access it
concurrently:

1. **Main loop (core 1)**: `Logger::log()` calls `addToRingBuffer()` —
   overwrites `_ringBuffer[_ringBufferHead]` with a new String, advances
   `_ringBufferHead` and `_ringBufferCount`
2. **HTTPS httpd task (core 0)**: `logGetHandler()` calls
   `getRingBuffer()` (returns const reference to live vector), iterates
   all 500 entries character-by-character to build JSON
3. **AsyncTCP task (core 0)**: HTTP `/log` handler does the same via
   `AsyncWebServerRequest`

Additionally, `Logger::log()` uses a shared `_buffer[512]` member for
`snprintf()`. The `[AUTH] HTTPS session created` log message is emitted
from the httpd task, racing with main loop log calls on the same buffer.

The race condition:

1. HTTPS `logGetHandler` starts reading ring buffer entries on core 0
2. Main loop calls `Log.info()` → `addToRingBuffer()` on core 1
3. `addToRingBuffer()` overwrites `_ringBuffer[idx]` — the old String's
   internal buffer is freed
4. The httpd task reads the freed String — corrupted `length()` field
   causes infinite loop in the character-by-character JSON escaping loop
5. httpd task hangs → all HTTPS requests queue behind it (single-threaded)
6. Hung TCP connection cascades through lwIP, killing MQTT and HTTP

The log from 2026-03-13 confirms the sequence:
- `08:26:50` — HTTPS session created (user opened dashboard)
- `08:27:21` — MQTT disconnected (networking dying)
- `08:31:35` — Weather HTTP fetch failed (networking dead)
- `08:27–08:51` — HP control loop continued normally (Y/CNT/FAN events)
- Log page showed "loading..." and never returned

## Fix

Added a FreeRTOS mutex to the `Logger` class protecting all shared state:

1. **Mutex on `Logger::log()`** — serializes all writes to `_buffer` and
   ring buffer across tasks. Uses 100ms timeout — drops the log entry
   rather than deadlocking:

```cpp
void Logger::log(Level level, const char* tag, const char* format, va_list args) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    // ... format message, write to all outputs ...
    xSemaphoreGive(_mutex);
}
```

2. **`getLogJson(limit)`** — new method that snapshots ring buffer entries
   under the mutex, then builds JSON from the copy outside the mutex.
   Eliminates the race entirely — readers never touch the live buffer:

```cpp
String Logger::getLogJson(size_t limit) const {
    std::vector<String> entries;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // Copy entries from ring buffer
        // ...
        xSemaphoreGive(_mutex);
    }
    // Build JSON from snapshot (outside mutex)
    // ...
}
```

3. Both `WebHandler.cpp` (`/log` HTTP) and `HttpsServer.cpp` (`/log`
   HTTPS) replaced with single call to `Log.getLogJson(limit)`.

## Affected Code

- `include/Logger.h`: Added `SemaphoreHandle_t _mutex`, `getLogJson()`
- `src/Logger.cpp`: Mutex init, `log()` protection, `getLogJson()` impl
- `src/WebHandler.cpp`: `/log` handler simplified to use `getLogJson()`
- `src/HttpsServer.cpp`: `logGetHandler` simplified to use `getLogJson()`

## Detection

User reported log page stuck on "loading..." at ~08:27 on 2026-03-13,
followed by complete HTTP/HTTPS unresponsiveness. SD card log showed
networking died (MQTT disconnect, weather fetch failures) while HP control
continued normally — classic symptom of a networking-layer hang with a
healthy main loop.

## Lessons Learned

- Any data structure accessed from multiple FreeRTOS tasks (httpd, AsyncTCP,
  Arduino loop) MUST have mutex protection — even read-only access is unsafe
  if another task is writing
- Returning `const&` to internal data structures invites unsynchronized
  access — prefer snapshot/copy methods for cross-task data
- String corruption from concurrent access doesn't always crash — it can
  cause infinite loops (corrupted length field) that silently freeze the
  accessing task
- The esp-idf httpd is single-threaded — one hung handler blocks all
  HTTPS requests, which cascades to kill lwIP networking
- Use bounded mutex timeouts (not `portMAX_DELAY`) to prevent deadlocks
  in logging — dropping a log entry is better than freezing
