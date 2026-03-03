#include "Config.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/bignum.h"

// External callbacks for temp sensors
extern void tempSensorUpdateCallback(TempSensor* sensor);
extern void tempSensorChangeCallback(TempSensor* sensor);

uint8_t Config::_aesKey[32] = {0};
bool Config::_encryptionReady = false;
String Config::_obfuscationKey = "";

void Config::setObfuscationKey(const String& key) {
    _obfuscationKey = key;
}

Config::Config()
    : _sdInitialized(false)
    , _mqttHost(192, 168, 0, 46)
    , _mqttPort(1883)
    , _mqttUser("debian")
    , _mqttPassword("")
    , _wifiSSID("")
    , _wifiPassword("")
    , _adminPasswordHash("")
    , _tempDiscoveryCb(nullptr)
    , _proj(nullptr)
{
}

bool Config::initEncryption() {
    static const uint8_t salt[] = "GoodmanHP-Config-Encrypt-v1";
    esp_err_t err = esp_hmac_calculate(HMAC_KEY0, salt, sizeof(salt) - 1, _aesKey);
    _encryptionReady = (err == ESP_OK);
    return _encryptionReady;
}

String Config::encryptPassword(const String& plaintext) {
    if (plaintext.length() == 0) return plaintext;

    // Fall back to XOR obfuscation when eFuse HMAC key is not available
    if (!_encryptionReady) {
        if (_obfuscationKey.length() == 0) return plaintext;
        size_t len = plaintext.length();
        uint8_t* xored = new uint8_t[len];
        for (size_t i = 0; i < len; i++) {
            xored[i] = plaintext[i] ^ _obfuscationKey[i % _obfuscationKey.length()];
        }
        size_t outLen = 0;
        mbedtls_base64_encode(nullptr, 0, &outLen, xored, len);
        uint8_t* b64 = new uint8_t[outLen + 1];
        mbedtls_base64_encode(b64, outLen + 1, &outLen, xored, len);
        b64[outLen] = '\0';
        String result = "$ENC$" + String((char*)b64);
        delete[] xored;
        delete[] b64;
        return result;
    }

    // Generate 12-byte random IV
    uint8_t iv[12];
    esp_fill_random(iv, sizeof(iv));

    size_t ptLen = plaintext.length();
    uint8_t* ciphertext = new uint8_t[ptLen];
    uint8_t tag[16];

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _aesKey, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, ptLen,
        iv, sizeof(iv), nullptr, 0,
        (const uint8_t*)plaintext.c_str(), ciphertext, sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);

    // Pack: IV[12] + ciphertext[N] + tag[16]
    size_t packedLen = 12 + ptLen + 16;
    uint8_t* packed = new uint8_t[packedLen];
    memcpy(packed, iv, 12);
    memcpy(packed + 12, ciphertext, ptLen);
    memcpy(packed + 12 + ptLen, tag, 16);
    delete[] ciphertext;

    // Base64 encode
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, packed, packedLen);
    uint8_t* b64 = new uint8_t[b64Len + 1];
    mbedtls_base64_encode(b64, b64Len + 1, &b64Len, packed, packedLen);
    b64[b64Len] = '\0';
    delete[] packed;

    String result = "$AES$" + String((char*)b64);
    delete[] b64;
    return result;
}

String Config::decryptPassword(const String& encrypted) {
    if (encrypted.startsWith("$AES$")) {
        if (!_encryptionReady) return "";

        String b64Part = encrypted.substring(5);
        size_t decodedLen = 0;
        mbedtls_base64_decode(nullptr, 0, &decodedLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());

        if (decodedLen < 12 + 16) return "";  // IV + tag minimum

        uint8_t* decoded = new uint8_t[decodedLen];
        mbedtls_base64_decode(decoded, decodedLen, &decodedLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());

        // Unpack: IV[12] + ciphertext[N] + tag[16]
        uint8_t* iv = decoded;
        size_t ctLen = decodedLen - 12 - 16;
        uint8_t* ciphertext = decoded + 12;
        uint8_t* tag = decoded + 12 + ctLen;

        uint8_t* plaintext = new uint8_t[ctLen + 1];

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _aesKey, 256);
        int ret = mbedtls_gcm_auth_decrypt(&gcm, ctLen,
            iv, 12, nullptr, 0, tag, 16,
            ciphertext, plaintext);
        mbedtls_gcm_free(&gcm);
        delete[] decoded;

        if (ret != 0) {
            delete[] plaintext;
            return "";  // Auth failed — tampered or wrong key
        }

        plaintext[ctLen] = '\0';
        String result = String((char*)plaintext);
        delete[] plaintext;
        return result;
    }

    if (encrypted.startsWith("$ENC$")) {
        if (_obfuscationKey.length() == 0) return encrypted;
        String b64Part = encrypted.substring(5);
        size_t outLen = 0;
        mbedtls_base64_decode(nullptr, 0, &outLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());
        uint8_t* decoded = new uint8_t[outLen + 1];
        mbedtls_base64_decode(decoded, outLen + 1, &outLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());
        for (size_t i = 0; i < outLen; i++) {
            decoded[i] ^= _obfuscationKey[i % _obfuscationKey.length()];
        }
        decoded[outLen] = '\0';
        String result = String((char*)decoded);
        delete[] decoded;
        return result;
    }

    // Plaintext passthrough
    return encrypted;
}

String Config::generateRandomPassword(uint8_t length) {
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    uint8_t buf[32];
    if (length > sizeof(buf)) length = sizeof(buf);
    esp_fill_random(buf, length);
    String result;
    result.reserve(length);
    for (uint8_t i = 0; i < length; i++) {
        result += charset[buf[i] % (sizeof(charset) - 1)];
    }
    return result;
}

void Config::setI2CDevice(const String& addr, const String& driver, const String& role) {
    if (driver.length() == 0) {
        _i2cDevices.erase(addr);
    } else {
        _i2cDevices[addr] = {driver, role};
    }
}

void Config::setAdminPassword(const String& plaintext) {
    _adminPasswordHash = plaintext;
}

bool Config::verifyAdminPassword(const String& plaintext) const {
    return plaintext == _adminPasswordHash;
}

bool Config::initSDCard(bool formatIfFail) {
    if (!SD.begin(SS, SPI, SD_SPI_SPEED * 1000000UL)) {
        if (formatIfFail) {
            Serial.println("\nSD mount failed, retrying with format_if_mount_failed...");
            if (!SD.begin(SS, SPI, SD_SPI_SPEED * 1000000UL, "/sd", 5, true)) {
                Serial.println("SD initialization failed even with format.");
                return false;
            }
            Serial.println("SD formatted and mounted successfully.");
            _sdInitialized = true;
            return true;
        }
        Serial.println("\nSD initialization failed.");
        Serial.println("Is the card correctly inserted?");
        Serial.println("Is chipSelect set to the correct value?");
        return false;
    }
    Serial.println("\nCard successfully initialized.\n");
    _sdInitialized = true;
    return true;
}

