#ifndef WEBHANDLER_H
#define WEBHANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>
#include <TaskSchedulerDeclarations.h>
#include "GoodmanHP.h"
#include "Logger.h"

class WebHandler {
  public:
    WebHandler(uint16_t port, Scheduler* ts, GoodmanHP* hpController);
    void begin();
    bool shouldReboot() const { return _shouldReboot; }

  private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;

    Scheduler* _ts;
    GoodmanHP* _hpController;

    bool _shouldReboot;
    bool _ntpSynced;
    Task* _tNtpSync;

    static constexpr float MB_MULTIPLIER = 1.0f / (1024.0f * 1024.0f);

    // NTP config
    static constexpr const char* NTP_SERVER1 = "192.168.0.1";
    static constexpr const char* NTP_SERVER2 = "time.nist.gov";
    static const long GMT_OFFSET_SEC = 0;
    static const int DAYLIGHT_OFFSET_SEC = 0;

    void syncNtpTime();
    void setupRoutes();
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);
};

#endif
