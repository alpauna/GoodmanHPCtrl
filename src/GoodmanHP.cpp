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
    , _defrostLastCondCheckTick(0)
    , _lpsFault(false)
    , _lowTemp(false)
    , _lowTempThreshold(DEFAULT_LOW_TEMP_F)
    , _compressorOverTemp(false)
    , _compressorOverTempStartTick(0)
    , _compressorOverTempLastCheckTick(0)
{
    _instance = this;
    _tskUpdate = new Task(500, TASK_FOREVER, [this]() {
        this->update();
    }, ts, false);
    _tskCheckTemps = new Task(10 * TASK_SECOND, TASK_FOREVER, [this]() {
        if (_sensors != nullptr) {
            _sensors->requestTemperatures();
        }
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
    // Ensure all outputs are OFF on startup and verify via digital read
    for (auto& pair : _outputMap) {
        if (pair.second != nullptr) {
            pair.second->turnOff();
            if (pair.second->isPinOn()) {
                Log.error("HP", "Output %s failed to turn OFF (pin still HIGH)", pair.first.c_str());
            } else {
                Log.info("HP", "Output %s verified OFF", pair.first.c_str());
            }
        }
    }
    _cntActivated = false;

    _tskUpdate->enable();
    _tskCheckTemps->enable();
    Log.info("HP", "GoodmanHP controller started, all outputs verified OFF");
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
    checkCompressorTemp();
    checkLPSFault();
    checkAmbientTemp();
    checkYAndActivateCNT();
    accumulateHeatRuntime();
    updateState();
    checkDefrostNeeded();
}

void GoodmanHP::checkLPSFault() {
    // Don't override compressor overtemp
    if (_compressorOverTemp) return;

    if (!isLPSActive() && !_lpsFault) {
        _lpsFault = true;
        State oldState = _state;
        _state = State::ERROR;
        Log.error("HP", "LPS fault: low refrigerant pressure detected");
        OutPin* cnt = getOutput("CNT");
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
            Log.error("HP", "CNT shut down due to LPS fault");
        }
        // Turn on W if in HEAT mode (Y active, O not active)
        OutPin* w = getOutput("W");
        if (w != nullptr && isYActive() && !isOActive()) {
            w->turnOn();
            Log.info("HP", "W turned ON for ERROR state (HEAT mode)");
        }
        if (_lpsFaultCb) _lpsFaultCb(true);
        if (_stateChangeCb) _stateChangeCb(State::ERROR, oldState);
    } else if (isLPSActive() && _lpsFault) {
        _lpsFault = false;
        Log.info("HP", "LPS fault cleared: pressure restored");
        // Turn off W that was enabled during ERROR
        OutPin* w = getOutput("W");
        if (w != nullptr && w->isOn()) {
            w->turnOff();
            Log.info("HP", "W turned OFF (LPS fault cleared)");
        }
        // Reset Y active start so short-cycle protection applies from recovery
        if (_yWasActive) {
            _yActiveStartTick = millis();
        }
        if (_lpsFaultCb) _lpsFaultCb(false);
        // Don't set _state here — let updateState() determine the correct state
    }
}

void GoodmanHP::checkAmbientTemp() {
    // Don't override higher-priority faults
    if (_compressorOverTemp || _lpsFault) return;

    TempSensor* ambient = getTempSensor("AMBIENT_TEMP");
    if (ambient == nullptr || !ambient->isValid()) return;

    float temp = ambient->getValue();

    if (temp < _lowTempThreshold && !_lowTemp) {
        _lowTemp = true;
        State oldState = _state;
        _state = State::LOW_TEMP;
        Log.warn("HP", "Low ambient temp %.1fF < %.1fF threshold, entering LOW_TEMP state",
                 temp, _lowTempThreshold);

        // Shut down CNT if running
        OutPin* cnt = getOutput("CNT");
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
            Log.warn("HP", "CNT shut down due to low ambient temp");
        }

        // Turn off FAN and RV
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr) fan->turnOff();
        OutPin* rv = getOutput("RV");
        if (rv != nullptr) rv->turnOff();

        // Turn on W (auxiliary heat) only if not in COOL mode (O+Y)
        OutPin* w = getOutput("W");
        if (w != nullptr && !isOActive()) {
            w->turnOn();
            Log.info("HP", "W turned ON for LOW_TEMP mode");
        }

        if (_stateChangeCb) _stateChangeCb(State::LOW_TEMP, oldState);
    } else if (temp >= _lowTempThreshold && _lowTemp) {
        _lowTemp = false;
        Log.info("HP", "Ambient temp %.1fF >= %.1fF threshold, exiting LOW_TEMP state",
                 temp, _lowTempThreshold);

        // Turn off W
        OutPin* w = getOutput("W");
        if (w != nullptr) w->turnOff();

        // Don't set _state here — let updateState() determine the correct state
    }
}

