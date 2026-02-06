#ifndef GOODMANHP_H
#define GOODMANHP_H

#include <Arduino.h>
#include <map>
#include <DallasTemperature.h>
#include <TaskSchedulerDeclarations.h>
#include "InputPin.h"
#include "OutPin.h"
#include "TempSensor.h"

class GoodmanHP {
  public:
    enum class State { OFF, COOL, HEAT, DEFROST };

    GoodmanHP(Scheduler *ts);

    void setDallasTemperature(DallasTemperature *sensors);
    void begin();
    void update();

    // Pin map management
    void addInput(const String& name, InputPin* pin);
    void addOutput(const String& name, OutPin* pin);
    InputPin* getInput(const String& name);
    OutPin* getOutput(const String& name);
    std::map<String, InputPin*>& getInputMap();
    std::map<String, OutPin*>& getOutputMap();

    // Temperature sensor management
    void addTempSensor(const String& name, TempSensor* sensor);
    TempSensor* getTempSensor(const String& name);
    TempSensorMap& getTempSensorMap();
    void clearTempSensors();

    State getState();
    const char* getStateString();

    bool isYActive();
    bool isOActive();
    bool isLPSActive();
    bool isDFTActive();

    uint32_t getYActiveTime();

  private:
    Scheduler *_ts;
    Task *_tskUpdate;
    Task *_tskCheckTemps;
    DallasTemperature *_sensors;

    std::map<String, InputPin*> _inputMap;
    std::map<String, OutPin*> _outputMap;
    TempSensorMap _tempSensorMap;

    State _state;
    uint32_t _yActiveStartTick;
    bool _yWasActive;
    bool _cntActivated;

    static const uint32_t Y_DELAY_MS = 30000;  // 30 seconds

    void checkYAndActivateCNT();
    void updateState();

    // Runtime callback for OutPins
    static GoodmanHP* _instance;
    static bool outPinRuntimeCallback(OutPin* pin, uint32_t onDuration);
    bool handleOutPinRuntime(OutPin* pin, uint32_t onDuration);
};

#endif
