#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include <SD.h>
#include "ArduinoJson.h"
#include "TempSensor.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"

struct ProjectInfo {
    String name;
    String createdOnDate;
    String description;
    String encrypt;
    bool encrypted;
    uint32_t maxLogSize;      // Max log file size in bytes before rotation
    uint8_t maxOldLogCount;   // Number of rotated log files to keep
    uint32_t heatRuntimeAccumulatedMs;  // Accumulated HEAT mode CNT runtime in ms
    int32_t gmtOffsetSec;        // GMT offset in seconds (default -21600 = UTC-6)
    int32_t daylightOffsetSec;   // DST offset in seconds (default 3600 = 1hr)
    // Heatpump protection settings (persisted in "heatpump" JSON section)
    float lowTempThreshold;              // Ambient temp below which compressor blocked (default 20°F)
    float highSuctionTempThreshold;      // Suction temp above which RV fail detected during defrost (default 140°F)
    bool rvFail;                         // Latched RV fail flag (persisted)
    uint32_t rvShortCycleMs;             // RV pressure equalization delay in defrost transition (default 30000)
    uint32_t cntShortCycleMs;            // CNT short cycle delay on Y activation (default 30000)
    uint32_t apFallbackSeconds;  // WiFi disconnect time before AP fallback (default 600 = 10 min)
    uint32_t tempHistoryIntervalSec; // Temp history capture interval in seconds (30-300, default 120)
    String theme;                // UI theme: "light" or "dark" (default "light")
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
    bool updateConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj);
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

    // Admin password (encrypted with $AES$ or $ENC$, same as WiFi/MQTT)
    bool hasAdminPassword() const { return _adminPasswordHash.length() > 0; }
    void setAdminPassword(const String& plaintext);
    bool verifyAdminPassword(const String& plaintext) const;

    // Certificate loading for HTTPS
    bool loadCertificates(const char* certFile, const char* keyFile);
    bool hasCertificates() const { return _certBuf != nullptr && _keyBuf != nullptr; }
    const uint8_t* getCert() const { return _certBuf; }
    size_t getCertLen() const { return _certLen; }
    const uint8_t* getKey() const { return _keyBuf; }
    size_t getKeyLen() const { return _keyLen; }

    // SD card access
    bool isSDCardInitialized() const { return _sdInitialized; }

    // ProjectInfo access
    void setProjectInfo(ProjectInfo* proj) { _proj = proj; }
    ProjectInfo* getProjectInfo() { return _proj; }

    // Password encryption (AES-256-GCM with eFuse HMAC-derived key)
    // Falls back to XOR obfuscation ($ENC$) when eFuse key not available
    bool initEncryption();
    static bool isEncryptionReady() { return _encryptionReady; }
    static void setObfuscationKey(const String& key);
    static String encryptPassword(const String& plaintext);
    static String decryptPassword(const String& encrypted);

    // Callback setter for temp sensor discovery
    typedef void (*TempSensorDiscoveryCallback)(TempSensorMap& config);
    void setTempSensorDiscoveryCallback(TempSensorDiscoveryCallback cb) { _tempDiscoveryCb = cb; }

  private:
    fs::File _configFile;
    bool _sdInitialized;

    // Config values
    String _wifiSSID;
    String _wifiPassword;
    IPAddress _mqttHost;
    uint16_t _mqttPort;
    String _mqttUser;
    String _mqttPassword;
    String _adminPasswordHash;

    // Callback for discovering temp sensors when saving new config
    TempSensorDiscoveryCallback _tempDiscoveryCb;

    // ProjectInfo pointer for WebHandler access
    ProjectInfo* _proj;

    // AES-256-GCM encryption key (derived from eFuse HMAC)
    static uint8_t _aesKey[32];
    static bool _encryptionReady;

    // XOR obfuscation key (fallback when eFuse not available)
    static String _obfuscationKey;

    // HTTPS certificate buffers (PSRAM)
    uint8_t* _certBuf = nullptr;
    size_t _certLen = 0;
    uint8_t* _keyBuf = nullptr;
    size_t _keyLen = 0;
};

#endif
