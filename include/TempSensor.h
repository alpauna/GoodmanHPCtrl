#ifndef TEMPSENSOR_H
#define TEMPSENSOR_H

#include <Arduino.h>
#include <map>
#include <DallasTemperature.h>

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

    // Callbacks
    void setUpdateCallback(TempSensorCallback callback) { _onUpdate = callback; }
    void setChangeCallback(TempSensorCallback callback) { _onChange = callback; }
    TempSensorCallback getUpdateCallback() const { return _onUpdate; }
    TempSensorCallback getChangeCallback() const { return _onChange; }

    // Operations
    void update(DallasTemperature* sensors, float threshold = 0.33f);
    void fireUpdateCallback();
    void fireChangeCallback();

    // Static helper for device address string conversion
    static String addressToString(uint8_t* address);
    static void stringToAddress(const String& str, uint8_t* address);

  private:
    String _description;
    uint8_t* _deviceAddress;
    float _value;
    float _previous;
    bool _valid;
    TempSensorCallback _onUpdate;
    TempSensorCallback _onChange;
};

#endif