bool Config::removeRecursive(const char* path) {
    fs::File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        return SD.remove(path);
    }

    // Collect entries first (can't modify while iterating)
    String entries[64];
    bool isDir[64];
    int count = 0;

    fs::File entry = dir.openNextFile();
    while (entry && count < 64) {
        entries[count] = String(entry.name());
        isDir[count] = entry.isDirectory();
        entry.close();
        count++;
        entry = dir.openNextFile();
    }
    dir.close();

    // Delete entries
    for (int i = 0; i < count; i++) {
        String fullPath = entries[i];
        // SD library may return name with or without leading slash
        if (!fullPath.startsWith("/")) {
            fullPath = String(path) + "/" + fullPath;
        }
        if (isDir[i]) {
            removeRecursive(fullPath.c_str());
        } else {
            SD.remove(fullPath.c_str());
        }
    }

    // Remove the directory itself (skip root)
    if (strcmp(path, "/") != 0) {
        SD.rmdir(path);
    }
    return true;
}

bool Config::formatSD(TempSensorMap& config, ProjectInfo& proj) {
    if (!_sdInitialized) return false;

    Serial.println("FORMAT: Removing all files from SD card...");

    // Remove all entries in root
    fs::File root = SD.open("/");
    if (!root) return false;

    String entries[64];
    bool isDirFlags[64];
    int count = 0;

    fs::File entry = root.openNextFile();
    while (entry && count < 64) {
        entries[count] = String(entry.name());
        isDirFlags[count] = entry.isDirectory();
        entry.close();
        count++;
        entry = root.openNextFile();
    }
    root.close();

    for (int i = 0; i < count; i++) {
        String path = entries[i];
        if (!path.startsWith("/")) path = "/" + path;
        if (isDirFlags[i]) {
            removeRecursive(path.c_str());
        } else {
            SD.remove(path.c_str());
        }
    }

    Serial.println("FORMAT: Creating directory structure...");

    // Create required directories
    SD.mkdir("/temps");
    static const char* sensorDirs[] = {"ambient", "compressor", "suction", "condenser", "liquid"};
    for (int i = 0; i < 5; i++) {
        char dir[32];
        snprintf(dir, sizeof(dir), "/temps/%s", sensorDirs[i]);
        SD.mkdir(dir);
    }

    // Reset runtime state but preserve network credentials
    proj.heatRuntimeAccumulatedMs = 0;
    proj.rvFail = false;
    proj.softwareDefrost = false;

    // Restore certificates if they were loaded in memory
    if (_certBuf && _certLen > 0 && _keyBuf && _keyLen > 0) {
        fs::File cf = SD.open("/cert.pem", FILE_WRITE);
        if (cf) { cf.write(_certBuf, _certLen); cf.close(); }
        fs::File kf = SD.open("/key.pem", FILE_WRITE);
        if (kf) { kf.write(_keyBuf, _keyLen); kf.close(); }
        Serial.println("FORMAT: Restored certificates to SD card.");
    }

    Serial.println("FORMAT: Writing config (preserving WiFi/MQTT/admin credentials)...");

    // Write fresh config file
    bool ok = saveConfiguration("/config.txt", config, proj);

    Serial.println(ok ? "FORMAT: Complete." : "FORMAT: Failed to write config.");
    return ok;
}

String Config::getSDInfo() const {
    if (!_sdInitialized) {
        return "{\"error\":\"SD card not initialized\"}";
    }

    const char* cardType = "Unknown";
    switch (SD.cardType()) {
        case CARD_MMC:  cardType = "MMC"; break;
        case CARD_SD:   cardType = "SD"; break;
        case CARD_SDHC: cardType = "SDHC"; break;
        case CARD_NONE: cardType = "None"; break;
        default: break;
    }

    uint64_t totalBytes = SD.totalBytes();
    uint64_t usedBytes = SD.usedBytes();
    uint64_t freeBytes = totalBytes - usedBytes;

    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"%s\",\"totalMB\":%llu,\"usedMB\":%llu,\"freeMB\":%llu}",
        cardType, totalBytes / (1024 * 1024), usedBytes / (1024 * 1024), freeBytes / (1024 * 1024));
    return String(buf);
}

bool Config::openConfigFile(const char* filename, TempSensorMap& config, ProjectInfo& proj) {
    if (!SD.exists(filename)) {
        return saveConfiguration(filename, config, proj);
    }
    _configFile = SD.open(filename, FILE_READ);
    if (!_configFile || _configFile.size() == 0) {
        _configFile.close();
        return saveConfiguration(filename, config, proj);
    }
    _configFile.close();

    _configFile = SD.open(filename, FILE_READ);
    return (bool)_configFile;
}

