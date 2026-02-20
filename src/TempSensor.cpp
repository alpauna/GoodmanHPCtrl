#include "TempSensor.h"
#include <Wire.h>

TempSensor::TempSensor()
    : _description("")
    , _deviceAddress(nullptr)
    , _value(0.0f)
    , _previous(0.0f)
    , _valid(false)
    , _onUpdate(nullptr)
    , _onChange(nullptr)
    , _mcp9600(nullptr)
    , _i2cAddress(0)
    , _mcp9600Failures(0)
    , _max6675(nullptr)
    , _max6675Failures(0)
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
    , _i2cAddress(0)
    , _mcp9600Failures(0)
    , _max6675(nullptr)
    , _max6675Failures(0)
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
    // MCP9600 I2C thermocouple path — raw Wire read to avoid Adafruit lib hangs
    if (_mcp9600 != nullptr) {
        if (_mcp9600Failures >= MCP9600_MAX_FAILURES) {
            // Too many failures — mark invalid and stop trying
            _valid = false;
            return;
        }
        // Read hot junction register (0x00) directly via Wire — 2 bytes, signed 16-bit
        uint8_t addr = _i2cAddress ? _i2cAddress : 0x67;
        Wire.beginTransmission(addr);
        Wire.write(0x00);  // MCP9600_HOTJUNCTION register
        uint8_t err = Wire.endTransmission(false);  // repeated start
        if (err != 0) {
            _mcp9600Failures++;
            Serial.printf("MCP9600 write failed (err=%d, fails=%d)\r\n", err, _mcp9600Failures);
            if (_mcp9600Failures >= MCP9600_MAX_FAILURES) {
                Serial.println("MCP9600 disabled after repeated failures");
                _valid = false;
            }
            return;
        }
        uint8_t got = Wire.requestFrom(addr, (uint8_t)2);
        if (got != 2) {
            _mcp9600Failures++;
            Serial.printf("MCP9600 read short (%d bytes, fails=%d)\r\n", got, _mcp9600Failures);
            if (_mcp9600Failures >= MCP9600_MAX_FAILURES) {
                Serial.println("MCP9600 disabled after repeated failures");
                _valid = false;
            }
            return;
        }
        uint8_t msb = Wire.read();
        uint8_t lsb = Wire.read();
        int16_t raw = (msb << 8) | lsb;
        float tempC = raw * 0.0625f;  // MCP9600 resolution: 0.0625°C/LSB
        float tempF = tempC * 9.0f / 5.0f + 32.0f;
        // Sanity check — reject wild readings
        if (tempF < -40.0f || tempF > 500.0f) {
            _mcp9600Failures++;
            return;
        }
        _mcp9600Failures = 0;  // success — reset counter
        updateValue(tempF, threshold);
        return;
    }

    // MAX6675 SPI thermocouple path
    if (_max6675 != nullptr) {
        if (_max6675Failures >= MAX6675_MAX_FAILURES) {
            _valid = false;
            return;
        }
        float tempC = _max6675->readCelsius();
        if (isnan(tempC)) {
            _max6675Failures++;
            Serial.printf("MAX6675 read failed (NAN, fails=%d)\r\n", _max6675Failures);
            if (_max6675Failures >= MAX6675_MAX_FAILURES) {
                Serial.println("MAX6675 disabled after repeated failures");
                _valid = false;
            }
            return;
        }
        float tempF = tempC * 9.0f / 5.0f + 32.0f;
        if (tempF < -40.0f || tempF > 1500.0f) {
            _max6675Failures++;
            return;
        }
        _max6675Failures = 0;
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
        case 4: return "LIQUID_TEMP";
        default: return "SENSOR_" + String(index);
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

        uint8_t family = sensor->getDeviceAddress()[0];
        const char* devType = (family == 0x3B) ? "MAX31850K thermocouple" :
                              (family == 0x28) ? "DS18B20" :
                              (family == 0x22) ? "DS1822" :
                              (family == 0x10) ? "DS18S20" : "unknown";
        Serial.printf("Device %d: %s (%s) Address: %s\r\n",
            i, sensor->getDescription().c_str(), devType,
            addressToString(sensor->getDeviceAddress()).c_str());
    }
}
