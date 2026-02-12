#ifndef HTTPSSERVER_H
#define HTTPSSERVER_H

#include <cstdint>
#include <cstddef>
#include <functional>

// Forward declarations — avoid including esp_https_server.h here
// to prevent enum conflicts with ESPAsyncWebServer
class Config;
class GoodmanHP;
class TempHistory;
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
    std::function<void(int)> ftpEnableCb;
    std::function<void()> ftpDisableCb;
    bool* ftpActive;
    unsigned long* ftpStopTime;
    // WiFi test state (shared with WebHandler)
    String* wifiTestState;
    String* wifiTestMessage;
    String* wifiTestNewSSID;
    String* wifiTestNewPassword;
    String* wifiOldSSID;
    String* wifiOldPassword;
    uint8_t* wifiTestCountdown;
    Task** wifiTestTask;
    TempHistory* tempHistory;
};

// Opaque handle
typedef void* HttpsServerHandle;

HttpsServerHandle httpsStart(const uint8_t* cert, size_t certLen,
                             const uint8_t* key, size_t keyLen,
                             HttpsContext* ctx);

#endif
