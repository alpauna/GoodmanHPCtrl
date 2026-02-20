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
    , _cntShortCycleMs(DEFAULT_CNT_SHORT_CYCLE_MS)
    , _defrostMinRuntimeMs(DEFROST_MIN_RUNTIME_MS)
    , _defrostExitTempF(DEFROST_EXIT_F)
    , _heatRuntimeThresholdMs(HEAT_RUNTIME_THRESHOLD_MS)
    , _heatRuntimeMs(0)
    , _heatRuntimeLastTick(0)
    , _heatRuntimeLastLogMs(0)
    , _softwareDefrost(false)
    , _defrostStartTick(0)
    , _defrostLastCondCheckTick(0)
    , _lpsFault(false)
    , _lowTemp(false)
    , _lowTempThreshold(DEFAULT_LOW_TEMP_F)
    , _lowTempEnableW(true)
    , _lowTempEnableAux(true)
    , _compressorOverTemp(false)
    , _compressorOverTempStartTick(0)
    , _compressorOverTempLastCheckTick(0)
    , _suctionLowTemp(false)
    , _suctionLowTempStartTick(0)
    , _suctionLowTempLastCheckTick(0)
    , _rvFail(false)
    , _highSuctionTemp(false)
    , _highSuctionTempThreshold(DEFAULT_HIGH_SUCTION_TEMP_F)
    , _rvShortCycleMs(DEFAULT_RV_SHORT_CYCLE_MS)
    , _defrostTransition(false)
    , _defrostTransitionStart(0)
    , _defrostCntPending(false)
    , _defrostCntPendingStart(0)
    , _defrostExiting(false)
    , _coolTransition(false)
    , _coolTransitionStart(0)
    , _coolCntPending(false)
    , _coolCntPendingStart(0)
    , _manualOverride(false)
    , _manualOverrideStart(0)
    , _startupLockout(true)
    , _startupTick(0)
    , _lastValidateTick(0)
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

    _startupLockout = true;
    _startupTick = millis();

    _tskUpdate->enable();
    _tskCheckTemps->enable();
    Log.info("HP", "GoodmanHP controller started, all outputs verified OFF, %lu sec startup lockout",
             STARTUP_LOCKOUT_MS / 1000UL);
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
    // Startup lockout: keep all outputs OFF until sensors have stabilized
    if (_startupLockout) {
        if (millis() - _startupTick >= STARTUP_LOCKOUT_MS) {
            _startupLockout = false;
            Log.info("HP", "Startup lockout complete, enabling output control");
        } else {
            return;
        }
    }

    // Manual override: skip state machine, only check timeout
    if (_manualOverride) {
        if (millis() - _manualOverrideStart >= MANUAL_OVERRIDE_TIMEOUT_MS) {
            Log.warn("HP", "Manual override timeout (30 min), disabling");
            setManualOverride(false);
        }
        return;
    }

    // Global rule: W must be off whenever O is active (any state)
    if (isOActive()) {
        OutPin* w = getOutput("W");
        if (w != nullptr && w->isOn()) {
            w->turnOff();
            Log.info("HP", "W turned OFF (O active)");
        }
    }

    checkCompressorTemp();
    checkSuctionTemp();
    checkHighSuctionTemp();
    checkLPSFault();
    checkAmbientTemp();
    checkYAndActivateCNT();
    accumulateHeatRuntime();
    updateState();
    checkDefrostNeeded();
    checkCoolTransition();
    validateOutputStates();
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

        // W requires all 3 gates: LOW_TEMP + checkbox + Y active (O acts like checkbox off)
        if (_lowTempEnableW && isYActive() && !isOActive()) {
            OutPin* w = getOutput("W");
            if (w != nullptr) {
                w->turnOn();
                Log.info("HP", "W turned ON for LOW_TEMP mode (Y active)");
            }
        }

        // AUX only cares about LOW_TEMP + checkbox
        if (_lowTempEnableAux) {
            OutPin* aux = getOutput("AUX");
            if (aux != nullptr) aux->turnOn();
        }

        if (_stateChangeCb) _stateChangeCb(State::LOW_TEMP, oldState);
    } else if (temp < _lowTempThreshold && _lowTemp) {
        // Already in LOW_TEMP — continuously manage W based on 3 gates:
        // (1) LOW_TEMP active, (2) W checkbox, (3) Y active + O not active
        OutPin* w = getOutput("W");
        if (w != nullptr) {
            bool wShouldBeOn = _lowTempEnableW && isYActive() && !isOActive();
            if (wShouldBeOn && !w->isOn()) {
                w->turnOn();
                Log.info("HP", "W turned ON in LOW_TEMP (Y activated)");
            } else if (!wShouldBeOn && w->isOn()) {
                w->turnOff();
                Log.info("HP", "W turned OFF in LOW_TEMP");
            }
        }
        return;
    } else if (temp >= _lowTempThreshold && _lowTemp) {
        _lowTemp = false;
        Log.info("HP", "Ambient temp %.1fF >= %.1fF threshold, exiting LOW_TEMP state",
                 temp, _lowTempThreshold);

        // Turn off W
        OutPin* w = getOutput("W");
        if (w != nullptr) w->turnOff();

        // Turn off AUX signal output
        OutPin* aux = getOutput("AUX");
        if (aux != nullptr) aux->turnOff();

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

void GoodmanHP::checkSuctionTemp() {
    // Only applies in COOL mode
    if (_state != State::COOL && !_suctionLowTemp) return;

    uint32_t now = millis();

    // If already in suction low temp fault, recheck for recovery
    if (_suctionLowTemp) {
        // Auto-clear if no longer in COOL mode
        if (_state != State::COOL && _state != State::ERROR) {
            uint32_t elapsed = now - _suctionLowTempStartTick;
            Log.info("HP", "Suction low temp cleared: no longer in COOL mode, resolved in %lu min %lu sec",
                     elapsed / 60000UL, (elapsed / 1000UL) % 60);
            _suctionLowTemp = false;
            if (_stateChangeCb) _stateChangeCb(_state, _state);
            return;
        }

        if (now - _suctionLowTempLastCheckTick < SUCTION_CHECK_MS) return;
        _suctionLowTempLastCheckTick = now;

        TempSensor* suction = getTempSensor("SUCTION_TEMP");
        if (suction == nullptr || !suction->isValid()) return;

        float temp = suction->getValue();
        Log.info("HP", "Suction low temp recheck: %.1fF (recovery > %.1fF)", temp, SUCTION_RESUME_F);

        if (temp > SUCTION_RESUME_F) {
            uint32_t elapsed = now - _suctionLowTempStartTick;
            Log.warn("HP", "Suction low temp cleared: %.1fF > %.1fF, resolved in %lu min %lu sec",
                     temp, SUCTION_RESUME_F, elapsed / 60000UL, (elapsed / 1000UL) % 60);
            _suctionLowTemp = false;
            if (_stateChangeCb) _stateChangeCb(_state, _state);
        }
        return;
    }

    // Only check every 1 minute for new condition
    if (now - _suctionLowTempLastCheckTick < SUCTION_CHECK_MS) return;
    _suctionLowTempLastCheckTick = now;

    TempSensor* suction = getTempSensor("SUCTION_TEMP");
    if (suction == nullptr || !suction->isValid()) return;

    float temp = suction->getValue();

    if (temp < SUCTION_CRITICAL_F) {
        _suctionLowTemp = true;
        _suctionLowTempStartTick = now;
        Log.error("HP", "Suction temp critically low: %.1fF < %.1fF, shutting down CNT (FAN stays on)",
                  temp, SUCTION_CRITICAL_F);

        OutPin* cnt = getOutput("CNT");
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
        }

        // Keep FAN on
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr && !fan->isOn()) {
            fan->turnOn();
            Log.info("HP", "FAN kept ON during suction low temp");
        }

        if (_stateChangeCb) _stateChangeCb(_state, _state);
    } else if (temp < SUCTION_WARN_F) {
        Log.warn("HP", "Suction temp low: %.1fF < %.1fF", temp, SUCTION_WARN_F);
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
            // RV and W off — system shuts down, but _softwareDefrost stays set
            // so defrost resumes on next Y activation in HEAT mode
            OutPin* rv = getOutput("RV");
            if (rv != nullptr) rv->turnOff();
            OutPin* w = getOutput("W");
            if (w != nullptr) w->turnOff();
            _defrostTransition = false;
            _defrostCntPending = false;
            Log.info("HP", "Y dropped during defrost, system shutdown (defrost pending)");
        }
        if (_defrostExiting) {
            // Cancel exit transition — all outputs already off from Y-drop above
            OutPin* rv = getOutput("RV");
            OutPin* w = getOutput("W");
            if (rv != nullptr) rv->turnOff();
            if (w != nullptr) w->turnOff();
            _defrostExiting = false;
            _defrostTransition = false;
            _defrostCntPending = false;
            Log.info("HP", "Y dropped during defrost exit, exit cancelled");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        if (_lpsFault || _lowTemp || _compressorOverTemp || _suctionLowTemp || _rvFail || _softwareDefrost || _defrostExiting || _coolTransition || _coolCntPending) return;
        // Check if CNT was off for less than 5 minutes - if so, enforce short cycle delay
        uint32_t offElapsed = millis() - cnt->getOffTick();
        if (cnt->getOffTick() > 0 && offElapsed < 5UL * 60 * 1000) {
            // Y still active, CNT off < 5 min - check if short cycle delay has passed
            uint32_t elapsed = millis() - _yActiveStartTick;
            if (elapsed >= _cntShortCycleMs) {
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

    if (_softwareDefrost && y->isActive() && !o->isActive()) {
        newState = State::DEFROST;
    } else if (_softwareDefrost && y->isActive() && o->isActive()) {
        // Thermostat switched to COOL during pending defrost — cancel defrost
        Log.info("HP", "COOL mode requested during defrost, cancelling defrost and clearing heat runtime");
        OutPin* cnt = getOutput("CNT");
        OutPin* w = getOutput("W");
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (w != nullptr) w->turnOff();
        // RV left as-is: if on (Phase 2/3), stays on for COOL; if off (Phase 1), cool transition handles it
        _softwareDefrost = false;
        _defrostTransition = false;
        _defrostCntPending = false;
        _defrostExiting = false;
        resetHeatRuntime();
        newState = State::COOL;
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

        // Control RV based on mode
        OutPin* rv = getOutput("RV");
        if (rv != nullptr && !_softwareDefrost) {
            if (newState == State::COOL) {
                // Cancel any defrost exit in progress
                if (_defrostExiting) {
                    _defrostExiting = false;
                    _defrostTransition = false;
                    _defrostCntPending = false;
                    Log.info("HP", "Defrost exit cancelled (entering COOL)");
                }
                if (!rv->isOn()) {
                    // RV off → sequenced transition: CNT off → wait RV SC → RV on → wait CNT SC → CNT on
                    OutPin* cnt = getOutput("CNT");
                    OutPin* fan = getOutput("FAN");
                    if (cnt != nullptr && cnt->isOn()) { cnt->turnOff(); _cntActivated = false; }
                    if (fan != nullptr) fan->turnOff();
                    _coolTransition = true;
                    _coolTransitionStart = millis();
                    _coolCntPending = false;
                    Log.info("HP", "Starting COOL transition (%lu s RV short cycle)", _rvShortCycleMs / 1000UL);
                } else {
                    Log.info("HP", "RV already ON for COOL mode");
                }
            } else if (!_defrostExiting) {
                if (newState == State::HEAT || newState == State::OFF) {
                    rv->turnOff();
                    Log.info("HP", "RV turned OFF for %s mode",
                             newState == State::HEAT ? "HEAT" : "OFF");
                }
            }
        }

        // Control W: always OFF in COOL; ON in DEFROST (after Phase 1), HEAT with RV fail
        OutPin* w = getOutput("W");
        if (w != nullptr) {
            if (newState == State::COOL) {
                // W always off in COOL mode, overrides defrost exit
                if (w->isOn()) {
                    w->turnOff();
                    Log.info("HP", "W turned OFF for COOL mode");
                }
            } else if (newState == State::DEFROST && !_defrostTransition) {
                w->turnOn();
                Log.info("HP", "W turned ON for DEFROST mode");
            } else if (newState == State::HEAT && _rvFail) {
                w->turnOn();
                Log.info("HP", "W turned ON for HEAT mode (RV fail — auxiliary heat)");
            } else if (!_defrostExiting) {
                w->turnOff();
                Log.info("HP", "W turned OFF for %s mode", getStateString());
            }
        }

        // Resume defrost from Phase 1 when Y returns in HEAT mode
        if (newState == State::DEFROST && _softwareDefrost && oldState != State::DEFROST) {
            OutPin* cnt = getOutput("CNT");
            OutPin* dfFan = getOutput("FAN");
            Log.info("HP", "Defrost resuming, restarting transition from Phase 1 (%lu s RV short cycle)",
                     _rvShortCycleMs / 1000UL);
            if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
            if (dfFan != nullptr) dfFan->turnOff();
            _defrostTransition = true;
            _defrostTransitionStart = millis();
            _defrostCntPending = false;
        }

        // Control FAN: OFF during DEFROST and COOL transition, restore when leaving DEFROST if Y active
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr) {
            if (newState == State::DEFROST) {
                fan->turnOff();
                Log.info("HP", "FAN turned OFF for DEFROST mode");
            } else if (oldState == State::DEFROST && y->isActive() && !_defrostExiting && !_coolTransition) {
                // Leaving defrost with Y still active and no transition pending
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

bool GoodmanHP::isSuctionLowTempActive() const {
    return _suctionLowTemp;
}

bool GoodmanHP::isRvFailActive() const {
    return _rvFail;
}

bool GoodmanHP::isHighSuctionTempActive() const {
    return _highSuctionTemp;
}

bool GoodmanHP::isDefrostTransitionActive() const {
    return _defrostTransition;
}

void GoodmanHP::clearRvFail() {
    _rvFail = false;
    _highSuctionTemp = false;
    // Turn off W that was enabled for auxiliary heat during RV fail
    OutPin* w = getOutput("W");
    if (w != nullptr && w->isOn()) {
        w->turnOff();
        Log.info("HP", "W turned OFF (RV fail cleared)");
    }
    Log.info("HP", "RV fail cleared");
}

void GoodmanHP::setRvFail() {
    _rvFail = true;
    Log.warn("HP", "RV fail state restored from config");
}

void GoodmanHP::setHighSuctionTempThreshold(float f) {
    _highSuctionTempThreshold = f;
    Log.info("HP", "High suction temp threshold set to %.1fF", f);
}

float GoodmanHP::getHighSuctionTempThreshold() const {
    return _highSuctionTempThreshold;
}

void GoodmanHP::setRvShortCycleMs(uint32_t ms) {
    _rvShortCycleMs = ms;
    Log.info("HP", "RV short cycle set to %lu ms", ms);
}

uint32_t GoodmanHP::getRvShortCycleMs() const {
    return _rvShortCycleMs;
}

void GoodmanHP::setCntShortCycleMs(uint32_t ms) {
    _cntShortCycleMs = ms;
    Log.info("HP", "CNT short cycle set to %lu ms", ms);
}

uint32_t GoodmanHP::getCntShortCycleMs() const {
    return _cntShortCycleMs;
}

uint32_t GoodmanHP::getDefrostTransitionRemainingMs() const {
    if (!_defrostTransition) return 0;
    uint32_t elapsed = millis() - _defrostTransitionStart;
    if (elapsed >= _rvShortCycleMs) return 0;
    return _rvShortCycleMs - elapsed;
}

bool GoodmanHP::isDefrostCntPendingActive() const {
    return _defrostCntPending;
}

bool GoodmanHP::isDefrostExitingActive() const {
    return _defrostExiting;
}

uint32_t GoodmanHP::getDefrostCntPendingRemainingMs() const {
    if (!_defrostCntPending) return 0;
    uint32_t elapsed = millis() - _defrostCntPendingStart;
    if (elapsed >= _cntShortCycleMs) return 0;
    return _cntShortCycleMs - elapsed;
}

bool GoodmanHP::isCoolTransitionActive() const {
    return _coolTransition;
}

bool GoodmanHP::isCoolCntPendingActive() const {
    return _coolCntPending;
}

uint32_t GoodmanHP::getCoolTransitionRemainingMs() const {
    if (!_coolTransition) return 0;
    uint32_t elapsed = millis() - _coolTransitionStart;
    if (elapsed >= _rvShortCycleMs) return 0;
    return _rvShortCycleMs - elapsed;
}

uint32_t GoodmanHP::getCoolCntPendingRemainingMs() const {
    if (!_coolCntPending) return 0;
    uint32_t elapsed = millis() - _coolCntPendingStart;
    if (elapsed >= _cntShortCycleMs) return 0;
    return _cntShortCycleMs - elapsed;
}

void GoodmanHP::checkCoolTransition() {
    if (!_coolTransition && !_coolCntPending) return;

    // Abort if state changed away from COOL (fault or LOW_TEMP took over)
    if (_state != State::COOL) {
        _coolTransition = false;
        _coolCntPending = false;
        Log.info("HP", "COOL transition cancelled (state: %s)", getStateString());
        return;
    }

    // Abort if Y or O dropped
    if (!isYActive() || !isOActive()) {
        OutPin* rv = getOutput("RV");
        OutPin* cnt = getOutput("CNT");
        OutPin* fan = getOutput("FAN");
        if (rv != nullptr && rv->isOn()) rv->turnOff();
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (fan != nullptr) fan->turnOff();
        _coolTransition = false;
        _coolCntPending = false;
        Log.info("HP", "COOL transition cancelled (Y or O dropped)");
        return;
    }

    uint32_t now = millis();

    // Phase 1: Pressure equalization (CNT+W off, RV still off, wait RV SC)
    if (_coolTransition) {
        if (now - _coolTransitionStart >= _rvShortCycleMs) {
            _coolTransition = false;
            Log.info("HP", "COOL Phase 1 complete, RV on, waiting %lu s CNT short cycle",
                     _cntShortCycleMs / 1000UL);
            OutPin* rv = getOutput("RV");
            if (rv != nullptr) rv->turnOn();
            _coolCntPending = true;
            _coolCntPendingStart = now;
        }
        return;
    }

    // Phase 2: RV on, waiting CNT short cycle
    if (_coolCntPending) {
        if (now - _coolCntPendingStart >= _cntShortCycleMs) {
            _coolCntPending = false;
            Log.info("HP", "COOL Phase 2 complete, CNT+FAN on — COOL mode active");
            OutPin* cnt = getOutput("CNT");
            OutPin* fan = getOutput("FAN");
            if (cnt != nullptr) {
                cnt->turnOn();
                _cntActivated = true;
            }
            if (fan != nullptr) fan->turnOn();
        }
    }
}

void GoodmanHP::validateOutputStates() {
    uint32_t now = millis();
    if (now - _lastValidateTick < STATE_VALIDATE_MS) return;
    _lastValidateTick = now;

    // Skip during transitions — those manage their own output states
    if (_defrostTransition || _defrostCntPending || _defrostExiting ||
        _coolTransition || _coolCntPending || _startupLockout || _manualOverride) return;

    OutPin* cnt = getOutput("CNT");
    OutPin* fan = getOutput("FAN");
    OutPin* w   = getOutput("W");
    OutPin* rv  = getOutput("RV");
    bool corrected = false;

    // === GLOBAL INVARIANTS (always enforced) ===

    // W must NEVER be on when O is active
    if (w != nullptr && w->isOn() && isOActive()) {
        Log.error("HP", "STATE CHECK: W on while O active — forcing OFF");
        w->turnOff();
        corrected = true;
    }

    // CNT must be OFF when any blocking condition is active
    if (cnt != nullptr && cnt->isOn()) {
        if (_lpsFault) {
            Log.error("HP", "STATE CHECK: CNT on during LPS fault — forcing OFF");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        } else if (_lowTemp) {
            Log.error("HP", "STATE CHECK: CNT on during LOW_TEMP — forcing OFF");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        } else if (_compressorOverTemp) {
            Log.error("HP", "STATE CHECK: CNT on during compressor overtemp — forcing OFF");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        } else if (_suctionLowTemp) {
            Log.error("HP", "STATE CHECK: CNT on during suction low temp — forcing OFF");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        } else if (_rvFail && !_softwareDefrost) {
            Log.error("HP", "STATE CHECK: CNT on during RV fail — forcing OFF");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        }
    }

    // === PER-STATE VALIDATION ===

    switch (_state) {
        case State::OFF:
            if (cnt != nullptr && cnt->isOn()) {
                Log.error("HP", "STATE CHECK: CNT on in OFF — forcing OFF");
                cnt->turnOff(); _cntActivated = false; corrected = true;
            }
            if (fan != nullptr && fan->isOn()) {
                Log.error("HP", "STATE CHECK: FAN on in OFF — forcing OFF");
                fan->turnOff(); corrected = true;
            }
            if (w != nullptr && w->isOn()) {
                Log.error("HP", "STATE CHECK: W on in OFF — forcing OFF");
                w->turnOff(); corrected = true;
            }
            if (rv != nullptr && rv->isOn()) {
                Log.error("HP", "STATE CHECK: RV on in OFF — forcing OFF");
                rv->turnOff(); corrected = true;
            }
            break;

        case State::COOL:
            // RV must be ON
            if (rv != nullptr && !rv->isOn()) {
                Log.error("HP", "STATE CHECK: RV off in COOL — forcing ON");
                rv->turnOn(); corrected = true;
            }
            // W must be OFF
            if (w != nullptr && w->isOn()) {
                Log.error("HP", "STATE CHECK: W on in COOL — forcing OFF");
                w->turnOff(); corrected = true;
            }
            // FAN should match CNT (both on or FAN on for cooling during fault)
            if (cnt != nullptr && cnt->isOn() && fan != nullptr && !fan->isOn()) {
                Log.error("HP", "STATE CHECK: FAN off while CNT on in COOL — forcing ON");
                fan->turnOn(); corrected = true;
            }
            break;

        case State::HEAT:
            // RV must be OFF
            if (rv != nullptr && rv->isOn()) {
                Log.error("HP", "STATE CHECK: RV on in HEAT — forcing OFF");
                rv->turnOff(); corrected = true;
            }
            // W only allowed if _rvFail
            if (w != nullptr && w->isOn() && !_rvFail) {
                Log.error("HP", "STATE CHECK: W on in HEAT without RV fail — forcing OFF");
                w->turnOff(); corrected = true;
            }
            // FAN should be on if CNT is on
            if (cnt != nullptr && cnt->isOn() && fan != nullptr && !fan->isOn()) {
                Log.error("HP", "STATE CHECK: FAN off while CNT on in HEAT — forcing ON");
                fan->turnOn(); corrected = true;
            }
            break;

        case State::DEFROST:
            // Phase 3 (active defrost): CNT on, FAN off, W on, RV on
            if (_softwareDefrost) {
                if (rv != nullptr && !rv->isOn()) {
                    Log.error("HP", "STATE CHECK: RV off during active defrost — forcing ON");
                    rv->turnOn(); corrected = true;
                }
                if (w != nullptr && !w->isOn()) {
                    Log.error("HP", "STATE CHECK: W off during active defrost — forcing ON");
                    w->turnOn(); corrected = true;
                }
                if (fan != nullptr && fan->isOn()) {
                    Log.error("HP", "STATE CHECK: FAN on during active defrost — forcing OFF");
                    fan->turnOff(); corrected = true;
                }
            }
            break;

        case State::ERROR:
            // CNT must be OFF
            if (cnt != nullptr && cnt->isOn()) {
                Log.error("HP", "STATE CHECK: CNT on in ERROR — forcing OFF");
                cnt->turnOff(); _cntActivated = false; corrected = true;
            }
            break;

        case State::LOW_TEMP:
            // CNT OFF, FAN OFF, RV OFF
            if (cnt != nullptr && cnt->isOn()) {
                Log.error("HP", "STATE CHECK: CNT on in LOW_TEMP — forcing OFF");
                cnt->turnOff(); _cntActivated = false; corrected = true;
            }
            if (fan != nullptr && fan->isOn()) {
                Log.error("HP", "STATE CHECK: FAN on in LOW_TEMP — forcing OFF");
                fan->turnOff(); corrected = true;
            }
            if (rv != nullptr && rv->isOn()) {
                Log.error("HP", "STATE CHECK: RV on in LOW_TEMP — forcing OFF");
                rv->turnOff(); corrected = true;
            }
            break;
    }

    if (corrected && _stateChangeCb) {
        _stateChangeCb(_state, _state);
    }
}

void GoodmanHP::setDefrostMinRuntimeMs(uint32_t ms) {
    _defrostMinRuntimeMs = ms;
    Log.info("HP", "Defrost min runtime set to %lu ms", ms);
}

uint32_t GoodmanHP::getDefrostMinRuntimeMs() const {
    return _defrostMinRuntimeMs;
}

void GoodmanHP::setDefrostExitTempF(float f) {
    _defrostExitTempF = f;
    Log.info("HP", "Defrost exit temp set to %.1fF", f);
}

float GoodmanHP::getDefrostExitTempF() const {
    return _defrostExitTempF;
}

void GoodmanHP::setHeatRuntimeThresholdMs(uint32_t ms) {
    _heatRuntimeThresholdMs = ms;
    Log.info("HP", "Heat runtime threshold set to %lu ms (%lu min)", ms, ms / 60000UL);
}

uint32_t GoodmanHP::getHeatRuntimeThresholdMs() const {
    return _heatRuntimeThresholdMs;
}

void GoodmanHP::checkHighSuctionTemp() {
    // Only check during active defrost (after both transition phases)
    if (!_softwareDefrost || _defrostTransition || _defrostCntPending) return;

    TempSensor* suction = getTempSensor("SUCTION_TEMP");
    if (suction == nullptr || !suction->isValid()) return;

    float temp = suction->getValue();

    if (temp >= _highSuctionTempThreshold && !_highSuctionTemp) {
        _highSuctionTemp = true;
        _rvFail = true;
        Log.error("HP", "HIGH SUCTION TEMP: %.1fF >= %.1fF during defrost — RV FAIL detected",
                  temp, _highSuctionTempThreshold);
        Log.error("HP", "RV fail latched — CNT blocked until cleared via config page");

        // Stop CNT immediately, keep FAN on
        OutPin* cnt = getOutput("CNT");
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
        }

        // Keep FAN on to dissipate heat
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr && !fan->isOn()) {
            fan->turnOn();
            Log.info("HP", "FAN turned ON (RV fail — dissipate heat)");
        }

        // Turn on W for auxiliary heat if in HEAT mode (Y active, O not active)
        OutPin* w = getOutput("W");
        if (w != nullptr && isYActive() && !isOActive()) {
            w->turnOn();
            Log.info("HP", "W turned ON for RV fail (auxiliary heat)");
        }

        // Stop defrost
        OutPin* rv = getOutput("RV");
        if (rv != nullptr) rv->turnOff();
        _softwareDefrost = false;
        resetHeatRuntime();

        if (_stateChangeCb) _stateChangeCb(_state, _state);
    }
}

bool GoodmanHP::isStartupLockoutActive() const {
    return _startupLockout;
}

uint32_t GoodmanHP::getStartupLockoutRemainingMs() const {
    if (!_startupLockout) return 0;
    uint32_t elapsed = millis() - _startupTick;
    if (elapsed >= STARTUP_LOCKOUT_MS) return 0;
    return STARTUP_LOCKOUT_MS - elapsed;
}

bool GoodmanHP::isShortCycleProtectionActive() const {
    auto it = _outputMap.find("CNT");
    if (it == _outputMap.end() || it->second == nullptr) return false;
    OutPin* cnt = it->second;
    // Short cycle protection is active when CNT is off, has been off before,
    // and less than 5 minutes have elapsed since it turned off
    if (cnt->isPinOn() || cnt->getOffTick() == 0) return false;
    uint32_t offElapsed = millis() - cnt->getOffTick();
    return offElapsed < 5UL * 60 * 1000;
}

void GoodmanHP::setLowTempThreshold(float threshold) {
    _lowTempThreshold = threshold;
    Log.info("HP", "Low temp threshold set to %.1fF", threshold);
}

float GoodmanHP::getLowTempThreshold() const {
    return _lowTempThreshold;
}

void GoodmanHP::setLowTempEnableW(bool enable) {
    _lowTempEnableW = enable;
}

bool GoodmanHP::getLowTempEnableW() const {
    return _lowTempEnableW;
}

void GoodmanHP::setLowTempEnableAux(bool enable) {
    _lowTempEnableAux = enable;
}

bool GoodmanHP::getLowTempEnableAux() const {
    return _lowTempEnableAux;
}

void GoodmanHP::restoreSoftwareDefrost() {
    _softwareDefrost = true;
    Log.warn("HP", "Software defrost state restored from config");
}

void GoodmanHP::setStateChangeCallback(StateChangeCallback cb) {
    _stateChangeCb = cb;
}

void GoodmanHP::setLPSFaultCallback(LPSFaultCallback cb) {
    _lpsFaultCb = cb;
}

void GoodmanHP::accumulateHeatRuntime() {
    uint32_t now = millis();

    // COOL, DEFROST, and DFT off (temps > 32°F, no ice) clear accumulated runtime
    if (_state == State::COOL) {
        if (_heatRuntimeMs > 0) {
            Log.info("HP", "Switched to COOL, resetting heat runtime (%lu min accumulated)", _heatRuntimeMs / 60000UL);
            resetHeatRuntime();
        }
        _heatRuntimeLastTick = now;
        return;
    }

    // DFT off means temps > 32°F — no ice on coils, clear runtime
    if (!isDFTActive() && _heatRuntimeMs > 0 && !_softwareDefrost) {
        Log.info("HP", "DFT off (temps > 32F), resetting heat runtime (%lu min accumulated)", _heatRuntimeMs / 60000UL);
        resetHeatRuntime();
        _heatRuntimeLastTick = now;
        return;
    }

    // Only accumulate in HEAT mode when CNT is on, DFT is active (closed at 32°F),
    // and not currently in software defrost
    OutPin* cnt = getOutput("CNT");
    if (_state == State::HEAT && cnt != nullptr && cnt->isOn() && !_softwareDefrost && isDFTActive()) {
        uint32_t delta = now - _heatRuntimeLastTick;
        _heatRuntimeMs += delta;

        // Log every 5 minutes of accumulated runtime
        uint32_t logInterval = 5UL * 60 * 1000;
        if (_heatRuntimeMs / logInterval > _heatRuntimeLastLogMs / logInterval) {
            _heatRuntimeLastLogMs = _heatRuntimeMs;
            Log.info("HP", "Heat runtime accumulated: %lu min (DFT active)", _heatRuntimeMs / 60000UL);
        }
    }

    _heatRuntimeLastTick = now;
}

void GoodmanHP::checkDefrostNeeded() {
    uint32_t now = millis();

    // Safety: abort any defrost/exit sequence if Y is not active
    InputPin* yPin = getInput("Y");
    bool yActive = (yPin != nullptr && yPin->isActive());
    if (!yActive && (_defrostExiting || _defrostTransition || _defrostCntPending)) {
        if (_defrostExiting || _defrostTransition || _defrostCntPending) {
            OutPin* rv = getOutput("RV");
            OutPin* w = getOutput("W");
            OutPin* cnt = getOutput("CNT");
            OutPin* fan = getOutput("FAN");
            if (rv != nullptr) rv->turnOff();
            if (w != nullptr) w->turnOff();
            if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
            if (fan != nullptr) fan->turnOff();
            Log.warn("HP", "Y inactive during defrost/exit sequence, all outputs OFF");
        }
        _defrostExiting = false;
        _defrostTransition = false;
        _defrostCntPending = false;
        // _softwareDefrost stays set for resume on next Y in HEAT mode
        return;
    }

    // Exit Phase 1: Pressure equalization after defrost (CNT off, RV+W still on)
    if (_defrostTransition && _defrostExiting) {
        if (now - _defrostTransitionStart >= _rvShortCycleMs) {
            _defrostTransition = false;
            Log.info("HP", "Exit Phase 1 complete, RV+W off, waiting %lu s CNT short cycle",
                     _cntShortCycleMs / 1000UL);
            OutPin* rv = getOutput("RV");
            OutPin* w = getOutput("W");
            if (rv != nullptr) rv->turnOff();
            if (w != nullptr) w->turnOff();
            _defrostCntPending = true;
            _defrostCntPendingStart = now;
        }
        return;
    }

    // Exit Phase 2: RV switched back to heat, waiting CNT short cycle
    if (_defrostCntPending && _defrostExiting) {
        if (now - _defrostCntPendingStart >= _cntShortCycleMs) {
            _defrostCntPending = false;
            _defrostExiting = false;
            Log.info("HP", "Exit Phase 2 complete, CNT+FAN on — back in HEAT mode");
            OutPin* cnt = getOutput("CNT");
            OutPin* fan = getOutput("FAN");
            if (cnt != nullptr) {
                cnt->turnOn();
                _cntActivated = true;
            }
            if (fan != nullptr) fan->turnOn();
        }
        return;
    }

    // Phase 1: Pressure equalization (all off, waiting for RV short cycle)
    if (_defrostTransition) {
        if (now - _defrostTransitionStart >= _rvShortCycleMs) {
            _defrostTransition = false;
            Log.info("HP", "Phase 1 complete, engaging RV+W, waiting %lu s CNT short cycle",
                     _cntShortCycleMs / 1000UL);

            OutPin* rv = getOutput("RV");
            OutPin* w = getOutput("W");
            if (rv != nullptr) rv->turnOn();
            if (w != nullptr) w->turnOn();

            // Enter Phase 2: CNT short cycle pending
            _defrostCntPending = true;
            _defrostCntPendingStart = now;
        }
        return;  // Don't check exit conditions during Phase 1
    }

    // Phase 2: CNT short cycle (RV+W on, waiting for CNT activation)
    if (_defrostCntPending) {
        if (now - _defrostCntPendingStart >= _cntShortCycleMs) {
            _defrostCntPending = false;
            Log.info("HP", "Phase 2 complete, engaging CNT — defrost fully active");

            OutPin* cnt = getOutput("CNT");
            if (cnt != nullptr) {
                cnt->turnOn();
                _cntActivated = true;
            }
            _defrostStartTick = now;
            _defrostLastCondCheckTick = now;
        }
        return;  // Don't check exit conditions during Phase 2
    }

    // If software defrost is active, check exit conditions
    if (_softwareDefrost) {
        // Guard: _defrostStartTick is only set when Phase 3 begins.
        // If still 0, defrost never reached active phase (Y dropped during Phase 1/2).
        // Don't check timeout — defrost will resume from Phase 1 when Y reactivates.
        if (_defrostStartTick == 0) {
            return;
        }

        uint32_t elapsed = now - _defrostStartTick;

        // Enforce minimum runtime
        if (elapsed < _defrostMinRuntimeMs) {
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
                         condTemp, _defrostExitTempF, elapsed / 1000UL);
                if (condTemp >= _defrostExitTempF) {
                    Log.info("HP", "Defrost complete: condenser %.1fF >= %.1fF",
                             condTemp, _defrostExitTempF);
                    stopSoftwareDefrost();
                    return;
                }
            }
        }
        return;
    }

    // Check if heat runtime threshold reached
    if (_heatRuntimeMs >= _heatRuntimeThresholdMs) {
        Log.info("HP", "Heat runtime %lu min >= %lu min threshold, starting defrost",
                 _heatRuntimeMs / 60000UL, _heatRuntimeThresholdMs / 60000UL);
        startSoftwareDefrost();
    }
}

void GoodmanHP::startSoftwareDefrost() {
    if (_softwareDefrost) return;

    OutPin* cnt = getOutput("CNT");
    OutPin* rv = getOutput("RV");
    OutPin* fan = getOutput("FAN");

    if (cnt == nullptr || rv == nullptr) {
        Log.error("HP", "Cannot start software defrost: CNT or RV output not found");
        return;
    }

    Log.info("HP", "Starting defrost transition (%lu s RV short cycle)", _rvShortCycleMs / 1000UL);

    // Turn off CNT and FAN during pressure equalization
    cnt->turnOff();
    _cntActivated = false;
    if (fan != nullptr) fan->turnOff();

    // Enter transition phase — do NOT turn on RV or CNT yet
    _defrostTransition = true;
    _defrostTransitionStart = millis();
    _softwareDefrost = true;
}

void GoodmanHP::stopSoftwareDefrost() {
    OutPin* cnt = getOutput("CNT");
    OutPin* fan = getOutput("FAN");

    // Safety: if Y is not active, just clean up — no exit transition
    InputPin* y = getInput("Y");
    if (y == nullptr || !y->isActive()) {
        Log.info("HP", "Defrost stopped (Y inactive), no exit transition");
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (fan != nullptr) fan->turnOff();
        OutPin* rv = getOutput("RV");
        OutPin* w = getOutput("W");
        if (rv != nullptr) rv->turnOff();
        if (w != nullptr) w->turnOff();
        _softwareDefrost = false;
        _defrostExiting = false;
        _defrostTransition = false;
        _defrostCntPending = false;
        _highSuctionTemp = false;
        _defrostStartTick = 0;
        resetHeatRuntime();
        return;
    }

    Log.info("HP", "Defrost complete, starting exit transition (%lu s pressure equalization)",
             _rvShortCycleMs / 1000UL);

    // Turn off CNT and FAN only — RV and W stay on during exit Phase 1
    if (cnt != nullptr) {
        cnt->turnOff();
        _cntActivated = false;
    }
    if (fan != nullptr) fan->turnOff();

    // Start exit transition (reuses _defrostTransition / _defrostCntPending flags)
    _defrostExiting = true;
    _defrostTransition = true;
    _defrostTransitionStart = millis();
    _defrostCntPending = false;

    // Clear defrost so state machine transitions DEFROST → HEAT
    _softwareDefrost = false;
    _highSuctionTemp = false;
    _defrostStartTick = 0;
    resetHeatRuntime();
}

bool GoodmanHP::isManualOverrideActive() const {
    return _manualOverride;
}

uint32_t GoodmanHP::getManualOverrideRemainingMs() const {
    if (!_manualOverride) return 0;
    uint32_t elapsed = millis() - _manualOverrideStart;
    if (elapsed >= MANUAL_OVERRIDE_TIMEOUT_MS) return 0;
    return MANUAL_OVERRIDE_TIMEOUT_MS - elapsed;
}

void GoodmanHP::setManualOverride(bool on) {
    if (on && !_manualOverride) {
        _manualOverride = true;
        _manualOverrideStart = millis();
        Log.warn("HP", "MANUAL OVERRIDE enabled (30 min timeout)");
        // Stop any active defrost or exit transition
        if (_softwareDefrost) {
            stopSoftwareDefrost();
        }
        _defrostExiting = false;
        _coolTransition = false;
        _coolCntPending = false;
    } else if (!on && _manualOverride) {
        _manualOverride = false;
        // Turn all outputs off and let state machine resume
        for (auto& pair : _outputMap) {
            if (pair.second != nullptr) {
                pair.second->turnOff();
            }
        }
        _cntActivated = false;
        Log.warn("HP", "MANUAL OVERRIDE disabled, all outputs OFF");
    }
}

String GoodmanHP::setManualOutput(const String& name, bool on) {
    if (!_manualOverride) return "Manual override not active";

    OutPin* pin = getOutput(name);
    if (pin == nullptr) return "Output not found: " + name;

    if (on && name == "CNT") {
        // Apply same short cycle protection as normal mode
        if (pin->getOffTick() > 0) {
            uint32_t offElapsed = millis() - pin->getOffTick();
            if (offElapsed < 5UL * 60 * 1000 && offElapsed < _cntShortCycleMs) {
                uint32_t remainSec = (_cntShortCycleMs - offElapsed + 999) / 1000;
                return "Short cycle protection: " + String(remainSec) + "s remaining";
            }
        }
    }

    if (on) {
        pin->turnOn();
    } else {
        pin->turnOff();
    }

    if (name == "CNT") _cntActivated = on;

    Log.info("HP", "Manual override: %s %s", name.c_str(), on ? "ON" : "OFF");
    return "";
}

String GoodmanHP::forceDefrost() {
    if (_manualOverride) return "Disable manual override first";
    if (_softwareDefrost) return "Defrost already active";
    if (_defrostExiting) return "Defrost exit transition active";
    if (_coolTransition || _coolCntPending) return "COOL mode transition in progress";
    if (_state != State::HEAT) return "Must be in HEAT mode (current: " + String(getStateString()) + ")";
    if (_lpsFault) return "LPS fault active";
    if (_compressorOverTemp) return "Compressor over-temp active";
    if (_lowTemp) return "Low temp protection active";
    if (_rvFail) return "RV fail active";

    Log.warn("HP", "FORCE DEFROST initiated from web interface");
    startSoftwareDefrost();
    return "";
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
