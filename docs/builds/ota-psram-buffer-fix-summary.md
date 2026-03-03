# OTA Upload PSRAM Buffer Fix

**Date:** 2026-03-03

## Problem

OTA firmware uploads (`POST /update`) consistently hung the device, requiring a power cycle to recover. The device would become completely unreachable (no ping, no HTTP) with no WDT/panic reset recorded (crash boots stayed at 0). Reproducible on both HTTP (AsyncWebServer) and HTTPS (ESP-IDF httpd) paths.

## Root Cause

**Concurrent SD card access from multiple FreeRTOS tasks without thread safety.**

The OTA upload handlers ran on separate FreeRTOS tasks from the Arduino loop:
- HTTPS: ESP-IDF httpd task
- HTTP: AsyncTCP task

Both handlers used a tight `recv() + SD.write()` loop with a 1024-byte buffer — ~1500 iterations for a 1.5MB firmware, taking 10-30 seconds. During that window, the Arduino loop's scheduled tasks continued accessing SD:
- `Logger::writeToSdCard()` on every log entry
- `tSaveRuntime` every 5 minutes
- `tLogTempsCSV` every 30 seconds

The Arduino SD library uses SPI and is **not thread-safe**. When two FreeRTOS tasks on different cores call `SD.open()`/`SD.write()` simultaneously, the SPI bus state gets corrupted, causing one or both tasks to spin-wait indefinitely on an SPI transfer that will never complete — a deadlock, not a crash.

## Fix

Buffer the entire firmware in PSRAM during the network receive phase, then write to SD in a single operation after all data is received. This eliminates the long concurrent SD access window (from ~30 seconds down to a fraction of a second).

### HTTPS handler (`HttpsServer.cpp`)

**Before:** Blocking `while` loop doing `httpd_req_recv()` + `fw.write()` with 1024-byte buffer
```cpp
char buf[1024];
while (remaining > 0) {
    int ret = httpd_req_recv(req, buf, toRead);
    fw.write((uint8_t*)buf, ret);  // SD write on every iteration
    remaining -= ret;
}
```

**After:** Receive into PSRAM, then single SD write
```cpp
uint8_t* fwBuf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
while (received < total) {
    int ret = httpd_req_recv(req, (char*)(fwBuf + received), total - received);
    received += ret;  // No SD access during recv
}
fw.write(fwBuf, total);  // Single SD write after all data received
```

Also increased httpd task stack size from 10KB (default) to 16KB for TLS + SD operation headroom.

### HTTP handler (`WebHandler.cpp`)

**Before:** AsyncWebServer body callback doing `SD.write()` on every chunk
```cpp
// Body handler called per chunk
if (_otaFile) {
    _otaFile.write(data, len);  // SD write on every chunk
}
```

**After:** Buffer chunks in PSRAM, single SD write on last chunk
```cpp
// Body handler called per chunk
if (_otaBuffer && index + len <= _otaTotal) {
    memcpy(_otaBuffer + index, data, len);  // PSRAM only, no SD
}
if (index + len == total && _otaBuffer) {
    File fw = SD.open("/firmware.new", FILE_WRITE);
    fw.write(_otaBuffer, _otaTotal);  // Single SD write at end
}
```

## Files Modified (3 files)

| File | Changes |
|------|---------|
| `src/HttpsServer.cpp` | Rewrote `updatePostHandler()`: PSRAM buffer for recv, single SD write, 2MB size limit. Increased `cfg.httpd.stack_size` to 16384 |
| `src/WebHandler.cpp` | Rewrote `/update` body handler: PSRAM buffer via `heap_caps_malloc`, `memcpy` per chunk, single SD write on final chunk |
| `include/WebHandler.h` | Replaced `File _otaFile` with `uint8_t* _otaBuffer` and `size_t _otaTotal` |

## Verification

| Test | Result |
|------|--------|
| OTA to test board (.136) via HTTPS | OK in 5.8s |
| OTA to production (.138) via HTTPS | OK in 5.5s |
| Apply + reboot on production | Clean boot, new firmware running |
| Test board still responsive after OTA | Confirmed |

Previously both devices hung on every OTA attempt.

## Key Takeaway

The Arduino SD library (SPI-based) is not thread-safe. Any code running on a non-Arduino FreeRTOS task (HTTPS httpd, AsyncTCP, etc.) must minimize SD access duration to avoid SPI bus corruption with the main loop's SD operations. PSRAM buffering is the preferred pattern for large transfers.
