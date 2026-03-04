# BUG-008: OTA upload hangs device due to concurrent SD access

**Date**: 2026-03-03
**Severity**: High
**Status**: Fixed
**Affected versions**: v1.0 through v1.2-rc8
**Fixed in**: `a75ab7b`

## Symptom

Device becomes completely unresponsive during OTA firmware upload via
`POST /update`. No WDT/panic reset — just frozen. Requires physical power
cycle to recover.

## Impact

- OTA firmware updates unreliable — device hangs mid-upload
- No crash dump or WDT reset, making diagnosis difficult
- Requires physical access to recover (power cycle)

## Root Cause

The HTTPS `httpd` task runs on a separate FreeRTOS task from the Arduino
`loop()`. The OTA handler performed `httpd_req_recv()` + `SD.write()` in a
tight loop with a 1024-byte buffer (~1500 iterations for a 1.5MB firmware).

Meanwhile, the main loop continued running tasks that access SD:
- `Logger` — log file writes
- `tSaveRuntime` — config runtime persistence (every 5 min)
- `tLogTempsCSV` — temperature CSV logging (every 30s)

The Arduino SD library is **not thread-safe**. Concurrent SPI bus access from
multiple FreeRTOS tasks corrupts the SPI bus state, causing a deadlock where
both tasks wait indefinitely on the SPI peripheral.

## Fix

Buffer the entire firmware in PSRAM before writing to SD:

```cpp
// Allocate full firmware buffer in PSRAM
uint8_t* fwBuf = (uint8_t*)heap_caps_malloc(totalSize, MALLOC_CAP_SPIRAM);

// Receive entire firmware into PSRAM buffer
while (remaining > 0) {
    int ret = httpd_req_recv(req, (char*)(fwBuf + offset), remaining);
    offset += ret;
    remaining -= ret;
}

// Single SD write after receive completes
File f = SD.open("/firmware.new", FILE_WRITE);
f.write(fwBuf, totalSize);
f.close();
heap_caps_free(fwBuf);
```

Also bumped HTTPS httpd task stack from 10KB to 16KB.

## Affected Code

- `src/HttpsServer.cpp`: OTA upload handler — PSRAM buffering, stack size

## Lessons Learned

- Arduino SD is NOT thread-safe — any code on non-Arduino FreeRTOS tasks
  (HTTPS httpd, AsyncTCP) must minimize SD access duration
- Use PSRAM buffering for large transfers to avoid SPI bus corruption with
  main loop SD operations
- SPI bus deadlocks don't trigger WDT because the tasks are technically
  still running (blocked on SPI, not in an infinite loop)
