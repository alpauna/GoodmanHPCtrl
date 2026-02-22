#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include <SD.h>
#include "ArduinoJson.h"
#include "TempSensor.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"

struct I2CDeviceInfo {
    String driver;  // e.g. "MCP9600"
    String role;    // e.g. "LIQUID_TEMP"
};
typedef std::map<String, I2CDeviceInfo> I2CDeviceMap;

struct ProjectInfo {
    String name;
    String createdOnDate;
    String description;
    String encrypt;
    bool encrypted;
    uint32_t maxLogSize;      // Max log file size in bytes before rotation
    uint8_t maxOldLogCount;   // Number of rotated log files to keep
    uint32_t heatRuntimeAccumulatedMs;  // Accumulated HEAT mode CNT runtime in ms
    String timezone;             // POSIX TZ string (default "CST6CDT,M3.2.0,M11.1.0" = US Central with auto DST)
    // Heatpump protection settings (persisted in "heatpump" JSON section)
    float lowTempThreshold;              // Ambient temp below which compressor blocked (default 20°F)
    bool lowTempEnableW;                 // Enable W relay in LOW_TEMP state (default true)
    bool lowTempEnableAux;               // Enable AUX signal in LOW_TEMP state (default true)
    float highSuctionTempThreshold;      // Suction temp above which RV fail detected during defrost (default 140°F)
    bool rvFail;                         // Latched RV fail flag (persisted)
    uint32_t rvShortCycleMs;             // RV pressure equalization delay in defrost transition (default 30000)
    uint32_t cntShortCycleMs;            // CNT short cycle delay on Y activation (default 30000)
    uint32_t defrostMinRuntimeMs;        // Defrost minimum runtime in ms (default 180000 = 3 min)
    float defrostExitTempF;              // Condenser temp cutoff to end defrost (default 60.0°F)
    uint32_t heatRuntimeThresholdMs;     // Heat runtime threshold to trigger defrost in ms (default 5400000 = 90 min)
    bool softwareDefrost;                // Persisted software defrost state (survives reboot)
    uint32_t apFallbackSeconds;  // WiFi disconnect time before AP fallback (default 600 = 10 min)
    String apPassword;           // AP mode password override (empty = auto-generate)
    uint32_t tempHistoryIntervalSec; // Temp history capture interval in seconds (30-300, default 120)
    String theme;                // UI theme: "light" or "dark" (default "light")
    // Display settings
    uint32_t displayPageIntervalSec; // OLED page flip interval (3-60, default 10)
    bool displayEnabled;             // OLED display on/off (default true)
    // MAX6675 SPI thermocouple settings
    uint8_t max6675Clk;              // SPI CLK pin (default GPIO 39)
    uint8_t max6675Cs;               // SPI CS pin (default GPIO 40)
    uint8_t max6675Do;               // SPI DO/MISO pin (default GPIO 41)
    bool max6675Enabled;             // Enable MAX6675 sensor (default true)
    // Safe mode
    bool forceSafeMode;              // One-shot flag: force safe mode on next boot (cleared after entering)
    // System identity
    String systemName;               // System display name, max 20 chars alphanumeric+spaces (default "Goodman HP"), also used as AP SSID
    String mqttPrefix;               // MQTT topic prefix (default "goodman"), topics: prefix/temps, prefix/state, etc.
    uint32_t sessionTimeoutMinutes;  // Session timeout in minutes (0=disabled/Basic Auth, default 0)
};

class Config {
  public:
    Config();

    // SD Card operations
    bool initSDCard(bool formatIfFail = false);
    bool openConfigFile(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool loadTempConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool saveConfiguration(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    bool updateRuntime(const char* filename, uint32_t heatRuntimeMs, bool softwareDefrost);
    bool updateConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj);
    void clearConfig(TempSensorMap& config);
    bool formatSD(TempSensorMap& config, ProjectInfo& proj);
    String getSDInfo() const;

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
    bool generateSelfSignedCert();
    bool isCertExpired() const;
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
    static String generateRandomPassword(uint8_t length = 8);

    // I2C device assignments
    I2CDeviceMap& getI2CDevices() { return _i2cDevices; }
    const I2CDeviceMap& getI2CDevices() const { return _i2cDevices; }
    void setI2CDevice(const String& addr, const String& driver, const String& role);

    // Callback setter for temp sensor discovery
    typedef void (*TempSensorDiscoveryCallback)(TempSensorMap& config);
    void setTempSensorDiscoveryCallback(TempSensorDiscoveryCallback cb) { _tempDiscoveryCb = cb; }

  private:
    bool removeRecursive(const char* path);

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

    // I2C device assignments (persisted in sensors.i2c JSON section)
    I2CDeviceMap _i2cDevices;

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