bool Config::loadTempConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj) {
    if (!_configFile) {
        return false;
    }
    _configFile.seek(0);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, _configFile);

    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return false;
    }

    const char* project = doc["project"];
    const char* created = doc["created"];
    const char* description = doc["description"];
    proj.name = project != nullptr ? project : "";
    proj.createdOnDate = created != nullptr ? created : "";
    proj.description = description != nullptr ? description : "";

    JsonObject wifiObj = doc["wifi"];
    const char* wifi_ssid = wifiObj["ssid"];
    const char* wifi_password = wifiObj["password"];
    _wifiSSID = wifi_ssid != nullptr ? wifi_ssid : "";
    _wifiPassword = wifi_password != nullptr ? decryptPassword(wifi_password != nullptr ? wifi_password : "") : "";
    proj.apFallbackSeconds = wifiObj["apFallbackSeconds"] | 600;
    const char* apPw = wifiObj["apPassword"];
    proj.apPassword = (apPw != nullptr && strlen(apPw) > 0) ? decryptPassword(String(apPw)) : "";
    const char* ftpPw = doc["admin"]["ftpPassword"];
    proj.ftpPassword = (ftpPw != nullptr && strlen(ftpPw) > 0) ? decryptPassword(String(ftpPw)) : "";
    Serial.printf("Read WiFi SSID:%s apFallback:%us apPassword:%s ftpPassword:%s\n",
                  wifi_ssid ? wifi_ssid : "", proj.apFallbackSeconds,
                  proj.apPassword.length() > 0 ? "set" : "auto",
                  proj.ftpPassword.length() > 0 ? "set" : "default");

    JsonObject mqtt = doc["mqtt"];
    const char* mqtt_user = mqtt["user"];
    const char* mqtt_password = mqtt["password"];
    const char* mqtt_host = mqtt["host"];
    int mqtt_port = mqtt["port"];
    _mqttPort = mqtt_port;
    _mqttUser = mqtt_user != nullptr ? mqtt_user : "";
    _mqttPassword = mqtt_password != nullptr ? decryptPassword(mqtt_password != nullptr ? mqtt_password : "") : "";
    _mqttHost.fromString(mqtt_host != nullptr ? mqtt_host : "192.168.1.2");
    Serial.printf("Read mqtt Host:%s\n", _mqttHost.toString().c_str());

    // Load log rotation settings
    JsonObject logging = doc["logging"];
    proj.maxLogSize = logging["maxLogSize"] | (50 * 1024 * 1024);  // Default 50MB
    proj.maxOldLogCount = logging["maxOldLogCount"] | 10;          // Default 10 files
    Serial.printf("Read log settings: maxSize=%u maxOldCount=%d\n", proj.maxLogSize, (int)proj.maxOldLogCount);

    // Load heat runtime accumulation
    JsonObject runtime = doc["runtime"];
    proj.heatRuntimeAccumulatedMs = runtime["heatAccumulatedMs"] | 0;
    Serial.printf("Read heat runtime: %u ms\n", proj.heatRuntimeAccumulatedMs);

    // Load timezone settings (POSIX TZ string with migration from old numeric format)
    JsonObject timezone = doc["timezone"];
    if (timezone["posix"].is<const char*>()) {
        proj.timezone = timezone["posix"].as<String>();
    } else {
        // Migration: old numeric gmtOffset/daylightOffset → default POSIX string
        proj.timezone = "CST6CDT,M3.2.0,M11.1.0";
    }
    Serial.printf("Read timezone: %s\n", proj.timezone.c_str());

    // Load heatpump protection settings (with migration from old "lowTemp" root key)
    JsonObject heatpump = doc["heatpump"];
    if (heatpump.isNull() && doc["lowTemp"].is<JsonObject>()) {
        // Old format: migrate lowTemp.threshold into heatpump section
        proj.lowTempThreshold = doc["lowTemp"]["threshold"] | 20.0f;
        proj.lowTempEnableW = true;
        proj.lowTempEnableAux = true;
        proj.highSuctionTempThreshold = 140.0f;
        proj.rvFail = false;
        proj.rvShortCycleMs = 30000;
        proj.cntShortCycleMs = 30000;
        // Legacy defaults — Warm band values
        proj.defrostColdMaxTempF = 23.0f;
        proj.defrostWarmMinTempF = 31.0f;
        proj.defrostColdRuntimeMs = 1800000;   proj.defrostColdMinRuntimeMs = 120000;  proj.defrostColdExitTempF = 45.0f;
        proj.defrostMidRuntimeMs = 3600000;    proj.defrostMidMinRuntimeMs = 180000;   proj.defrostMidExitTempF = 55.0f;
        proj.defrostWarmRuntimeMs = 5400000;   proj.defrostWarmMinRuntimeMs = 180000;  proj.defrostWarmExitTempF = 60.0f;
        proj.softwareDefrost = false;
        proj.stateValidationMs = 30000;
        proj.inputDelayMs = 2000;
        proj.compressorOvercurrentAmps = 0;
        proj.fanOvercurrentAmps = 0;
        proj.overcurrentDelayMs = 5000;
        proj.lockedRotorThreshold = 0;
        proj.lockedRotorTimeoutMs = 5000;
        proj.lockedRotorFault = false;
        Serial.println("Config migration: old lowTemp format detected, will migrate on next save");
    } else {
        proj.lowTempThreshold = heatpump["lowTemp"]["threshold"] | 20.0f;
        proj.lowTempEnableW = heatpump["lowTemp"]["enableW"] | true;
        proj.lowTempEnableAux = heatpump["lowTemp"]["enableAux"] | true;
        proj.highSuctionTempThreshold = heatpump["highSuctionTemp"]["threshold"] | 140.0f;
        proj.rvFail = heatpump["highSuctionTemp"]["rvFail"] | false;
        proj.rvShortCycleMs = heatpump["shortCycle"]["rv"] | 30000;
        proj.cntShortCycleMs = heatpump["shortCycle"]["cnt"] | 30000;
        // Adaptive defrost bands — migrate from old single-value fields if needed
        JsonObject defrost = heatpump["defrost"];
        if (defrost["cold"].is<JsonObject>()) {
            // New band format
            proj.defrostColdMaxTempF = defrost["coldMaxTemp"] | 23.0f;
            proj.defrostWarmMinTempF = defrost["warmMinTemp"] | 31.0f;
            proj.defrostColdRuntimeMs = defrost["cold"]["runtimeThresholdMs"] | 1800000;
            proj.defrostColdMinRuntimeMs = defrost["cold"]["minRuntimeMs"] | 120000;
            proj.defrostColdExitTempF = defrost["cold"]["exitTempF"] | 45.0f;
            proj.defrostMidRuntimeMs = defrost["mid"]["runtimeThresholdMs"] | 3600000;
            proj.defrostMidMinRuntimeMs = defrost["mid"]["minRuntimeMs"] | 180000;
            proj.defrostMidExitTempF = defrost["mid"]["exitTempF"] | 55.0f;
            proj.defrostWarmRuntimeMs = defrost["warm"]["runtimeThresholdMs"] | 5400000;
            proj.defrostWarmMinRuntimeMs = defrost["warm"]["minRuntimeMs"] | 180000;
            proj.defrostWarmExitTempF = defrost["warm"]["exitTempF"] | 60.0f;
        } else {
            // Migrate from old single-value fields: old values become Warm band
            uint32_t oldRuntimeMs = defrost["heatRuntimeThresholdMs"] | 5400000;
            uint32_t oldMinMs = defrost["minRuntimeMs"] | 180000;
            float oldExitF = defrost["exitTempF"] | 60.0f;
            proj.defrostColdMaxTempF = 23.0f;
            proj.defrostWarmMinTempF = 31.0f;
            // Warm = old values; Mid/Cold derived proportionally
            proj.defrostWarmRuntimeMs = oldRuntimeMs;
            proj.defrostWarmMinRuntimeMs = oldMinMs;
            proj.defrostWarmExitTempF = oldExitF;
            proj.defrostMidRuntimeMs = (oldRuntimeMs * 2) / 3;  // ~67% of warm
            proj.defrostMidMinRuntimeMs = oldMinMs;
            proj.defrostMidExitTempF = oldExitF - 5.0f;
            proj.defrostColdRuntimeMs = oldRuntimeMs / 3;        // ~33% of warm
            proj.defrostColdMinRuntimeMs = oldMinMs > 60000 ? oldMinMs - 60000 : oldMinMs;  // 1 min less
            proj.defrostColdExitTempF = oldExitF - 15.0f;
            Serial.println("Config migration: old single-value defrost -> band format");
        }
        proj.softwareDefrost = defrost["active"] | false;
        proj.stateValidationMs = heatpump["stateValidation"]["delayMs"] | 30000;
        proj.inputDelayMs = heatpump["inputDelay"]["ms"] | 2000;
        // Current monitoring
        JsonObject hpCurrent = heatpump["current"];
        proj.compressorOvercurrentAmps = hpCurrent["compressorOvercurrentAmps"] | 0.0f;
        proj.fanOvercurrentAmps = hpCurrent["fanOvercurrentAmps"] | 0.0f;
        proj.overcurrentDelayMs = hpCurrent["overcurrentDelayMs"] | 5000;
        proj.lockedRotorThreshold = hpCurrent["lockedRotorThreshold"] | 0.0f;
        proj.lockedRotorTimeoutMs = hpCurrent["lockedRotorTimeoutMs"] | 5000;
        proj.lockedRotorFault = hpCurrent["lockedRotorFault"] | false;
    }

    // Defensive clamping for defrost band values (catches corrupt SD data)
    if (proj.defrostColdMaxTempF < proj.lowTempThreshold) proj.defrostColdMaxTempF = proj.lowTempThreshold;
    if (proj.defrostWarmMinTempF > 35.0f) proj.defrostWarmMinTempF = 35.0f;
    if (proj.defrostColdMaxTempF >= proj.defrostWarmMinTempF) proj.defrostColdMaxTempF = proj.defrostWarmMinTempF - 1.0f;
    // Per-band runtime: min 15 minutes (900000 ms)
    if (proj.defrostColdRuntimeMs < 900000) proj.defrostColdRuntimeMs = 900000;
    if (proj.defrostMidRuntimeMs < 900000) proj.defrostMidRuntimeMs = 900000;
    if (proj.defrostWarmRuntimeMs < 900000) proj.defrostWarmRuntimeMs = 900000;
    // Per-band exit temp: 36-65°F
    auto clampExitTemp = [](float& v) { if (v < 36.0f) v = 36.0f; if (v > 65.0f) v = 65.0f; };
    clampExitTemp(proj.defrostColdExitTempF);
    clampExitTemp(proj.defrostMidExitTempF);
    clampExitTemp(proj.defrostWarmExitTempF);

    Serial.printf("Read heatpump: lowTemp=%.1fF highSuct=%.1fF rvFail=%d rvSC=%lu cntSC=%lu\n",
                  proj.lowTempThreshold, proj.highSuctionTempThreshold, proj.rvFail,
                  proj.rvShortCycleMs, proj.cntShortCycleMs);
    Serial.printf("  Defrost bands: cold(%lum/%lus/%.0fF) mid(%lum/%lus/%.0fF) warm(%lum/%lus/%.0fF) breaks=%.0f/%.0fF\n",
                  proj.defrostColdRuntimeMs/60000UL, proj.defrostColdMinRuntimeMs/1000UL, proj.defrostColdExitTempF,
                  proj.defrostMidRuntimeMs/60000UL, proj.defrostMidMinRuntimeMs/1000UL, proj.defrostMidExitTempF,
                  proj.defrostWarmRuntimeMs/60000UL, proj.defrostWarmMinRuntimeMs/1000UL, proj.defrostWarmExitTempF,
                  proj.defrostColdMaxTempF, proj.defrostWarmMinTempF);

    // Load temp history capture interval
    proj.tempHistoryIntervalSec = doc["tempHistory"]["intervalSec"] | 120;
    if (proj.tempHistoryIntervalSec < 30) proj.tempHistoryIntervalSec = 30;
    if (proj.tempHistoryIntervalSec > 300) proj.tempHistoryIntervalSec = 300;
    Serial.printf("Read tempHistory interval: %us\n", proj.tempHistoryIntervalSec);

    // Load UI theme
    const char* uiTheme = doc["ui"]["theme"];
    proj.theme = (uiTheme != nullptr) ? String(uiTheme) : "dark";
    Serial.printf("Read UI theme: %s\n", proj.theme.c_str());

    // Load display settings
    proj.displayPageIntervalSec = doc["display"]["pageIntervalSec"] | 10;
    if (proj.displayPageIntervalSec < 3) proj.displayPageIntervalSec = 3;
    if (proj.displayPageIntervalSec > 60) proj.displayPageIntervalSec = 60;
    proj.displayEnabled = doc["display"]["enabled"] | true;
    Serial.printf("Read display: interval=%us enabled=%d\n", proj.displayPageIntervalSec, proj.displayEnabled);

    // Load MAX6675 SPI thermocouple settings
    JsonObject max6675Obj = doc["sensors"]["max6675"];
    proj.max6675Clk = max6675Obj["clk"] | 39;
    proj.max6675Cs = max6675Obj["cs"] | 40;
    proj.max6675Do = max6675Obj["do"] | 41;
    proj.max6675Enabled = max6675Obj["enabled"] | true;
    Serial.printf("Read MAX6675: clk=%d cs=%d do=%d enabled=%d\n",
                  proj.max6675Clk, proj.max6675Cs, proj.max6675Do, proj.max6675Enabled);

    // Load safe mode flag
    proj.forceSafeMode = doc["safeMode"]["force"] | false;

    // Load system identity
    JsonObject systemObj = doc["system"];
    const char* sysName = systemObj["name"];
    proj.systemName = (sysName != nullptr && strlen(sysName) > 0) ? String(sysName) : "Goodman HP";
    const char* mqttPfx = systemObj["mqttPrefix"];
    proj.mqttPrefix = (mqttPfx != nullptr && strlen(mqttPfx) > 0) ? String(mqttPfx) : "goodman";
    Serial.printf("Read system: name=%s mqttPrefix=%s\n", proj.systemName.c_str(), proj.mqttPrefix.c_str());

    // Load session timeout
    proj.sessionTimeoutMinutes = doc["auth"]["sessionTimeoutMinutes"] | 0;
    Serial.printf("Read session timeout: %u min\n", proj.sessionTimeoutMinutes);

    // Load poll interval
    proj.pollIntervalSec = doc["ui"]["pollIntervalSec"] | 2;
    if (proj.pollIntervalSec < 1) proj.pollIntervalSec = 1;
    if (proj.pollIntervalSec > 10) proj.pollIntervalSec = 10;

    // Load weather ambient fallback settings
    JsonObject weatherObj = doc["weather"];
    const char* wSrc = weatherObj["source"];
    proj.weatherSource = (wSrc != nullptr) ? String(wSrc) : "none";
    const char* wTopic = weatherObj["mqttTopic"];
    proj.weatherMqttTopic = (wTopic != nullptr) ? String(wTopic) : "";
    const char* wApiKey = weatherObj["apiKey"];
    proj.weatherApiKey = (wApiKey != nullptr && strlen(wApiKey) > 0) ? decryptPassword(String(wApiKey)) : "";
    const char* wZip = weatherObj["zipCode"];
    proj.weatherZipCode = (wZip != nullptr) ? String(wZip) : "";
    const char* wCountry = weatherObj["country"];
    proj.weatherCountry = (wCountry != nullptr && strlen(wCountry) > 0) ? String(wCountry) : "US";
    proj.weatherStaleMinutes = weatherObj["staleMinutes"] | 30;
    proj.weatherRefreshMinutes = weatherObj["refreshMinutes"] | 10;
    if (proj.weatherRefreshMinutes < 1) proj.weatherRefreshMinutes = 1;
    if (proj.weatherRefreshMinutes > 60) proj.weatherRefreshMinutes = 60;
    proj.internalTempOffsetF = weatherObj["internalTempOffsetF"] | 0.0f;
    // Enforce stale >= 2x refresh
    uint32_t minStale = proj.weatherRefreshMinutes * 2;
    if (proj.weatherStaleMinutes < minStale) proj.weatherStaleMinutes = minStale;
    Serial.printf("Read weather: source=%s topic=%s zip=%s country=%s stale=%umin refresh=%umin\n",
                  proj.weatherSource.c_str(), proj.weatherMqttTopic.c_str(),
                  proj.weatherZipCode.c_str(), proj.weatherCountry.c_str(),
                  proj.weatherStaleMinutes, proj.weatherRefreshMinutes);

    // Load admin password (encrypted same as WiFi/MQTT passwords)
    const char* adminPw = doc["admin"]["password"];
    String adminPwStr = (adminPw != nullptr && strlen(adminPw) > 0) ? String(adminPw) : "";
    _adminPasswordHash = decryptPassword(adminPwStr);
    Serial.printf("Admin password: %s\n", _adminPasswordHash.length() > 0 ? "set" : "not set");

    // Load I2C device assignments
    _i2cDevices.clear();
    JsonObject i2cObj = doc["sensors"]["i2c"];
    for (JsonPair kv : i2cObj) {
        String addr = kv.key().c_str();
        String driver = kv.value()["driver"] | String("");
        String role = kv.value()["role"] | String("");
        if (driver.length() > 0) {
            _i2cDevices[addr] = {driver, role};
            Serial.printf("I2C device %s: driver=%s role=%s\n", addr.c_str(), driver.c_str(), role.c_str());
        }
    }

    // Load sensor display ranges
    _sensorRanges.clear();
    JsonObject ranges = doc["sensors"]["ranges"];
    for (JsonPair kv : ranges) {
        SensorRange r;
        r.min = kv.value()["min"] | 0.0f;
        r.max = kv.value()["max"] | 100.0f;
        r.thresholdMin = kv.value()["thresholdMin"].isNull() ? NAN : kv.value()["thresholdMin"].as<float>();
        r.thresholdMax = kv.value()["thresholdMax"].isNull() ? NAN : kv.value()["thresholdMax"].as<float>();
        _sensorRanges[kv.key().c_str()] = r;
    }

    clearConfig(config);
    for (JsonPair sensors_temp_item : doc["sensors"]["temp"].as<JsonObject>()) {
        const char* key = sensors_temp_item.key().c_str();
        const char* desc = sensors_temp_item.value()["description"];
        int last_value = sensors_temp_item.value()["last-value"];
        const char* name = sensors_temp_item.value()["name"];
        Serial.printf("Key:%s\tDescription:%s\tLast Value:%d\n", key, desc, last_value);
        Serial.printf("Name: %s\n", name);

        TempSensor* sensor = new TempSensor(String(desc));
        config[name] = sensor;

        String devaddrStr = String(key);
        Serial.printf("Devstr:%s\n", devaddrStr.c_str());
        TempSensor::stringToAddress(devaddrStr, sensor->getDeviceAddress());

        sensor->setValue(last_value);
        sensor->setPrevious(sensor->getValue());
        sensor->setValid(false);  // Not valid until first real bus read
        sensor->setChangeCallback(tempSensorChangeCallback);
        sensor->setUpdateCallback(tempSensorUpdateCallback);

        Serial.printf("JSON description: %s\tID:%s\t Value:%.1f\n",
             sensor->getDescription().c_str(),
             TempSensor::addressToString(sensor->getDeviceAddress()).c_str(),
             sensor->getValue());
    }
    _configFile.close();

    // Apply default sensor ranges for any sensor missing a range entry
    struct DefaultRange { const char* name; float min; float max; };
    static const DefaultRange defaultRanges[] = {
        {"COMPRESSOR_TEMP", 50, 300},
        {"SUCTION_TEMP", 20, 80},
        {"AMBIENT_TEMP", -20, 120},
        {"CONDENSER_TEMP", 30, 150},
        {"LIQUID_TEMP", 60, 250}
    };
    for (auto& mp : config) {
        if (_sensorRanges.count(mp.first) == 0) {
            SensorRange r = {0, 100, NAN, NAN};  // generic default, no thresholds
            for (const auto& dr : defaultRanges) {
                if (mp.first == dr.name) {
                    r.min = dr.min;
                    r.max = dr.max;
                    break;
                }
            }
            _sensorRanges[mp.first] = r;
        }
    }

    return true;
}

