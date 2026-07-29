#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include <SD.h>
#include "ArduinoJson.h"
#include "TempSensor.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"

struct SensorRange {
    float min;
    float max;
    float thresholdMin;  // NAN = not set
    float thresholdMax;  // NAN = not set
};
typedef std::map<String, SensorRange> SensorRangeMap;

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
    float highAmbientHeatLockoutF;       // Ambient temp above which HEAT is blocked (default 70°F)
    // Adaptive defrost bands (Cold / Mid / Warm)
    float defrostColdMaxTempF;              // Band breakpoint: ≤ this = Cold (default 23°F)
    float defrostWarmMinTempF;              // Band breakpoint: ≥ this = Warm (default 31°F)
    uint32_t defrostColdRuntimeMs;          // Cold: runtime threshold ms (default 1800000 = 30 min)
    uint32_t defrostColdMinRuntimeMs;       // Cold: min defrost ms (default 120000 = 2 min)
    float defrostColdExitTempF;             // Cold: condenser cutoff (default 45°F)
    uint32_t defrostMidRuntimeMs;           // Mid: runtime threshold ms (default 3600000 = 60 min)
    uint32_t defrostMidMinRuntimeMs;        // Mid: min defrost ms (default 180000 = 3 min)
    float defrostMidExitTempF;              // Mid: condenser cutoff (default 55°F)
    uint32_t defrostWarmRuntimeMs;          // Warm: runtime threshold ms (default 5400000 = 90 min)
    uint32_t defrostWarmMinRuntimeMs;       // Warm: min defrost ms (default 180000 = 3 min)
    float defrostWarmExitTempF;             // Warm: condenser cutoff (default 60°F)
    bool softwareDefrost;                // Persisted software defrost state (survives reboot)
    uint32_t stateValidationMs;          // State validation delay in ms (default 30000 = 30s)
    uint32_t inputDelayMs;               // Input pin debounce/validation delay in ms (default 10000 = 10s)
    uint32_t apFallbackSeconds;  // WiFi disconnect time before AP fallback (default 600 = 10 min)
    String apPassword;           // AP mode password override (empty = auto-generate)
    String ftpPassword;          // FTP server password (empty = default "admin")
    uint32_t tempHistoryIntervalSec; // Temp history capture interval in seconds (30-300, default 120)
    String theme;                // UI theme: "light" or "dark" (default "light")
    // Display settings
    uint32_t displayPageIntervalSec; // OLED page flip interval (3-60, default 10)
    bool displayEnabled;             // OLED display on/off (default true)
    // Safe mode
    bool forceSafeMode;              // One-shot flag: force safe mode on next boot (cleared after entering)
    // System identity
    String systemName;               // System display name, max 20 chars alphanumeric+spaces (default "Goodman HP"), also used as AP SSID
    String mqttPrefix;               // MQTT topic prefix (default "goodman"), topics: prefix/temps, prefix/state, etc.
    uint32_t sessionTimeoutMinutes;  // Session timeout in minutes (0=disabled/Basic Auth, default 0)
    uint8_t pollIntervalSec;         // Dashboard/pins polling interval in seconds (1-10, default 2)
    // Current monitoring (ADS1115 CT clamp)
    float compressorOvercurrentAmps; // Compressor overcurrent threshold in amps (0 = disabled)
    float fanOvercurrentAmps;        // Fan overcurrent threshold in amps (0 = disabled)
    uint32_t overcurrentDelayMs;     // How long overcurrent must persist before fault (default 5000)
    float lockedRotorThreshold;      // Locked rotor current threshold in amps (0 = disabled)
    uint32_t lockedRotorTimeoutMs;   // Max inrush settle time before locked rotor fault (default 5000)
    bool lockedRotorFault;           // Latched locked rotor fault (persisted, like rvFail)
    float compressorCtRatio;         // CT clamp ratio for compressor (default 30.0 = SCT-013-030)
    float fanCtRatio;                // CT clamp ratio for fan (default 30.0 = SCT-013-030)
    float crankcaseCtRatio;          // CT clamp ratio for crankcase heater (default 5.0 = SCT-013-005)
    float crankcaseExpectedAmps;     // Expected crankcase heater current in amps (0 = not monitored)
    float compressorBurdenOhms;      // Burden resistor for SCT-013-000 (0 = voltage-output CT, no burden)
    float fanBurdenOhms;             // Burden resistor for SCT-013-000 (0 = voltage-output CT, no burden)
    float crankcaseBurdenOhms;       // Burden resistor for SCT-013-000 (0 = voltage-output CT, no burden)
    // Weather ambient temperature fallback
    String weatherSource;            // "none", "mqtt", or "http" (default "none")
    String weatherMqttTopic;         // MQTT topic for weather temp (e.g., "homeassistant/sensor/outdoor_temp/state")
    String weatherApiKey;            // OpenWeatherMap API key
    String weatherZipCode;           // ZIP code (e.g., "73099")
    String weatherCountry;           // Country code (default "US")
    uint32_t weatherStaleMinutes;    // Max age before cached weather value expires (default 30)
    uint32_t weatherRefreshMinutes;  // HTTP fetch interval in minutes (default 10, range 1-60)
    float internalTempOffsetF;       // ESP32 internal temp offset in °F (default 0, range -50 to 50)
    // HP unit info
    float hpTonnage;                 // Heat pump tonnage (1.5, 2, 2.5, 3, 3.5, 4, 5; default 3)
    bool scrollCompressor;           // Scroll compressor type (default true)
    // CAN bus
    bool canEnabled;                 // Enable CAN bus for thermostat communication (default false)
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

    // Sensor display ranges (min/max for arc gauges)
    SensorRangeMap& getSensorRanges() { return _sensorRanges; }
    const SensorRangeMap& getSensorRanges() const { return _sensorRanges; }

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

    // Sensor display ranges (persisted in sensors.ranges JSON section)
    SensorRangeMap _sensorRanges;

    // ProjectInfo pointer for WebHandler access
    ProjectInfo* _proj;

    // AES-256-GCM encryption key (derived from eFuse HMAC)
    static uint8_t _aesKey[32];
    static bool _encryptionReady;

    // XOR obfuscation key (fallback when eFuse not available)
    static String _obfuscationKey;

};

#endif
