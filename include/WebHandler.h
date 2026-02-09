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
    void startNtpSync();
    void setTimezone(int32_t gmtOffset, int32_t daylightOffset);
    bool shouldReboot() const { return _shouldReboot; }
    const char * getWiFiIP();

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
    int32_t _gmtOffsetSec = -21600;
    int32_t _daylightOffsetSec = 3600;
    static constexpr const char* NOT_AVAILABLE = "NA";

    void syncNtpTime();
    void setupRoutes();
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);

    // GPIO test state
    Task* _tGpioTest;
    bool _gpioTestRunning;
    int _gpioTestStep;
    String _gpioTestResult;
    int _gpioTestWOn, _gpioTestOOn;   // W/O reads after W turned ON
    int _gpioTestWOff, _gpioTestOOff; // W/O reads after W turned OFF
};

#endif
