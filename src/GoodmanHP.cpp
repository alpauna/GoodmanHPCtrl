#include "GoodmanHP.h"
#include "Logger.h"

// Static instance pointer for runtime callback
GoodmanHP* GoodmanHP::_instance = nullptr;

GoodmanHP::GoodmanHP(Scheduler *ts)
    : _ts(ts)
    , _sensors(nullptr)
    , _state(State::OFF)
    , _yActiveStartTick(0)
    , _yWasActive(false)
    , _cntActivated(false)
    , _heatRuntimeMs(0)
    , _heatRuntimeLastTick(0)
    , _heatRuntimeLastLogMs(0)
    , _softwareDefrost(false)
    , _defrostStartTick(0)
    , _defrostRecheckTick(0)
    , _defrostPersistent(false)
    , _dftDefrost(false)
    , _dftDefrostStartTick(0)
{
    _instance = this;
    _tskUpdate = new Task(500, TASK_FOREVER, [this]() {
        this->update();
    }, ts, false);
    _tskCheckTemps = new Task(10 * TASK_SECOND, TASK_FOREVER, [this]() {
        if (_sensors == nullptr) return;
        _sensors->requestTemperatures();
        for (auto& mp : _tempSensorMap) {
            if (mp.second == nullptr) continue;
            mp.second->update(_sensors);
        }
    }, ts, false);
}

void GoodmanHP::setDallasTemperature(DallasTemperature *sensors) {
    _sensors = sensors;
}

