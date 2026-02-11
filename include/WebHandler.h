#ifndef WEBHANDLER_H
#define WEBHANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <TaskSchedulerDeclarations.h>
#include "GoodmanHP.h"
#include "Logger.h"
#include "Config.h"

class WebHandler {
  public:
    WebHandler(uint16_t port, Scheduler* ts, GoodmanHP* hpController);
    void begin();
    void startNtpSync();
    void setTimezone(int32_t gmtOffset, int32_t daylightOffset);
    void setConfig(Config* config) { _config = config; }
    bool shouldReboot() const { return _shouldReboot; }
    const char * getWiFiIP();

  private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;

    Scheduler* _ts;
    GoodmanHP* _hpController;
    Config* _config;

    bool _shouldReboot;
    Task* _tDelayedReboot;
    bool _ntpSynced;
    Task* _tNtpSync;

    static constexpr float MB_MULTIPLIER = 1.0f / (1024.0f * 1024.0f);

    // NTP config
    static constexpr const char* NTP_SERVER1 = "192.168.0.1";
    static constexpr const char* NTP_SERVER2 = "time.nist.gov";
    int32_t _gmtOffsetSec = -21600;
    int32_t _daylightOffsetSec = 3600;
    static constexpr const char* NOT_AVAILABLE = "NA";
    String _wifiIPStr;

    void syncNtpTime();
    void setupRoutes();
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);
};

#endif
