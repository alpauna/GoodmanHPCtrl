#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include "SdFat.h"
#include "ArduinoJson.h"

// Forward declarations
struct CurrentTemp;
struct ProjectInfo;

typedef std::map<String, CurrentTemp*> TempMap;

class Config {
  public:
    Config();

    // SD Card operations
    bool initSDCard();
    bool openConfigFile(const char* filename, TempMap& config, ProjectInfo& proj);
    bool loadTempConfig(const char* filename, TempMap& config);
    bool saveConfiguration(const char* filename, TempMap& config, ProjectInfo& proj);
    void clearConfig(TempMap& config);

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
    typedef void (*TempSensorDiscoveryCallback)(TempMap& config);
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