void GoodmanHP::checkCompressorTemp() {

    uint32_t now = millis();

    // If already in overtemp, recheck every 1 minute for recovery
    if (_compressorOverTemp) {
        if (now - _compressorOverTempLastCheckTick < COMPRESSOR_OVERTEMP_CHECK_MS) return;
        _compressorOverTempLastCheckTick = now;

        TempSensor* comp = getTempSensor("COMPRESSOR_TEMP");
        if (comp == nullptr || !comp->isValid()) return;

        float temp = comp->getValue();
        Log.info("HP", "Compressor overtemp recheck: %.1fF (recovery < %.1fF)", temp, COMPRESSOR_OVERTEMP_OFF_F);

        if (temp < COMPRESSOR_OVERTEMP_OFF_F) {
            uint32_t elapsed = now - _compressorOverTempStartTick;
            Log.warn("HP", "Compressor overtemp cleared: %.1fF < %.1fF, resolved in %lu min %lu sec",
                     temp, COMPRESSOR_OVERTEMP_OFF_F, elapsed / 60000UL, (elapsed / 1000UL) % 60);
            _compressorOverTemp = false;
            if (_stateChangeCb) _stateChangeCb(_state, _state);
        }
        return;
    }

    // Only check every 1 minute for new overtemp condition
    if (now - _compressorOverTempLastCheckTick < COMPRESSOR_OVERTEMP_CHECK_MS) return;
    _compressorOverTempLastCheckTick = now;

    TempSensor* comp = getTempSensor("COMPRESSOR_TEMP");
    if (comp == nullptr || !comp->isValid()) return;

    float temp = comp->getValue();

    if (temp >= COMPRESSOR_OVERTEMP_ON_F) {
        _compressorOverTemp = true;
        _compressorOverTempStartTick = now;
        Log.error("HP", "Compressor overtemp: %.1fF >= %.1fF, shutting down CNT (FAN stays on)",
                  temp, COMPRESSOR_OVERTEMP_ON_F);

        // Shut down CNT
        OutPin* cnt = getOutput("CNT");
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
        }

        // Keep FAN on to cool the compressor
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr && !fan->isOn()) {
            fan->turnOn();
            Log.info("HP", "FAN turned ON to cool compressor");
        }

        if (_stateChangeCb) _stateChangeCb(_state, _state);
    }
}

