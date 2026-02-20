#ifndef TEMPSENSOR_H
#define TEMPSENSOR_H

#include <Arduino.h>
#include <map>
#include <DallasTemperature.h>
#include <Adafruit_MCP9600.h>
#include <max6675.h>

class TempSensor;
typedef void (*TempSensorCallback)(TempSensor* sensor);
typedef std::map<String, TempSensor*> TempSensorMap;

class TempSensor {
  public:
    TempSensor();
    TempSensor(const String& description);
    ~TempSensor();

    // Getters
    String getDescription() const { return _description; }
    uint8_t* getDeviceAddress() { return _deviceAddress; }
    float getValue() const { return _value; }
    float getPrevious() const { return _previous; }
    bool isValid() const { return _valid; }

    // Setters
    void setDescription(const String& description) { _description = description; }
    void setDeviceAddress(uint8_t* address);
    void setValue(float value);
    void setPrevious(float previous) { _previous = previous; }
    void setValid(bool valid) { _valid = valid; }
    void setMCP9600(Adafruit_MCP9600* mcp) { _mcp9600 = mcp; }
    bool hasMCP9600() const { return _mcp9600 != nullptr; }
    void setMAX6675(MAX6675* max) { _max6675 = max; }
    bool hasMAX6675() const { return _max6675 != nullptr; }
    void setI2CAddress(uint8_t addr) { _i2cAddress = addr; }
    uint8_t getI2CAddress() const { return _i2cAddress; }

    // Callbacks
    void setUpdateCallback(TempSensorCallback callback) { _onUpdate = callback; }
    void setChangeCallback(TempSensorCallback callback) { _onChange = callback; }
    TempSensorCallback getUpdateCallback() const { return _onUpdate; }
    TempSensorCallback getChangeCallback() const { return _onChange; }

    // Operations
    void update(DallasTemperature* sensors, float threshold = 0.33f);
    void updateValue(float tempF, float threshold = 0.33f);
    void fireUpdateCallback();
    void fireChangeCallback();

    // Static helper for device address string conversion
    static String addressToString(uint8_t* address);
    static void stringToAddress(const String& str, uint8_t* address);
    static void printAddress(uint8_t* address);

    // Static sensor discovery
    static void discoverSensors(DallasTemperature* sensors, TempSensorMap& tempMap,
                                TempSensorCallback updateCallback = nullptr,
                                TempSensorCallback changeCallback = nullptr);
    static String getDefaultDescription(uint8_t index);

  private:
    String _description;
    uint8_t* _deviceAddress;
    float _value;
    float _previous;
    bool _valid;
    TempSensorCallback _onUpdate;
    TempSensorCallback _onChange;
    Adafruit_MCP9600* _mcp9600;
    uint8_t _i2cAddress;
    uint8_t _mcp9600Failures;
    static const uint8_t MCP9600_MAX_FAILURES = 5;
    MAX6675* _max6675;
    uint8_t _max6675Failures;
    static const uint8_t MAX6675_MAX_FAILURES = 5;
};

#endif
