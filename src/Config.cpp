#include "Config.h"
#include "sdios.h"

// External references needed for config loading
extern ArduinoOutStream cout;

// External callbacks for temp sensors
extern void tempSensorUpdateCallback(TempSensor* sensor);
extern void tempSensorChangeCallback(TempSensor* sensor);

struct ProjectInfo {
    String name;
    String createdOnDate;
    String description;
    String encrypt;
    bool encrpytped;
};

#define SPI_SPEED SD_SCK_MHZ(SD_SPI_SPEED)

Config::Config()
    : _sdInitialized(false)
    , _mqttHost(192, 168, 0, 46)
    , _mqttPort(1883)
    , _mqttUser("debian")
    , _mqttPassword("")
    , _wifiSSID("")
    , _wifiPassword("")
    , _tempDiscoveryCb(nullptr)
{
}

bool Config::initSDCard() {
    if (!_sd.begin(SS, SPI_SPEED)) {
        if (_sd.card()->errorCode()) {
            cout <<
                "\nSD initialization failed.\n"
                "Do not reformat the card!\n"
                "Is the card correctly inserted?\n"
                "Is chipSelect set to the correct value?\n"
                "Does another SPI device need to be disabled?\n"
                "Is there a wiring/soldering problem?\n";
            cout << "\nerrorCode: " << hex << showbase;
            cout << int(_sd.card()->errorCode());
            cout << ", errorData: " << int(_sd.card()->errorData());
            cout << dec << noshowbase << endl;
            return false;
        }
        cout << "\nCard successfully initialized.\n";
        if (_sd.vol()->fatType() == 0) {
            cout << "Can't find a valid FAT16/FAT32/exFAT partition.\n";
            return false;
        }
        cout << "Can't determine error type\n";
        return false;
    }
    cout << "\nCard successfully initialized.\n";
    cout << endl;

    uint32_t size = _sd.card()->sectorCount();
    if (size == 0) {
        cout << "Can't determine the card size.\n";
        return false;
    }
    _sdInitialized = true;
    return true;
}

bool Config::openConfigFile(const char* filename, TempSensorMap& config, ProjectInfo& proj) {
    if (!_sd.exists(filename) || (_configFile.open(filename, O_RDONLY) && _configFile.size() == 0)) {
        _configFile.close();
        return saveConfiguration(filename, config, proj);
    }
    _configFile.close();

    return _configFile.open(filename, O_RDONLY);
}

bool Config::loadTempConfig(const char* filename, TempSensorMap& config) {
    if (!_configFile.isOpen()) {
        return false;
    }
    _configFile.rewind();

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

    const char* wifi_ssid = doc["wifi"]["ssid"];
    const char* wifi_password = doc["wifi"]["password"];
    _wifiSSID = wifi_ssid != nullptr ? wifi_ssid : "";
    _wifiPassword = wifi_password != nullptr ? wifi_password : "";
    cout << "Read WiFi SSID:" << wifi_ssid << endl;

    JsonObject mqtt = doc["mqtt"];
    const char* mqtt_user = mqtt["user"];
    const char* mqtt_password = mqtt["password"];
    const char* mqtt_host = mqtt["host"];
    int mqtt_port = mqtt["port"];
    _mqttPort = mqtt_port;
    _mqttUser = mqtt_user != nullptr ? mqtt_user : "";
    _mqttPassword = mqtt_password != nullptr ? mqtt_password : "";
    _mqttHost.fromString(mqtt_host != nullptr ? mqtt_host : "192.168.1.2");
    cout << "Read mqtt Host:" << _mqttHost << endl;

    clearConfig(config);
    for (JsonPair sensors_temp_item : doc["sensors"]["temp"].as<JsonObject>()) {
        const char* key = sensors_temp_item.key().c_str();
        const char* desc = sensors_temp_item.value()["description"];
        int last_value = sensors_temp_item.value()["last-value"];
        const char* name = sensors_temp_item.value()["name"];
        cout << "Key:" << key << "\t";
        cout << "Description:" << desc << "\t";
        cout << "Last Value:" << last_value << endl;
        cout << "Name: " << name << endl;

        TempSensor* sensor = new TempSensor(String(desc));
        config[name] = sensor;

        String devaddrStr = String(key);
        cout << "Devstr:" << devaddrStr << endl;
        TempSensor::stringToAddress(devaddrStr, sensor->getDeviceAddress());

        sensor->setValue(last_value);
        sensor->setPrevious(sensor->getValue());
        sensor->setValid(true);
        sensor->setChangeCallback(tempSensorChangeCallback);
        sensor->setUpdateCallback(tempSensorUpdateCallback);

        cout << "JSON description: " << sensor->getDescription()
             << "\tID:" << TempSensor::addressToString(sensor->getDeviceAddress())
             << "\t Value:" << sensor->getValue() << endl;
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
    if (_sd.exists(filename) && _configFile.open(filename, O_RDONLY) && _configFile.size() > 0) {
        _configFile.close();
        return false;
    }
    _configFile.close();
    if (!_configFile.open(filename, O_RDWR | O_TRUNC | O_CREAT)) {
        cout << "open failed: " << "\"" << filename << "\"" << endl;
        return false;
    }
    JsonDocument doc;

    doc["project"] = proj.name;
    doc["created"] = proj.createdOnDate;
    doc["description"] = proj.description;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = "MEGA";
    wifi["password"] = "";

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["user"] = "debian";
    mqtt["password"] = "";
    mqtt["host"] = "192.168.1.1";
    mqtt["port"] = 1883;

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
    cout << "Temp sensor as json..." << endl;
    cout << output << endl;
    _configFile.close();
    return true;
}
