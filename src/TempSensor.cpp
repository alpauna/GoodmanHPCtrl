#include "TempSensor.h"

TempSensor::TempSensor()
    : _description("")
    , _deviceAddress(nullptr)
    , _value(0.0f)
    , _previous(0.0f)
    , _valid(false)
    , _onUpdate(nullptr)
    , _onChange(nullptr)
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
