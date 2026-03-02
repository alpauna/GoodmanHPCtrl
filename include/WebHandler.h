#ifndef WEBHANDLER_H
#define WEBHANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>
#include <SD.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <functional>
#include <TaskSchedulerDeclarations.h>
#include "GoodmanHP.h"
#include "Logger.h"
#include "Config.h"
#include "HttpsServer.h"
#include "SessionManager.h"

class TempHistory;

class WebHandler {
  public:
    WebHandler(uint16_t port, Scheduler* ts, GoodmanHP* hpController);
    void begin();
    bool beginSecure(const uint8_t* cert, size_t certLen, const uint8_t* key, size_t keyLen);
    void startNtpSync();
    void setTimezone(const String& tz);
    void setConfig(Config* config) { _config = config; }
    bool shouldReboot() const { return _shouldReboot; }
    void setRebootRateLimited(bool* flag) { _rebootRateLimited = flag; }
    void setSafeMode(bool* flag, uint32_t* crashCount) { _safeMode = flag; _crashBootCount = crashCount; }
    const char * getWiFiIP();

    typedef std::function<void(int)> FtpEnableCallback;
    typedef std::function<void()> FtpDisableCallback;
    typedef std::function<String()> FtpStatusCallback;
    typedef std::function<void(uint32_t)> TempHistoryIntervalCallback;
    typedef std::function<void(uint32_t, bool)> DisplayConfigCallback;
    typedef std::function<String()> APStartCallback;
    typedef std::function<void()> APStopCallback;
    typedef std::function<void(const String&)> WeatherTopicCallback;
    typedef std::function<void(bool)> WeatherHttpCallback;
    typedef std::function<void(bool)> FailoverTestCallback;
    typedef std::function<void(uint32_t)> WeatherRefreshCallback;
    typedef std::function<bool()> MqttConnectedCallback;
    void setTempHistory(TempHistory* th) { _tempHistory = th; }
    void setFtpControl(FtpEnableCallback enableCb, FtpDisableCallback disableCb, FtpStatusCallback statusCb);
    void setTempHistoryIntervalCallback(TempHistoryIntervalCallback cb) { _tempHistIntervalCb = cb; }
    void setDisplayConfigCallback(DisplayConfigCallback cb) { _displayConfigCb = cb; }
    void setFtpState(bool* activePtr, unsigned long* stopTimePtr);
    void setAPCallbacks(APStartCallback startCb, APStopCallback stopCb);
    void setWeatherTopicCallback(WeatherTopicCallback cb) { _weatherTopicCb = cb; }
    void setWeatherHttpCallback(WeatherHttpCallback cb) { _weatherHttpCb = cb; }
    void setFailoverTestCallback(FailoverTestCallback cb) { _failoverTestCb = cb; }
    void setWeatherRefreshCallback(WeatherRefreshCallback cb) { _weatherRefreshCb = cb; }
    void setMqttConnectedCallback(MqttConnectedCallback cb) { _mqttConnectedCb = cb; }

  private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    HttpsServerHandle _httpsServer = nullptr;
    HttpsContext _httpsCtx = {};
    SessionManager _sessionMgr;

    Scheduler* _ts;
    GoodmanHP* _hpController;
    Config* _config;
    TempHistory* _tempHistory = nullptr;

    bool _shouldReboot;
    bool* _rebootRateLimited = nullptr;
    bool* _safeMode = nullptr;
    uint32_t* _crashBootCount = nullptr;
    Task* _tDelayedReboot;
    bool _ntpSynced;
    Task* _tNtpSync;

    static constexpr float MB_MULTIPLIER = 1.0f / (1024.0f * 1024.0f);

    // NTP config
    static constexpr const char* NTP_SERVER1 = "192.168.0.1";
    static constexpr const char* NTP_SERVER2 = "time.nist.gov";
    String _timezone = "CST6CDT,M3.2.0,M11.1.0";
    static constexpr const char* NOT_AVAILABLE = "NA";
    String _wifiIPStr;

    FtpEnableCallback _ftpEnableCb;
    FtpDisableCallback _ftpDisableCb;
    FtpStatusCallback _ftpStatusCb;
    TempHistoryIntervalCallback _tempHistIntervalCb;
    DisplayConfigCallback _displayConfigCb;
    APStartCallback _apStartCb;
    APStopCallback _apStopCb;
    WeatherTopicCallback _weatherTopicCb;
    WeatherHttpCallback _weatherHttpCb;
    FailoverTestCallback _failoverTestCb;
    WeatherRefreshCallback _weatherRefreshCb;
    MqttConnectedCallback _mqttConnectedCb;
    bool* _ftpActivePtr = nullptr;
    unsigned long* _ftpStopTimePtr = nullptr;

    // OTA upload state (for chunked body handler)
    File _otaFile;
    bool _otaUploadOk = false;

    // WWW file upload state (for chunked body handler)
    File _wwwUploadFile;
    String _wwwUploadName;
    size_t _wwwUploadSize = 0;
    bool _wwwUploadOk = false;

    // WiFi test state
    String _wifiTestState = "idle";
    String _wifiTestMessage;
    String _wifiTestNewSSID;
    String _wifiTestNewPassword;
    String _wifiOldSSID;
    String _wifiOldPassword;
    Task* _tWifiTest = nullptr;
    uint8_t _wifiTestCountdown = 0;

    bool isRebootBlocked() const { return _rebootRateLimited && *_rebootRateLimited; }
    bool checkAuth(AsyncWebServerRequest* request);
    void redirectToLogin(AsyncWebServerRequest* request, bool expired);
    void syncNtpTime();
    void setupRoutes();
    void serveFile(AsyncWebServerRequest* request, const String& path);
    static const char* getContentType(const String& path);
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);
};

#endif
