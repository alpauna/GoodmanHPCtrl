#include "GoodmanHP.h"
#include "Logger.h"

// Static instance pointer for runtime callback
GoodmanHP* GoodmanHP::_instance = nullptr;

GoodmanHP::GoodmanHP(Scheduler *ts)
    : _ts(ts)
    , _state(State::OFF)
    , _yActiveStartTick(0)
    , _yWasActive(false)
    , _cntActivated(false)
{
    _instance = this;
    _tskUpdate = new Task(500, TASK_FOREVER, [this]() {
        this->update();
    }, ts, false);
}

void GoodmanHP::begin() {
    _tskUpdate->enable();
    Log.info("HP", "GoodmanHP controller started");
}

void GoodmanHP::addInput(const String& name, InputPin* pin) {
    _inputMap[name] = pin;
    pin->initPin();
}

void GoodmanHP::addOutput(const String& name, OutPin* pin) {
    _outputMap[name] = pin;
    pin->initPin();
    // Set runtime callback so GoodmanHP can respond to OutPin events
    pin->setRuntimeCallback(outPinRuntimeCallback);
}

InputPin* GoodmanHP::getInput(const String& name) {
    auto it = _inputMap.find(name);
    if (it != _inputMap.end()) {
        return it->second;
    }
    return nullptr;
}

OutPin* GoodmanHP::getOutput(const String& name) {
    auto it = _outputMap.find(name);
    if (it != _outputMap.end()) {
        return it->second;
    }
    return nullptr;
}

std::map<String, InputPin*>& GoodmanHP::getInputMap() {
    return _inputMap;
}

std::map<String, OutPin*>& GoodmanHP::getOutputMap() {
    return _outputMap;
}

void GoodmanHP::addTempSensor(const String& name, TempSensor* sensor) {
    _tempSensorMap[name] = sensor;
}

TempSensor* GoodmanHP::getTempSensor(const String& name) {
    auto it = _tempSensorMap.find(name);
    if (it != _tempSensorMap.end()) {
        return it->second;
    }
    return nullptr;
}

TempSensorMap& GoodmanHP::getTempSensorMap() {
    return _tempSensorMap;
}

void GoodmanHP::clearTempSensors() {
    for (auto& pair : _tempSensorMap) {
        if (pair.second != nullptr) {
            delete pair.second;
        }
    }
    _tempSensorMap.clear();
}

void GoodmanHP::update() {
    checkYAndActivateCNT();
    updateState();
}

void GoodmanHP::checkYAndActivateCNT() {
    InputPin* y = getInput("Y");
    OutPin* cnt = getOutput("CNT");

    if (y == nullptr || cnt == nullptr) {
        return;
    }

    bool yActive = y->isActive();

    if (yActive && !_yWasActive) {
        // Y just became active - record start time
        _yActiveStartTick = millis();
        _yWasActive = true;
        Log.info("HP", "Y input activated, starting 30s timer");
    } else if (!yActive && _yWasActive) {
        // Y just became inactive - reset
        _yWasActive = false;
        _yActiveStartTick = 0;
        if (_cntActivated) {
            cnt->turnOff();
            _cntActivated = false;
            Log.info("HP", "Y input deactivated, CNT turned off");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        // Y still active - check if 30 seconds have passed
        uint32_t elapsed = millis() - _yActiveStartTick;
        if (elapsed >= Y_DELAY_MS) {
            cnt->turnOn();
            _cntActivated = true;
            Log.info("HP", "Y active for 30s, CNT activated");
        }
    }
}

void GoodmanHP::updateState() {
    InputPin* dft = getInput("DFT");
    InputPin* y = getInput("Y");
    InputPin* o = getInput("O");

    if (dft == nullptr || y == nullptr || o == nullptr) {
        return;
    }

    State newState = State::OFF;

    if (dft->isActive()) {
        newState = State::DEFROST;
    } else if (y->isActive() && o->isActive()) {
        newState = State::HEAT;
    } else if (y->isActive()) {
        newState = State::COOL;
    }

    if (newState != _state) {
        Log.info("HP", "State changed: %s -> %s", getStateString(),
                 newState == State::OFF ? "OFF" :
                 newState == State::COOL ? "COOL" :
                 newState == State::HEAT ? "HEAT" : "DEFROST");
        _state = newState;
    }
}

GoodmanHP::State GoodmanHP::getState() {
    return _state;
}

const char* GoodmanHP::getStateString() {
    switch (_state) {
        case State::OFF: return "OFF";
        case State::COOL: return "COOL";
        case State::HEAT: return "HEAT";
        case State::DEFROST: return "DEFROST";
        default: return "UNKNOWN";
    }
}

bool GoodmanHP::isYActive() {
    InputPin* y = getInput("Y");
    return y != nullptr && y->isActive();
}

bool GoodmanHP::isOActive() {
    InputPin* o = getInput("O");
    return o != nullptr && o->isActive();
}

bool GoodmanHP::isLPSActive() {
    InputPin* lps = getInput("LPS");
    return lps != nullptr && lps->isActive();
}

bool GoodmanHP::isDFTActive() {
    InputPin* dft = getInput("DFT");
    return dft != nullptr && dft->isActive();
}

uint32_t GoodmanHP::getYActiveTime() {
    if (_yWasActive) {
        return millis() - _yActiveStartTick;
    }
    return 0;
}

// Static callback delegates to instance method
bool GoodmanHP::outPinRuntimeCallback(OutPin* pin, uint32_t onDuration) {
    if (_instance != nullptr) {
        return _instance->handleOutPinRuntime(pin, onDuration);
    }
    return false;  // No instance, stop the callback
}

// Instance method handles specific OutPin runtime events
bool GoodmanHP::handleOutPinRuntime(OutPin* pin, uint32_t onDuration) {
    if (pin == nullptr) {
        return false;
    }

    String pinName = pin->getName();

    // Handle specific OutPins based on their name
    if (pinName == "CNT") {
        // Contactor runtime monitoring
        Log.debug("HP", "CNT runtime: %lu ms", onDuration);
        return true;  // Continue monitoring
    } else if (pinName == "FAN") {
        // Fan runtime monitoring
        Log.debug("HP", "FAN runtime: %lu ms", onDuration);
        return true;  // Continue monitoring
    } else if (pinName == "W") {
        // Heating relay runtime monitoring
        Log.debug("HP", "W runtime: %lu ms", onDuration);
        return true;  // Continue monitoring
    } else if (pinName == "RV") {
        // Reversing valve runtime monitoring
        Log.debug("HP", "RV runtime: %lu ms", onDuration);
        return true;  // Continue monitoring
    }

    // Unknown pin, continue callback by default
    return true;
}
