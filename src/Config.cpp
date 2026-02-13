#include "Config.h"
#include "esp_hmac.h"
#include "esp_random.h"

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

void Config::setAdminPassword(const String& plaintext) {
    _adminPasswordHash = plaintext;
}

bool Config::verifyAdminPassword(const String& plaintext) const {
    return plaintext == _adminPasswordHash;
}

bool Config::initSDCard() {
    if (!SD.begin(SS, SPI, SD_SPI_SPEED * 1000000UL)) {
        Serial.println("\nSD initialization failed.");
        Serial.println("Is the card correctly inserted?");
        Serial.println("Is chipSelect set to the correct value?");
        return false;
    }
    Serial.println("\nCard successfully initialized.\n");
    _sdInitialized = true;
    return true;
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
    Serial.printf("Read WiFi SSID:%s apFallback:%us\n", wifi_ssid ? wifi_ssid : "", proj.apFallbackSeconds);

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

    // Load timezone settings
    JsonObject timezone = doc["timezone"];
    proj.gmtOffsetSec = timezone["gmtOffset"] | (-21600);
    proj.daylightOffsetSec = timezone["daylightOffset"] | 3600;
    Serial.printf("Read timezone: gmtOffset=%d daylightOffset=%d\n", proj.gmtOffsetSec, proj.daylightOffsetSec);

    // Load heatpump protection settings (with migration from old "lowTemp" root key)
    JsonObject heatpump = doc["heatpump"];
    if (heatpump.isNull() && doc["lowTemp"].is<JsonObject>()) {
        // Old format: migrate lowTemp.threshold into heatpump section
        proj.lowTempThreshold = doc["lowTemp"]["threshold"] | 20.0f;
        proj.highSuctionTempThreshold = 140.0f;
        proj.rvFail = false;
        proj.rvShortCycleMs = 30000;
        proj.cntShortCycleMs = 30000;
        Serial.println("Config migration: old lowTemp format detected, will migrate on next save");
    } else {
        proj.lowTempThreshold = heatpump["lowTemp"]["threshold"] | 20.0f;
        proj.highSuctionTempThreshold = heatpump["highSuctionTemp"]["threshold"] | 140.0f;
        proj.rvFail = heatpump["highSuctionTemp"]["rvFail"] | false;
        proj.rvShortCycleMs = heatpump["shortCycle"]["rv"] | 30000;
        proj.cntShortCycleMs = heatpump["shortCycle"]["cnt"] | 30000;
    }
    Serial.printf("Read heatpump: lowTemp=%.1fF highSuct=%.1fF rvFail=%d rvSC=%lu cntSC=%lu\n",
                  proj.lowTempThreshold, proj.highSuctionTempThreshold, proj.rvFail,
                  proj.rvShortCycleMs, proj.cntShortCycleMs);

    // Load UI theme
    const char* uiTheme = doc["ui"]["theme"];
    proj.theme = (uiTheme != nullptr) ? String(uiTheme) : "dark";
    Serial.printf("Read UI theme: %s\n", proj.theme.c_str());

    // Load admin password (encrypted same as WiFi/MQTT passwords)
    const char* adminPw = doc["admin"]["password"];
    String adminPwStr = (adminPw != nullptr && strlen(adminPw) > 0) ? String(adminPw) : "";
    _adminPasswordHash = decryptPassword(adminPwStr);
    Serial.printf("Admin password: %s\n", _adminPasswordHash.length() > 0 ? "set" : "not set");

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
        sensor->setValid(true);
        sensor->setChangeCallback(tempSensorChangeCallback);
        sensor->setUpdateCallback(tempSensorUpdateCallback);

        Serial.printf("JSON description: %s\tID:%s\t Value:%.1f\n",
             sensor->getDescription().c_str(),
             TempSensor::addressToString(sensor->getDeviceAddress()).c_str(),
             sensor->getValue());
    }
    _configFile.close();
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
    timezone["gmtOffset"] = proj.gmtOffsetSec;
    timezone["daylightOffset"] = proj.daylightOffsetSec;

    JsonObject heatpump = doc["heatpump"].to<JsonObject>();
    JsonObject hpLowTemp = heatpump["lowTemp"].to<JsonObject>();
    hpLowTemp["threshold"] = proj.lowTempThreshold;
    JsonObject hpHighSuction = heatpump["highSuctionTemp"].to<JsonObject>();
    hpHighSuction["threshold"] = proj.highSuctionTempThreshold;
    hpHighSuction["rvFail"] = proj.rvFail;
    JsonObject hpShortCycle = heatpump["shortCycle"].to<JsonObject>();
    hpShortCycle["rv"] = proj.rvShortCycleMs;
    hpShortCycle["cnt"] = proj.cntShortCycleMs;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = "";

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
    timezone["gmtOffset"] = proj.gmtOffsetSec;
    timezone["daylightOffset"] = proj.daylightOffsetSec;

    // Remove old root-level lowTemp key (migration cleanup)
    doc.remove("lowTemp");

    JsonObject heatpump = doc["heatpump"].to<JsonObject>();
    JsonObject hpLowTemp = heatpump["lowTemp"].to<JsonObject>();
    hpLowTemp["threshold"] = proj.lowTempThreshold;
    JsonObject hpHighSuction = heatpump["highSuctionTemp"].to<JsonObject>();
    hpHighSuction["threshold"] = proj.highSuctionTempThreshold;
    hpHighSuction["rvFail"] = proj.rvFail;
    JsonObject hpShortCycle = heatpump["shortCycle"].to<JsonObject>();
    hpShortCycle["rv"] = proj.rvShortCycleMs;
    hpShortCycle["cnt"] = proj.cntShortCycleMs;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = encryptPassword(_adminPasswordHash);

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

bool Config::updateRuntime(const char* filename, uint32_t heatRuntimeMs) {
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

    // Update runtime field
    doc["runtime"]["heatAccumulatedMs"] = heatRuntimeMs;

    // Write back
    file = SD.open(filename, FILE_WRITE);
    if (!file) {
        return false;
    }
    serializeJson(doc, file);
    file.close();
    return true;
}