void GoodmanHP::checkYAndActivateCNT() {
    InputPin* y = getInput("Y");
    OutPin* cnt = getOutput("CNT");

    if (y == nullptr || cnt == nullptr) {
        return;
    }

    bool yActive = y->isActive();

    OutPin* fan = getOutput("FAN");

    if (yActive && !_yWasActive) {
        // Y just became active - record start time
        _yActiveStartTick = millis();
        _yWasActive = true;
        // Turn on FAN when Y activates (unless in defrost)
        if (fan != nullptr && _state != State::DEFROST) {
            fan->turnOn();
            Log.info("HP", "FAN turned ON (Y activated)");
        }
        Log.info("HP", "Y input activated, starting 30s timer");
    } else if (!yActive && _yWasActive) {
        // Y just became inactive - reset
        _yWasActive = false;
        _yActiveStartTick = 0;
        // Turn off FAN when Y deactivates
        if (fan != nullptr) {
            fan->turnOff();
            Log.info("HP", "FAN turned OFF (Y deactivated)");
        }
        if (_cntActivated) {
            cnt->turnOff();
            _cntActivated = false;
            Log.info("HP", "Y input deactivated, CNT turned off");
        }
        if (_softwareDefrost) {
            // RV off too — system shuts down, but _softwareDefrost stays set
            // so defrost resumes on next Y activation in HEAT mode
            OutPin* rv = getOutput("RV");
            if (rv != nullptr) rv->turnOff();
            Log.info("HP", "Y dropped during defrost, system shutdown (defrost pending)");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        if (_lpsFault || _lowTemp || _compressorOverTemp) return;
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
    // Don't compute new state while faulted or low temp
    if (_lpsFault || _lowTemp) return;

    InputPin* dft = getInput("DFT");
    InputPin* y = getInput("Y");
    InputPin* o = getInput("O");

    if (dft == nullptr || y == nullptr || o == nullptr) {
        return;
    }

    State newState = State::OFF;

    // DFT input triggers defrost from HEAT mode only
    bool dftTrigger = dft->isActive() && _state == State::HEAT && !_softwareDefrost;

    if (dftTrigger) {
        Log.info("HP", "DFT emergency defrost triggered from HEAT mode");
        startSoftwareDefrost();
    }

    if (_softwareDefrost && y->isActive()) {
        newState = State::DEFROST;
    } else if (y->isActive() && o->isActive()) {
        newState = State::COOL;
    } else if (y->isActive()) {
        // If _softwareDefrost is set, re-enter DEFROST is handled above
        newState = State::HEAT;
    }

    if (newState != _state) {
        State oldState = _state;
        Log.info("HP", "State changed: %s -> %s", getStateString(),
                 newState == State::OFF ? "OFF" :
                 newState == State::COOL ? "COOL" :
                 newState == State::HEAT ? "HEAT" :
                 newState == State::DEFROST ? "DEFROST" : "ERROR");
        _state = newState;

        if (_stateChangeCb) {
            _stateChangeCb(newState, oldState);
        }

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

        // Control W: ON only in DEFROST, OFF otherwise
        OutPin* w = getOutput("W");
        if (w != nullptr) {
            if (newState == State::DEFROST) {
                w->turnOn();
                Log.info("HP", "W turned ON for DEFROST mode");
            } else {
                w->turnOff();
                Log.info("HP", "W turned OFF for %s mode", getStateString());
            }
        }

        // Re-engage defrost relays when resuming defrost (Y returned after drop)
        if (newState == State::DEFROST && _softwareDefrost && oldState != State::DEFROST) {
            OutPin* cnt = getOutput("CNT");
            if (rv != nullptr) {
                rv->turnOn();
                Log.info("HP", "RV re-engaged for resumed defrost");
            }
            if (cnt != nullptr && !_cntActivated) {
                cnt->turnOn();
                _cntActivated = true;
                Log.info("HP", "CNT re-engaged for resumed defrost");
            }
        }

        // Control FAN: OFF during DEFROST, restore when leaving DEFROST if Y active
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr) {
            if (newState == State::DEFROST) {
                fan->turnOff();
                Log.info("HP", "FAN turned OFF for DEFROST mode");
            } else if (oldState == State::DEFROST && y->isActive()) {
                // Leaving defrost with Y still active — turn FAN back on
                fan->turnOn();
                Log.info("HP", "FAN turned ON (defrost complete, Y active)");
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
        case State::ERROR: return "ERROR";
        case State::LOW_TEMP: return "LOW_TEMP";
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
}

bool GoodmanHP::isSoftwareDefrostActive() const {
    return _softwareDefrost;
}

bool GoodmanHP::isLPSFaultActive() const {
    return _lpsFault;
}

bool GoodmanHP::isLowTempActive() const {
    return _lowTemp;
}

bool GoodmanHP::isCompressorOverTempActive() const {
    return _compressorOverTemp;
}

void GoodmanHP::setLowTempThreshold(float threshold) {
    _lowTempThreshold = threshold;
    Log.info("HP", "Low temp threshold set to %.1fF", threshold);
}

float GoodmanHP::getLowTempThreshold() const {
    return _lowTempThreshold;
}

void GoodmanHP::setStateChangeCallback(StateChangeCallback cb) {
    _stateChangeCb = cb;
}

void GoodmanHP::setLPSFaultCallback(LPSFaultCallback cb) {
    _lpsFaultCb = cb;
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

    // If software defrost is active, check exit conditions
    if (_softwareDefrost) {
        uint32_t elapsed = now - _defrostStartTick;

        // Enforce minimum runtime
        if (elapsed < DEFROST_MIN_RUNTIME_MS) {
            return;
        }

        // Safety timeout
        if (elapsed >= DEFROST_TIMEOUT_MS) {
            Log.error("HP", "Defrost timeout (%lu min), forcing stop", DEFROST_TIMEOUT_MS / 60000UL);
            stopSoftwareDefrost();
            return;
        }

        // Check condenser temp every 1 minute
        if (now - _defrostLastCondCheckTick >= DEFROST_COND_CHECK_MS) {
            _defrostLastCondCheckTick = now;
            TempSensor* condenser = getTempSensor("CONDENSER_TEMP");
            if (condenser != nullptr && condenser->isValid()) {
                float condTemp = condenser->getValue();
                Log.info("HP", "Defrost condenser check: %.1fF (target > %.1fF, elapsed %lu sec)",
                         condTemp, DEFROST_EXIT_F, elapsed / 1000UL);
                if (condTemp >= DEFROST_EXIT_F) {
                    Log.info("HP", "Defrost complete: condenser %.1fF >= %.1fF",
                             condTemp, DEFROST_EXIT_F);
                    stopSoftwareDefrost();
                    return;
                }
            }
        }
        return;
    }

    // Check if heat runtime threshold reached
    if (_heatRuntimeMs >= HEAT_RUNTIME_THRESHOLD_MS) {
        Log.info("HP", "Heat runtime %lu min >= %lu min threshold, starting defrost",
                 _heatRuntimeMs / 60000UL, HEAT_RUNTIME_THRESHOLD_MS / 60000UL);
        startSoftwareDefrost();
    }
}

void GoodmanHP::startSoftwareDefrost() {
    if (_softwareDefrost) return;

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
    _defrostStartTick = millis();
    _defrostLastCondCheckTick = _defrostStartTick;
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