void Config::clearConfig(TempSensorMap& config) {
    for (auto& pair : config) {
        if (pair.second != nullptr) {
            delete pair.second;
        }
    }
    config.clear();
}

bool Config::saveConfiguration(const char* filename, TempSensorMap& config, ProjectInfo& proj) {
    if (SD.exists(filename)) {
        _configFile = SD.open(filename, FILE_READ);
        if (_configFile && _configFile.size() > 0) {
            _configFile.close();
            return false;
        }
        _configFile.close();
    }
    _configFile = SD.open(filename, FILE_WRITE);
    if (!_configFile) {
        Serial.printf("open failed: \"%s\"\n", filename);
        return false;
    }
    JsonDocument doc;

    doc["project"] = proj.name;
    doc["created"] = proj.createdOnDate;
    doc["description"] = proj.description;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = "MEGA";
    wifi["password"] = "";
    wifi["apFallbackSeconds"] = proj.apFallbackSeconds;

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["user"] = "debian";
    mqtt["password"] = "";
    mqtt["host"] = "192.168.1.1";
    mqtt["port"] = 1883;

    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["maxLogSize"] = proj.maxLogSize;
    logging["maxOldLogCount"] = proj.maxOldLogCount;

    JsonObject runtime = doc["runtime"].to<JsonObject>();
    runtime["heatAccumulatedMs"] = proj.heatRuntimeAccumulatedMs;

    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["posix"] = proj.timezone.length() > 0 ? proj.timezone : "CST6CDT,M3.2.0,M11.1.0";

    JsonObject heatpump = doc["heatpump"].to<JsonObject>();
    JsonObject hpLowTemp = heatpump["lowTemp"].to<JsonObject>();
    hpLowTemp["threshold"] = proj.lowTempThreshold;
    hpLowTemp["enableW"] = proj.lowTempEnableW;
    hpLowTemp["enableAux"] = proj.lowTempEnableAux;
    JsonObject hpHighSuction = heatpump["highSuctionTemp"].to<JsonObject>();
    hpHighSuction["threshold"] = proj.highSuctionTempThreshold;
    hpHighSuction["rvFail"] = proj.rvFail;
    JsonObject hpShortCycle = heatpump["shortCycle"].to<JsonObject>();
    hpShortCycle["rv"] = proj.rvShortCycleMs;
    hpShortCycle["cnt"] = proj.cntShortCycleMs;
    JsonObject hpDefrost = heatpump["defrost"].to<JsonObject>();
    hpDefrost["active"] = proj.softwareDefrost;
    hpDefrost["coldMaxTemp"] = proj.defrostColdMaxTempF;
    hpDefrost["warmMinTemp"] = proj.defrostWarmMinTempF;
    JsonObject dfCold = hpDefrost["cold"].to<JsonObject>();
    dfCold["runtimeThresholdMs"] = proj.defrostColdRuntimeMs;
    dfCold["minRuntimeMs"] = proj.defrostColdMinRuntimeMs;
    dfCold["exitTempF"] = proj.defrostColdExitTempF;
    JsonObject dfMid = hpDefrost["mid"].to<JsonObject>();
    dfMid["runtimeThresholdMs"] = proj.defrostMidRuntimeMs;
    dfMid["minRuntimeMs"] = proj.defrostMidMinRuntimeMs;
    dfMid["exitTempF"] = proj.defrostMidExitTempF;
    JsonObject dfWarm = hpDefrost["warm"].to<JsonObject>();
    dfWarm["runtimeThresholdMs"] = proj.defrostWarmRuntimeMs;
    dfWarm["minRuntimeMs"] = proj.defrostWarmMinRuntimeMs;
    dfWarm["exitTempF"] = proj.defrostWarmExitTempF;
    JsonObject hpStateVal = heatpump["stateValidation"].to<JsonObject>();
    hpStateVal["delayMs"] = proj.stateValidationMs;
    JsonObject hpInputDelay = heatpump["inputDelay"].to<JsonObject>();
    hpInputDelay["ms"] = proj.inputDelayMs;

    JsonObject tempHistObj = doc["tempHistory"].to<JsonObject>();
    tempHistObj["intervalSec"] = proj.tempHistoryIntervalSec;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";
    ui["pollIntervalSec"] = proj.pollIntervalSec;

    JsonObject displayObj = doc["display"].to<JsonObject>();
    displayObj["pageIntervalSec"] = proj.displayPageIntervalSec;
    displayObj["enabled"] = proj.displayEnabled;

    JsonObject safeModeObj = doc["safeMode"].to<JsonObject>();
    safeModeObj["force"] = proj.forceSafeMode;

    JsonObject systemObj = doc["system"].to<JsonObject>();
    systemObj["name"] = proj.systemName.length() > 0 ? proj.systemName : "Goodman HP";
    systemObj["mqttPrefix"] = proj.mqttPrefix.length() > 0 ? proj.mqttPrefix : "goodman";

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["sessionTimeoutMinutes"] = proj.sessionTimeoutMinutes;

    // Weather ambient fallback
    JsonObject weatherObj = doc["weather"].to<JsonObject>();
    weatherObj["source"] = proj.weatherSource.length() > 0 ? proj.weatherSource : "none";
    weatherObj["mqttTopic"] = proj.weatherMqttTopic;
    if (proj.weatherApiKey.length() > 0) {
        weatherObj["apiKey"] = encryptPassword(proj.weatherApiKey);
    }
    weatherObj["zipCode"] = proj.weatherZipCode;
    weatherObj["country"] = proj.weatherCountry.length() > 0 ? proj.weatherCountry : "US";
    weatherObj["staleMinutes"] = proj.weatherStaleMinutes;
    weatherObj["refreshMinutes"] = proj.weatherRefreshMinutes;
    weatherObj["internalTempOffsetF"] = serialized(String(proj.internalTempOffsetF, 1));

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = "";
    if (proj.ftpPassword.length() > 0) {
        admin["ftpPassword"] = encryptPassword(proj.ftpPassword);
    }

    JsonObject sensors = doc["sensors"].to<JsonObject>();
    JsonObject sensors_temp = sensors["temp"].to<JsonObject>();

    // If no temp sensors in config, try to discover them
    if (config.size() == 0 && _tempDiscoveryCb != nullptr) {
        _tempDiscoveryCb(config);
    }

    for (auto& mp : config) {
        String id = TempSensor::addressToString(mp.second->getDeviceAddress());
        JsonObject temp = sensors_temp[id].to<JsonObject>();
        temp["description"] = mp.second->getDescription();
        temp["last-value"] = mp.second->getValue();
        temp["name"] = mp.first;
    }

    // Write I2C device assignments
    JsonObject i2cObj = sensors["i2c"].to<JsonObject>();
    for (auto& kv : _i2cDevices) {
        JsonObject dev = i2cObj[kv.first].to<JsonObject>();
        dev["driver"] = kv.second.driver;
        dev["role"] = kv.second.role;
    }

    // Write MAX6675 SPI thermocouple settings
    JsonObject max6675Obj = sensors["max6675"].to<JsonObject>();
    max6675Obj["clk"] = proj.max6675Clk;
    max6675Obj["cs"] = proj.max6675Cs;
    max6675Obj["do"] = proj.max6675Do;
    max6675Obj["enabled"] = proj.max6675Enabled;

    // Write sensor display ranges
    JsonObject rangesObj = sensors["ranges"].to<JsonObject>();
    for (auto& kv : _sensorRanges) {
        JsonObject r = rangesObj[kv.first].to<JsonObject>();
        r["min"] = kv.second.min;
        r["max"] = kv.second.max;
        if (!isnan(kv.second.thresholdMin)) r["thresholdMin"] = kv.second.thresholdMin;
        if (!isnan(kv.second.thresholdMax)) r["thresholdMax"] = kv.second.thresholdMax;
    }

    String output;
    serializeJson(doc, _configFile);
    serializeJsonPretty(doc, output);
    Serial.println("Temp sensor as json...");
    Serial.println(output);
    _configFile.close();
    return true;
}

