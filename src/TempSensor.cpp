#include "TempSensor.h"

TempSensor::TempSensor()
    : _description("")
    , _deviceAddress(nullptr)
    , _value(0.0f)
    , _previous(0.0f)
    , _valid(false)
    , _onUpdate(nullptr)
    , _onChange(nullptr)
    , _mcp9600(nullptr)
{
    _deviceAddress = new uint8_t[sizeof(DeviceAddress)];
    memset(_deviceAddress, 0, sizeof(DeviceAddress));
}

TempSensor::TempSensor(const String& description)
    : _description(description)
    , _deviceAddress(nullptr)
    , _value(0.0f)
    , _previous(0.0f)
    , _valid(false)
    , _onUpdate(nullptr)
    , _onChange(nullptr)
    , _mcp9600(nullptr)
{
    _deviceAddress = new uint8_t[sizeof(DeviceAddress)];
    memset(_deviceAddress, 0, sizeof(DeviceAddress));
}

TempSensor::~TempSensor() {
    if (_deviceAddress != nullptr) {
        delete[] _deviceAddress;
        _deviceAddress = nullptr;
    }
}

void TempSensor::setDeviceAddress(uint8_t* address) {
    if (address != nullptr && _deviceAddress != nullptr) {
        memcpy(_deviceAddress, address, sizeof(DeviceAddress));
    }
}

void TempSensor::setValue(float value) {
    _previous = _value;
    _value = value;
    _valid = (value != DEVICE_DISCONNECTED_F);
}

void TempSensor::update(DallasTemperature* sensors, float threshold) {
    // MCP9600 I2C thermocouple path
    if (_mcp9600 != nullptr) {
        float tempC = _mcp9600->readThermocouple();
        float tempF = tempC * 9.0f / 5.0f + 32.0f;
        updateValue(tempF, threshold);
        return;
    }

    // OneWire DallasTemperature path
    if (sensors == nullptr || _deviceAddress == nullptr) {
        return;
    }

    float rawTemp = sensors->getTemp(_deviceAddress);
    float tempF = DallasTemperature::rawToFahrenheit(rawTemp);
    float delta = abs(_previous - tempF);

    if (delta > threshold) {
        _previous = _value;
        _value = tempF;
        _valid = (_value != DEVICE_DISCONNECTED_F);
        fireChangeCallback();
    }
}

void TempSensor::updateValue(float tempF, float threshold) {
    float delta = abs(_previous - tempF);

    if (delta > threshold) {
        _previous = _value;
        _value = tempF;
        _valid = true;
        fireChangeCallback();
    }
}

void TempSensor::fireUpdateCallback() {
    if (_onUpdate != nullptr) {
        _onUpdate(this);
    }
}

void TempSensor::fireChangeCallback() {
    if (_onChange != nullptr) {
        _onChange(this);
    }
}

String TempSensor::addressToString(uint8_t* address) {
    String result;
    char hexChar[3];
    for (uint8_t i = 0; i < 8; i++) {
        sprintf(hexChar, "%02X", address[i]);
        result += hexChar;
    }
    return result;
}

void TempSensor::stringToAddress(const String& str, uint8_t* address) {
    if (address == nullptr || str.length() < 16) {
        return;
    }
    char* endptr;
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t x = i * 2;
        String rs = String(str[x]) + String(str[x + 1]);
        uint32_t val = strtol(rs.c_str(), &endptr, 16);
        address[i] = val < 256 ? val : 0;
    }
}

void TempSensor::printAddress(uint8_t* address) {
    Serial.print(" ID: ");
    for (uint8_t i = 0; i < 8; i++) {
        if (address[i] < 16) Serial.print("0");
        Serial.print(address[i], HEX);
    }
}

String TempSensor::getDefaultDescription(uint8_t index) {
    switch (index) {
        case 0: return "COMPRESSOR_TEMP";
        case 1: return "SUCTION_TEMP";
        case 2: return "AMBIENT_TEMP";
        case 3: return "CONDENSER_TEMP";
        default: return "UNKNOWN_TEMP";
    }
}

void TempSensor::discoverSensors(DallasTemperature* sensors, TempSensorMap& tempMap,
                                  TempSensorCallback updateCallback,
                                  TempSensorCallback changeCallback) {
    if (sensors == nullptr) {
        return;
    }

    Serial.print("Locating devices...");
    sensors->begin();
    Serial.print("Found ");
    uint8_t oneWCount = sensors->getDeviceCount();
    Serial.print(oneWCount, DEC);
    Serial.println(" devices.");

    for (uint8_t i = 0; i < oneWCount; i++) {
        String description = getDefaultDescription(i);

        TempSensor* sensor = new TempSensor(description);
        if (changeCallback != nullptr) {
            sensor->setChangeCallback(changeCallback);
        }
        if (updateCallback != nullptr) {
            sensor->setUpdateCallback(updateCallback);
        }

        if (tempMap.count(description) == 0) {
            tempMap[description] = sensor;
        }

        if (!sensors->getAddress(sensor->getDeviceAddress(), i)) {
            Serial.println("Unable to find address for Device");
        }

        Serial.print("Device ");
        Serial.print(i);
        Serial.print(" Address: ");
        Serial.printf("Temp Sensor Description: %s ", sensor->getDescription().c_str());
        Serial.println(addressToString(sensor->getDeviceAddress()));
    }
}
