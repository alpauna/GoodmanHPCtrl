#ifndef GOODMANHP_H
#define GOODMANHP_H

#include <Arduino.h>
#include <map>
#include <TaskSchedulerDeclarations.h>
#include "InputPin.h"
#include "OutPin.h"

class GoodmanHP {
  public:
    enum class State { OFF, COOL, HEAT, DEFROST };

    GoodmanHP(Scheduler *ts);

    void begin();
    void update();

    // Pin map management
    void addInput(const String& name, InputPin* pin);
    void addOutput(const String& name, OutPin* pin);
    InputPin* getInput(const String& name);
    OutPin* getOutput(const String& name);
    std::map<String, InputPin*>& getInputMap();
    std::map<String, OutPin*>& getOutputMap();

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

    std::map<String, InputPin*> _inputMap;
    std::map<String, OutPin*> _outputMap;

    State _state;
    uint32_t _yActiveStartTick;
    bool _yWasActive;
    bool _cntActivated;

    static const uint32_t Y_DELAY_MS = 30000;  // 30 seconds

    void checkYAndActivateCNT();
    void updateState();
};

#endif