bool Config::updateConfig(const char* filename, TempSensorMap& config, ProjectInfo& proj) {
    if (!_sdInitialized) {
        return false;
    }

    fs::File file = SD.open(filename, FILE_READ);
    if (!file) {
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        return false;
    }

    // Update all config fields
    doc["project"] = proj.name;
    doc["description"] = proj.description;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = _wifiSSID;
    wifi["password"] = encryptPassword(_wifiPassword);
    wifi["apFallbackSeconds"] = proj.apFallbackSeconds;
    if (proj.apPassword.length() > 0) {
        wifi["apPassword"] = encryptPassword(proj.apPassword);
    } else {
        wifi.remove("apPassword");
    }

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["user"] = _mqttUser;
    mqtt["password"] = encryptPassword(_mqttPassword);
    mqtt["host"] = _mqttHost.toString();
    mqtt["port"] = _mqttPort;

    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["maxLogSize"] = proj.maxLogSize;
    logging["maxOldLogCount"] = proj.maxOldLogCount;

    JsonObject runtime = doc["runtime"].to<JsonObject>();
    runtime["heatAccumulatedMs"] = proj.heatRuntimeAccumulatedMs;

    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["posix"] = proj.timezone.length() > 0 ? proj.timezone : "CST6CDT,M3.2.0,M11.1.0";
    timezone.remove("gmtOffset");
    timezone.remove("daylightOffset");

    // Remove old root-level lowTemp key (migration cleanup)
    doc.remove("lowTemp");

    JsonObject heatpump = doc["heatpump"].to<JsonObject>();
    JsonObject hpLowTemp = heatpump["lowTemp"].to<JsonObject>();
    hpLowTemp["threshold"] = proj.lowTempThreshold;
    hpLowTemp["enableW"] = proj.lowTempEnableW;
    hpLowTemp["enableAux"] = proj.lowTempEnableAux;
    JsonObject hpHighSuction = heatpump["highSuctionTemp"].to<JsonObject>();
    hpHighSuction["threshold"] = proj.highSuctionTempThreshold;
    hpHighSuction["rvFail"] = proj.rvFail;
    JsonObject hpShortCycle = heatpump["shortCycle"].to<JsonObject>();
    hpShortCycle["rv"] = proj.rvShortCycleMs;
    hpShortCycle["cnt"] = proj.cntShortCycleMs;
    JsonObject hpDefrost = heatpump["defrost"].to<JsonObject>();
    hpDefrost["active"] = proj.softwareDefrost;
    hpDefrost["coldMaxTemp"] = proj.defrostColdMaxTempF;
    hpDefrost["warmMinTemp"] = proj.defrostWarmMinTempF;
    // Remove old single-value keys (migration cleanup)
    hpDefrost.remove("minRuntimeMs");
    hpDefrost.remove("exitTempF");
    hpDefrost.remove("heatRuntimeThresholdMs");
    JsonObject dfColdUpd = hpDefrost["cold"].to<JsonObject>();
    dfColdUpd["runtimeThresholdMs"] = proj.defrostColdRuntimeMs;
    dfColdUpd["minRuntimeMs"] = proj.defrostColdMinRuntimeMs;
    dfColdUpd["exitTempF"] = proj.defrostColdExitTempF;
    JsonObject dfMidUpd = hpDefrost["mid"].to<JsonObject>();
    dfMidUpd["runtimeThresholdMs"] = proj.defrostMidRuntimeMs;
    dfMidUpd["minRuntimeMs"] = proj.defrostMidMinRuntimeMs;
    dfMidUpd["exitTempF"] = proj.defrostMidExitTempF;
    JsonObject dfWarmUpd = hpDefrost["warm"].to<JsonObject>();
    dfWarmUpd["runtimeThresholdMs"] = proj.defrostWarmRuntimeMs;
    dfWarmUpd["minRuntimeMs"] = proj.defrostWarmMinRuntimeMs;
    dfWarmUpd["exitTempF"] = proj.defrostWarmExitTempF;
    JsonObject hpStateValUpd = heatpump["stateValidation"].to<JsonObject>();
    hpStateValUpd["delayMs"] = proj.stateValidationMs;
    JsonObject hpInputDelayUpd = heatpump["inputDelay"].to<JsonObject>();
    hpInputDelayUpd["ms"] = proj.inputDelayMs;
    // Current monitoring
    JsonObject hpCurrentUpd = heatpump["current"].to<JsonObject>();
    hpCurrentUpd["compressorOvercurrentAmps"] = proj.compressorOvercurrentAmps;
    hpCurrentUpd["fanOvercurrentAmps"] = proj.fanOvercurrentAmps;
    hpCurrentUpd["overcurrentDelayMs"] = proj.overcurrentDelayMs;
    hpCurrentUpd["lockedRotorThreshold"] = proj.lockedRotorThreshold;
    hpCurrentUpd["lockedRotorTimeoutMs"] = proj.lockedRotorTimeoutMs;
    hpCurrentUpd["lockedRotorFault"] = proj.lockedRotorFault;

    JsonObject tempHistObj = doc["tempHistory"].to<JsonObject>();
    tempHistObj["intervalSec"] = proj.tempHistoryIntervalSec;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";
    ui["pollIntervalSec"] = proj.pollIntervalSec;

    JsonObject displayObj = doc["display"].to<JsonObject>();
    displayObj["pageIntervalSec"] = proj.displayPageIntervalSec;
    displayObj["enabled"] = proj.displayEnabled;

    JsonObject safeModeUpd = doc["safeMode"].to<JsonObject>();
    safeModeUpd["force"] = proj.forceSafeMode;

    JsonObject systemObj = doc["system"].to<JsonObject>();
    systemObj["name"] = proj.systemName.length() > 0 ? proj.systemName : "Goodman HP";
    systemObj["mqttPrefix"] = proj.mqttPrefix.length() > 0 ? proj.mqttPrefix : "goodman";

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["sessionTimeoutMinutes"] = proj.sessionTimeoutMinutes;

    // Weather ambient fallback
    JsonObject weatherUpd = doc["weather"].to<JsonObject>();
    weatherUpd["source"] = proj.weatherSource.length() > 0 ? proj.weatherSource : "none";
    weatherUpd["mqttTopic"] = proj.weatherMqttTopic;
    if (proj.weatherApiKey.length() > 0) {
        weatherUpd["apiKey"] = encryptPassword(proj.weatherApiKey);
    } else {
        weatherUpd.remove("apiKey");
    }
    weatherUpd["zipCode"] = proj.weatherZipCode;
    weatherUpd["country"] = proj.weatherCountry.length() > 0 ? proj.weatherCountry : "US";
    weatherUpd["staleMinutes"] = proj.weatherStaleMinutes;
    weatherUpd["refreshMinutes"] = proj.weatherRefreshMinutes;
    weatherUpd["internalTempOffsetF"] = serialized(String(proj.internalTempOffsetF, 1));

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = encryptPassword(_adminPasswordHash);
    if (proj.ftpPassword.length() > 0) {
        admin["ftpPassword"] = encryptPassword(proj.ftpPassword);
    } else {
        admin.remove("ftpPassword");
    }

    // Update temp sensor assignments (to<JsonObject>() clears existing content)
    JsonObject sensors = doc["sensors"].to<JsonObject>();
    JsonObject sensors_temp = sensors["temp"].to<JsonObject>();
    for (auto& mp : config) {
        String id = TempSensor::addressToString(mp.second->getDeviceAddress());
        JsonObject temp = sensors_temp[id].to<JsonObject>();
        temp["description"] = mp.second->getDescription();
        temp["last-value"] = mp.second->getValue();
        temp["name"] = mp.first;
    }

    // Write I2C device assignments
    JsonObject i2cObj = sensors["i2c"].to<JsonObject>();
    for (auto& kv : _i2cDevices) {
        JsonObject dev = i2cObj[kv.first].to<JsonObject>();
        dev["driver"] = kv.second.driver;
        dev["role"] = kv.second.role;
    }

    // Write MAX6675 SPI thermocouple settings
    JsonObject max6675Upd = sensors["max6675"].to<JsonObject>();
    max6675Upd["clk"] = proj.max6675Clk;
    max6675Upd["cs"] = proj.max6675Cs;
    max6675Upd["do"] = proj.max6675Do;
    max6675Upd["enabled"] = proj.max6675Enabled;

    // Write sensor display ranges
    JsonObject rangesUpd = sensors["ranges"].to<JsonObject>();
    for (auto& kv : _sensorRanges) {
        JsonObject r = rangesUpd[kv.first].to<JsonObject>();
        r["min"] = kv.second.min;
        r["max"] = kv.second.max;
        if (!isnan(kv.second.thresholdMin)) r["thresholdMin"] = kv.second.thresholdMin;
        if (!isnan(kv.second.thresholdMax)) r["thresholdMax"] = kv.second.thresholdMax;
    }

    // Write back
    file = SD.open(filename, FILE_WRITE);
    if (!file) {
        return false;
    }
    serializeJson(doc, file);
    file.close();
    return true;
}

