#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include "SdFat.h"
#include "ArduinoJson.h"
#include "TempSensor.h"

struct ProjectInfo {
    String name;
    String createdOnDate;
    String description;
    String encrypt;
    bool encrypted;
    uint32_t maxLogSize;      // Max log file size in bytes before rotation
    uint8_t maxOldLogCount;   // Number of rotated log files to keep
    uint32_t heatRuntimeAccumulatedMs;  // Accumulated HEAT mode CNT runtime in ms
};

class Config {
  public:
    Config();

    // SD Card operations
    bool initSDCard();
    bool openConfigFile(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool loadTempConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool saveConfiguration(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool updateRuntime(const char* filename, uint32_t heatRuntimeMs);
    void clearConfig(TempSensorMap& config);

    // Getters for loaded config values
    String getWifiSSID() const { return _wifiSSID; }
    String getWifiPassword() const { return _wifiPassword; }
    IPAddress getMqttHost() const { return _mqttHost; }
    uint16_t getMqttPort() const { return _mqttPort; }
    String getMqttUser() const { return _mqttUser; }
    String getMqttPassword() const { return _mqttPassword; }

    // Setters for config values
    void setWifiSSID(const String& ssid) { _wifiSSID = ssid; }
    void setWifiPassword(const String& password) { _wifiPassword = password; }
    void setMqttHost(const IPAddress& host) { _mqttHost = host; }
    void setMqttPort(uint16_t port) { _mqttPort = port; }
    void setMqttUser(const String& user) { _mqttUser = user; }
    void setMqttPassword(const String& password) { _mqttPassword = password; }

    // SD card access
    SdFs* getSd() { return &_sd; }
    bool isSDCardInitialized() const { return _sdInitialized; }

    // Callback setter for temp sensor discovery
    typedef void (*TempSensorDiscoveryCallback)(TempSensorMap& config);
    void setTempSensorDiscoveryCallback(TempSensorDiscoveryCallback cb) { _tempDiscoveryCb = cb; }

  private:
    SdFs _sd;
    FsFile _configFile;
    bool _sdInitialized;

    // Config values
    String _wifiSSID;
    String _wifiPassword;
    IPAddress _mqttHost;
    uint16_t _mqttPort;
    String _mqttUser;
    String _mqttPassword;

    // Callback for discovering temp sensors when saving new config
    TempSensorDiscoveryCallback _tempDiscoveryCb;
};

#endif
