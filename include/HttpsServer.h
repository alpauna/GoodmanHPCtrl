#ifndef HTTPSSERVER_H
#define HTTPSSERVER_H

#include <cstdint>
#include <cstddef>

// Forward declarations — avoid including esp_https_server.h here
// to prevent enum conflicts with ESPAsyncWebServer
class Config;
class GoodmanHP;
class Scheduler;
class Task;

struct HttpsContext {
    Config* config;
    GoodmanHP* hpController;
    Scheduler* scheduler;
    bool* shouldReboot;
    Task** delayedReboot;
    int32_t* gmtOffsetSec;
    int32_t* daylightOffsetSec;
};

// Opaque handle
typedef void* HttpsServerHandle;

HttpsServerHandle httpsStart(const uint8_t* cert, size_t certLen,
                             const uint8_t* key, size_t keyLen,
                             HttpsContext* ctx);

#endif