bool Config::loadCertificates(const char* certFile, const char* keyFile) {
    if (!_sdInitialized) return false;

    // Helper lambda to read a PEM file into a PSRAM buffer
    auto readFile = [this](const char* path, uint8_t*& buf, size_t& len) -> bool {
        fs::File f = SD.open(path, FILE_READ);
        if (!f) return false;
        len = f.size();
        if (len == 0) { f.close(); return false; }
        buf = (uint8_t*)ps_malloc(len + 1);  // +1 for null terminator
        if (!buf) { f.close(); len = 0; return false; }
        if ((size_t)f.read(buf, len) != len) {
            free(buf); buf = nullptr; len = 0; f.close(); return false;
        }
        buf[len] = '\0';
        f.close();
        return true;
    };

    bool certOk = readFile(certFile, _certBuf, _certLen);
    bool keyOk = readFile(keyFile, _keyBuf, _keyLen);

    if (!certOk || !keyOk) {
        if (_certBuf) { free(_certBuf); _certBuf = nullptr; _certLen = 0; }
        if (_keyBuf) { free(_keyBuf); _keyBuf = nullptr; _keyLen = 0; }
        return false;
    }
    return true;
}

bool Config::generateSelfSignedCert() {
    if (!_sdInitialized) return false;

    // Free any existing cert/key buffers
    if (_certBuf) { free(_certBuf); _certBuf = nullptr; _certLen = 0; }
    if (_keyBuf) { free(_keyBuf); _keyBuf = nullptr; _keyLen = 0; }

    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi serial;
    bool success = false;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_mpi_init(&serial);

    // Seed RNG
    const char* pers = "esp32_cert_gen";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const uint8_t*)pers, strlen(pers));
    if (ret != 0) goto cleanup;

    // Generate ECC P-256 key pair
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) goto cleanup;

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) goto cleanup;

    // Set up certificate
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    {
        String cn = "CN=";
        cn += (_proj && _proj->systemName.length() > 0) ? _proj->systemName : "ESP32";
        ret = mbedtls_x509write_crt_set_subject_name(&crt, cn.c_str());
        if (ret != 0) goto cleanup;
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, cn.c_str());
        if (ret != 0) goto cleanup;
    }

    // Random serial number
    ret = mbedtls_mpi_lset(&serial, (int)esp_random());
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    if (ret != 0) goto cleanup;

    // Validity: now to 10 years from now
    {
        struct tm timeinfo;
        char notBefore[16], notAfter[16];
        if (getLocalTime(&timeinfo, 0)) {
            // NTP available — use real time
            strftime(notBefore, sizeof(notBefore), "%Y%m%d%H%M%S", &timeinfo);
            timeinfo.tm_year += 10;
            strftime(notAfter, sizeof(notAfter), "%Y%m%d%H%M%S", &timeinfo);
        } else {
            // No NTP — use a fixed range
            strcpy(notBefore, "20260101000000");
            strcpy(notAfter,  "20360101000000");
        }
        ret = mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter);
        if (ret != 0) goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_authority_key_identifier(&crt);
    if (ret != 0) goto cleanup;

    // Write cert and key as PEM to PSRAM buffers
    {
        const size_t bufSize = 4096;
        uint8_t* certPem = (uint8_t*)ps_malloc(bufSize);
        uint8_t* keyPem = (uint8_t*)ps_malloc(bufSize);
        if (!certPem || !keyPem) {
            if (certPem) free(certPem);
            if (keyPem) free(keyPem);
            goto cleanup;
        }

        ret = mbedtls_x509write_crt_pem(&crt, certPem, bufSize,
                                          mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0) { free(certPem); free(keyPem); goto cleanup; }

        ret = mbedtls_pk_write_key_pem(&key, keyPem, bufSize);
        if (ret != 0) { free(certPem); free(keyPem); goto cleanup; }

        _certLen = strlen((char*)certPem);
        _keyLen = strlen((char*)keyPem);

        // Save to SD card
        fs::File cf = SD.open("/cert.pem", FILE_WRITE);
        if (cf) { cf.write(certPem, _certLen); cf.close(); }
        else { free(certPem); free(keyPem); goto cleanup; }

        fs::File kf = SD.open("/key.pem", FILE_WRITE);
        if (kf) { kf.write(keyPem, _keyLen); kf.close(); }
        else { free(certPem); free(keyPem); goto cleanup; }

        // Keep buffers for in-memory use
        _certBuf = certPem;
        _keyBuf = keyPem;
        success = true;
    }

