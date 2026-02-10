#ifndef GOODMANHP_H
#define GOODMANHP_H

#include <Arduino.h>
#include <map>
#include <functional>
#include <DallasTemperature.h>
#include <TaskSchedulerDeclarations.h>
#include "InputPin.h"
#include "OutPin.h"
#include "TempSensor.h"

class GoodmanHP {
  public:
    enum class State { OFF, COOL, HEAT, DEFROST, ERROR };
    typedef std::function<void(State newState, State oldState)> StateChangeCallback;
    typedef std::function<void(bool active)> LPSFaultCallback;

    // Defrost constants
    static const uint32_t HEAT_RUNTIME_THRESHOLD_MS = 90UL * 60 * 1000;  // 90 min
    static const uint32_t DEFROST_MIN_RUNTIME_MS = 3UL * 60 * 1000;      // 3 min minimum defrost
    static const uint32_t DEFROST_TIMEOUT_MS = 15UL * 60 * 1000;         // 15 min safety timeout
    static constexpr float DEFROST_EXIT_F = 41.0f;

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

    // Heat runtime accumulation & automatic defrost
    uint32_t getHeatRuntimeMs() const;
    void setHeatRuntimeMs(uint32_t ms);
    void resetHeatRuntime();
    bool isSoftwareDefrostActive() const;
    bool isLPSFaultActive() const;

    void setStateChangeCallback(StateChangeCallback cb);
    void setLPSFaultCallback(LPSFaultCallback cb);

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

    // Heat runtime accumulation & automatic defrost
    uint32_t _heatRuntimeMs;
    uint32_t _heatRuntimeLastTick;
    uint32_t _heatRuntimeLastLogMs;
    bool _softwareDefrost;
    uint32_t _defrostStartTick;
    bool _lpsFault;
    StateChangeCallback _stateChangeCb;
    LPSFaultCallback _lpsFaultCb;

    void checkLPSFault();
    void checkYAndActivateCNT();
    void updateState();
    void accumulateHeatRuntime();
    void checkDefrostNeeded();
    void startSoftwareDefrost();
    void stopSoftwareDefrost();

    // Runtime callback for OutPins
    static GoodmanHP* _instance;
    static bool outPinRuntimeCallback(OutPin* pin, uint32_t onDuration);
    bool handleOutPinRuntime(OutPin* pin, uint32_t onDuration);
};

#endif