void GoodmanHP::begin() {
    _tskUpdate->enable();
    _tskCheckTemps->enable();
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
    accumulateHeatRuntime();
    updateState();
    checkDefrostNeeded();
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
        if (_cntActivated && !_softwareDefrost) {
            cnt->turnOff();
            _cntActivated = false;
            Log.info("HP", "Y input deactivated, CNT turned off");
        } else if (_softwareDefrost) {
            Log.info("HP", "Y input deactivated during software defrost, keeping CNT on");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        // Check if CNT was off for less than 5 minutes - if so, enforce 30s delay
        uint32_t offElapsed = millis() - cnt->getOffTick();
        if (cnt->getOffTick() > 0 && offElapsed < 5 * 60 * 1000UL) {
            // Y still active, CNT off < 5 min - check if 30 seconds have passed
            uint32_t elapsed = millis() - _yActiveStartTick;
            if (elapsed >= Y_DELAY_MS) {
                cnt->turnOn();
                _cntActivated = true;
                Log.info("HP", "Y active for 30s, CNT activated (short cycle protection)");
            }
        } else {
            // CNT off >= 5 min or never turned off - activate immediately
            cnt->turnOn();
            _cntActivated = true;
            Log.info("HP", "Y active, CNT activated immediately (off > 5 min)");
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

    // DFT input only triggers defrost from HEAT mode or if DFT defrost already active
    bool dftTrigger = dft->isActive() && (_state == State::HEAT || _dftDefrost);

    if (dftTrigger || _dftDefrost || _softwareDefrost || _defrostPersistent) {
        newState = State::DEFROST;
        // Latch DFT defrost on first activation
        if (dftTrigger && !_dftDefrost) {
            _dftDefrost = true;
            _dftDefrostStartTick = millis();
            Log.info("HP", "DFT emergency defrost activated (sensor < 41F)");
        }
    } else if (y->isActive() && o->isActive()) {
        newState = State::COOL;
    } else if (y->isActive()) {
        newState = State::HEAT;
    }

    if (newState != _state) {
        Log.info("HP", "State changed: %s -> %s", getStateString(),
                 newState == State::OFF ? "OFF" :
                 newState == State::COOL ? "COOL" :
                 newState == State::HEAT ? "HEAT" : "DEFROST");
        _state = newState;

        // Control RV based on mode: ON for COOL, OFF for HEAT/OFF
        OutPin* rv = getOutput("RV");
        if (rv != nullptr && !_softwareDefrost) {
            if (newState == State::COOL) {
                rv->turnOn();
                Log.info("HP", "RV turned ON for COOL mode");
            } else if (newState == State::HEAT || newState == State::OFF) {
                rv->turnOff();
                Log.info("HP", "RV turned OFF for %s mode",
                         newState == State::HEAT ? "HEAT" : "OFF");
            }
        }
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

uint32_t GoodmanHP::getHeatRuntimeMs() const {
    return _heatRuntimeMs;
}

void GoodmanHP::setHeatRuntimeMs(uint32_t ms) {
    _heatRuntimeMs = ms;
    Log.info("HP", "Heat runtime restored: %lu ms (%lu min)", ms, ms / 60000UL);
}

void GoodmanHP::resetHeatRuntime() {
    _heatRuntimeMs = 0;
    _heatRuntimeLastLogMs = 0;
    _defrostRecheckTick = 0;
}

bool GoodmanHP::isSoftwareDefrostActive() const {
    return _softwareDefrost;
}

void GoodmanHP::accumulateHeatRuntime() {
    uint32_t now = millis();

    if (_state == State::COOL) {
        if (_heatRuntimeMs > 0) {
            Log.info("HP", "Switched to COOL, resetting heat runtime (%lu min accumulated)", _heatRuntimeMs / 60000UL);
            resetHeatRuntime();
        }
        _heatRuntimeLastTick = now;
        return;
    }

    OutPin* cnt = getOutput("CNT");
    if (_state == State::HEAT && cnt != nullptr && cnt->isOn() && !_softwareDefrost) {
        uint32_t delta = now - _heatRuntimeLastTick;
        _heatRuntimeMs += delta;

        // Log every 5 minutes of accumulated runtime
        uint32_t logInterval = 5UL * 60 * 1000;
        if (_heatRuntimeMs / logInterval > _heatRuntimeLastLogMs / logInterval) {
            _heatRuntimeLastLogMs = _heatRuntimeMs;
            Log.info("HP", "Heat runtime accumulated: %lu min", _heatRuntimeMs / 60000UL);
        }
    }

    _heatRuntimeLastTick = now;
}

void GoodmanHP::checkDefrostNeeded() {
    uint32_t now = millis();

    // DFT emergency defrost: enforce 5-min minimum, then check condenser temp
    if (_dftDefrost) {
        uint32_t elapsed = now - _dftDefrostStartTick;
        if (elapsed < DFT_MIN_RUNTIME_MS) {
            // Still within minimum runtime, keep running
            return;
        }
        TempSensor* condenser = getTempSensor("CONDENSER_TEMP");
        if (condenser != nullptr && condenser->isValid() && condenser->getValue() > DEFROST_EXIT_F) {
            Log.info("HP", "DFT defrost complete: condenser %.1fF > %.1fF after %lu sec",
                     condenser->getValue(), DEFROST_EXIT_F, elapsed / 1000UL);
            _dftDefrost = false;
            _dftDefrostStartTick = 0;
            resetHeatRuntime();
        } else if (elapsed >= DEFROST_TIMEOUT_MS) {
            Log.error("HP", "DFT defrost timeout (%lu min), forcing exit", DEFROST_TIMEOUT_MS / 60000UL);
            _dftDefrost = false;
            _dftDefrostStartTick = 0;
            resetHeatRuntime();
        }
        return;
    }

    // If defrost was triggered but Y dropped, keep persistent flag
    if (_defrostPersistent && !_softwareDefrost) {
        InputPin* y = getInput("Y");
        InputPin* o = getInput("O");
        if (y != nullptr && o != nullptr && y->isActive() && !o->isActive()) {
            // Y returned without O (HEAT mode) — resume defrost
            Log.info("HP", "HEAT mode returned during persistent defrost, restarting software defrost");
            startSoftwareDefrost();
        }
        return;
    }

    // If software defrost is active, check exit conditions
    if (_softwareDefrost) {
        TempSensor* condenser = getTempSensor("CONDENSER_TEMP");
        if (condenser != nullptr && condenser->isValid() && condenser->getValue() > DEFROST_EXIT_F) {
            Log.info("HP", "Condenser temp %.1fF > %.1fF, ending software defrost", condenser->getValue(), DEFROST_EXIT_F);
            stopSoftwareDefrost();
            return;
        }

        // Safety timeout
        if (now - _defrostStartTick >= DEFROST_TIMEOUT_MS) {
            Log.error("HP", "Software defrost timeout (%lu min), forcing stop", DEFROST_TIMEOUT_MS / 60000UL);
            stopSoftwareDefrost();
            return;
        }
        return;
    }

    // Check if heat runtime threshold reached
    if (_heatRuntimeMs < HEAT_RUNTIME_THRESHOLD_MS) {
        return;
    }

    // If a recheck is scheduled and not due yet, skip
    if (_defrostRecheckTick > 0 && now < _defrostRecheckTick) {
        return;
    }

    TempSensor* condenser = getTempSensor("CONDENSER_TEMP");
    if (condenser == nullptr || !condenser->isValid()) {
        Log.warn("HP", "Condenser temp not available, scheduling recheck in 10 min");
        _defrostRecheckTick = now + DEFROST_RECHECK_MS;
        return;
    }

    if (condenser->getValue() < DEFROST_TRIGGER_F) {
        Log.info("HP", "Condenser temp %.1fF < %.1fF after %lu min HEAT runtime, starting defrost",
                 condenser->getValue(), DEFROST_TRIGGER_F, _heatRuntimeMs / 60000UL);
        startSoftwareDefrost();
    } else {
        Log.info("HP", "Condenser temp %.1fF >= %.1fF after %lu min, defrost not needed, recheck in 10 min",
                 condenser->getValue(), DEFROST_TRIGGER_F, _heatRuntimeMs / 60000UL);
        _defrostRecheckTick = now + DEFROST_RECHECK_MS;
    }
}

void GoodmanHP::startSoftwareDefrost() {
    OutPin* cnt = getOutput("CNT");
    OutPin* rv = getOutput("RV");

    if (cnt == nullptr || rv == nullptr) {
        Log.error("HP", "Cannot start software defrost: CNT or RV output not found");
        return;
    }

    Log.info("HP", "Starting software defrost cycle");

    // Turn off CNT first (will track off-tick for short-cycle protection)
    cnt->turnOff();
    _cntActivated = false;

    // Turn on RV (reversing valve)
    rv->turnOn();

    // Turn on CNT (will honor existing activation delay)
    cnt->turnOn();
    _cntActivated = true;

    _softwareDefrost = true;
    _defrostPersistent = true;
    _defrostStartTick = millis();
    _defrostRecheckTick = 0;
}

void GoodmanHP::stopSoftwareDefrost() {
    OutPin* cnt = getOutput("CNT");
    OutPin* rv = getOutput("RV");

    Log.info("HP", "Stopping software defrost cycle");

    if (cnt != nullptr) {
        cnt->turnOff();
        _cntActivated = false;
    }
    if (rv != nullptr) {
        rv->turnOff();
    }

    _softwareDefrost = false;
    _defrostPersistent = false;
    resetHeatRuntime();
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