cleanup:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return success;
}

bool Config::isCertExpired() const {
    if (!_certBuf || _certLen == 0) return false;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return false;  // No NTP — can't check, assume valid

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);

    int ret = mbedtls_x509_crt_parse(&crt, _certBuf, _certLen + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;  // Parse error — don't regenerate
    }

    // Compare not_after with current time
    const mbedtls_x509_time& na = crt.valid_to;
    bool expired = false;
    int year = timeinfo.tm_year + 1900;
    int mon = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;

    if (year > na.year) expired = true;
    else if (year == na.year && mon > na.mon) expired = true;
    else if (year == na.year && mon == na.mon && day > na.day) expired = true;

    mbedtls_x509_crt_free(&crt);
    return expired;
}

bool Config::updateRuntime(const char* filename, uint32_t heatRuntimeMs, bool softwareDefrost) {
    if (!_sdInitialized) {
        return false;
    }

    fs::File file = SD.open(filename, FILE_READ);
    if (!file) {
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        return false;
    }

    // Update runtime and defrost active fields
    doc["runtime"]["heatAccumulatedMs"] = heatRuntimeMs;
    doc["heatpump"]["defrost"]["active"] = softwareDefrost;

    // Write back
    file = SD.open(filename, FILE_WRITE);
    if (!file) {
        return false;
    }
    serializeJson(doc, file);
    file.close();
    return true;
}
