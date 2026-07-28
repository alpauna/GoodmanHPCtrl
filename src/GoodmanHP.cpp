#include "GoodmanHP.h"
#include "Logger.h"

// Static instance pointer for runtime callback
GoodmanHP* GoodmanHP::_instance = nullptr;

GoodmanHP::GoodmanHP(Scheduler *ts)
    : _ts(ts)
    , _sensors(nullptr)
    , _ads1115(nullptr)
    , _currentSamplingActive(false)
    , _state(State::OFF)
    , _yActiveStartTick(0)
    , _yWasActive(false)
    , _cntActivated(false)
    , _cntShortCycleMs(DEFAULT_CNT_SHORT_CYCLE_MS)
    , _coldMaxTempF(DEFROST_COLD_MAX_TEMP_F)
    , _warmMinTempF(DEFROST_WARM_MIN_TEMP_F)
    , _defrostBands{
        {DEFROST_COLD_RUNTIME_MS, DEFROST_COLD_MIN_RUNTIME_MS, DEFROST_COLD_EXIT_F},
        {DEFROST_MID_RUNTIME_MS,  DEFROST_MID_MIN_RUNTIME_MS,  DEFROST_MID_EXIT_F},
        {DEFROST_WARM_RUNTIME_MS, DEFROST_WARM_MIN_RUNTIME_MS, DEFROST_WARM_EXIT_F}
      }
    , _activeDefrostBand(DefrostBandName::WARM)
    , _defrostSnapshotBand{DEFROST_WARM_RUNTIME_MS, DEFROST_WARM_MIN_RUNTIME_MS, DEFROST_WARM_EXIT_F}
    , _heatRuntimeMs(0)
    , _heatRuntimeLastTick(0)
    , _heatRuntimeLastLogMs(0)
    , _dftOffStartTick(0)
    , _softwareDefrost(false)
    , _defrostStartTick(0)
    , _defrostLastCondCheckTick(0)
    , _lpsFault(false)
    , _lowTemp(false)
    , _lowTempThreshold(DEFAULT_LOW_TEMP_F)
    , _highAmbientHeatLockoutF(DEFAULT_HIGH_AMBIENT_HEAT_LOCKOUT_F)
    , _highAmbientHeatLockout(false)
    , _lowTempEnableW(true)
    , _lowTempEnableAux(true)
    , _lowTempPendingEntry(false)
    , _lowTempPendingExit(false)
    , _lowTempPendingTick(0)
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
    , _heatTransition(false)
    , _heatTransitionStart(0)
    , _heatCntPending(false)
    , _heatCntPendingStart(0)
    , _rvHoldActive(false)
    , _stateValidationActive(false)
    , _stateValidationStart(0)
    , _stateValidationMs(30000)
    , _lastValidationLogTick(0)
    , _ambientSource(AmbientSource::SENSOR)
    , _weatherTempF(0.0f)
    , _weatherTempValid(false)
    , _weatherTempTick(0)
    , _weatherStaleMs(30UL * 60 * 1000)
    , _internalTempOffsetF(0.0f)
    , _ambientFailoverTest(false)
    , _failoverTestStartTick(0)
    , _fanFault(false)
    , _fanFaultStartTick(0)
    , _fanNoCurrentStartTick(0)
    , _fanLowConsecutive(0)
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
        // Ambient temperature fallback chain: Sensor → Weather → ESP32 internal
        // Tries to "fail up" every cycle: INTERNAL→WEATHER when weather arrives,
        // any fallback→SENSOR when the real sensor recovers.
        TempSensor* ambient = getTempSensor("AMBIENT_TEMP");
        if (ambient != nullptr) {
            // Check if real OneWire sensor just read a valid value (set by update() above)
            bool sensorOk = ambient->isValid() && !_ambientFailoverTest;
            if (sensorOk && _ambientSource != AmbientSource::SENSOR) {
                // Sensor recovered — promote back to SENSOR
                _ambientSource = AmbientSource::SENSOR;
                Log.info("HP", "AMBIENT_TEMP sensor restored");
            } else if (!sensorOk) {
                // Sensor unavailable — use best available fallback
                bool weatherFresh = _weatherTempValid && (millis() - _weatherTempTick < _weatherStaleMs);
                if (weatherFresh) {
                    ambient->updateValue(_weatherTempF);
                    if (_ambientSource != AmbientSource::WEATHER) {
                        _ambientSource = AmbientSource::WEATHER;
                        Log.warn("HP", "AMBIENT_TEMP %s, using weather (%.1fF)",
                            _ambientFailoverTest ? "failover test" : "lost", _weatherTempF);
                    }
                } else {
                    float internalC = temperatureRead();
                    float internalF = internalC * 9.0f / 5.0f + 32.0f + _internalTempOffsetF;
                    ambient->updateValue(internalF);
                    if (_ambientSource != AmbientSource::INTERNAL) {
                        _ambientSource = AmbientSource::INTERNAL;
                        Log.warn("HP", "AMBIENT_TEMP %s, using ESP32 internal temp (%.1fF, offset %.1f)",
                            _ambientFailoverTest ? "failover test" : "lost", internalF, _internalTempOffsetF);
                    }
                }
            }
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

    // AUX/LT is a normally-closed circuit at the hardware level: closed (ON) by
    // default, opens (OFF) specifically when LOW_TEMP is reached. The generic
    // "all outputs off" loop above just forced it off along with everything
    // else — restore it to its normal ON state here so a fresh boot that never
    // enters LOW_TEMP doesn't leave it stuck off. _lowTemp is guaranteed false
    // at this point (10-minute validation hasn't run yet), so this is safe
    // regardless of actual ambient conditions; checkLowTempProtection() will
    // correctly turn it back off once/if LOW_TEMP validates.
    if (_lowTempEnableAux) {
        OutPin* aux = getOutput("AUX");
        if (aux != nullptr) {
            aux->turnOn();
            Log.info("HP", "AUX restored to normal ON state at startup (normally-closed circuit)");
        }
    }

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

// --- Current sensor management ---

void GoodmanHP::addCurrentSensor(const String& name, CurrentSensor* sensor) {
    _currentSensorMap[name] = sensor;
}

CurrentSensor* GoodmanHP::getCurrentSensor(const String& name) {
    auto it = _currentSensorMap.find(name);
    if (it != _currentSensorMap.end()) return it->second;
    return nullptr;
}

CurrentSensorMap& GoodmanHP::getCurrentSensorMap() {
    return _currentSensorMap;
}

// Starts a new non-blocking round of RMS acquisition across all current
// sensors. Only kicks off the first sensor's sampling — tickCurrentSensors()
// (called every loop pass) advances it and round-robins to the rest. If a
// prior round is still in progress (shouldn't normally happen at a 1s
// trigger interval given sampling takes ~470ms/sensor), this is a no-op so
// we don't abandon an in-flight acquisition.
void GoodmanHP::readCurrentSensors() {
    if (_ads1115 == nullptr || _currentSensorMap.empty()) return;
    if (_currentSamplingActive) return;

    _currentSampleIt = _currentSensorMap.begin();
    while (_currentSampleIt != _currentSensorMap.end() && _currentSampleIt->second == nullptr) {
        ++_currentSampleIt;
    }
    if (_currentSampleIt == _currentSensorMap.end()) return;

    _currentSamplingActive = true;
    _currentSampleIt->second->beginSample(_ads1115);
}

void GoodmanHP::tickCurrentSensors() {
    if (!_currentSamplingActive || _ads1115 == nullptr) return;

    CurrentSensor* sensor = _currentSampleIt->second;
    if (!sensor->tick(_ads1115)) return;  // still converting, nothing more to do this pass

    // This sensor's 60-sample cycle just completed.
    sensor->checkProtections();

    // Advance to the next sensor in the map, skipping any null entries.
    ++_currentSampleIt;
    while (_currentSampleIt != _currentSensorMap.end() && _currentSampleIt->second == nullptr) {
        ++_currentSampleIt;
    }

    if (_currentSampleIt == _currentSensorMap.end()) {
        _currentSamplingActive = false;  // round complete — wait for next readCurrentSensors() trigger
        return;
    }

    _currentSampleIt->second->beginSample(_ads1115);
}

bool GoodmanHP::isOvercurrentActive() const {
    for (const auto& pair : _currentSensorMap) {
        if (pair.second != nullptr && pair.second->isOvercurrent()) return true;
    }
    return false;
}

bool GoodmanHP::isLockedRotorActive() const {
    for (const auto& pair : _currentSensorMap) {
        if (pair.second != nullptr && pair.second->isLockedRotor()) return true;
    }
    return false;
}

void GoodmanHP::checkCurrentProtections() {
    if (_currentSensorMap.empty()) return;

    bool overcurrent = isOvercurrentActive();
    bool lockedRotor = isLockedRotorActive();

    if (!overcurrent && !lockedRotor) return;

    OutPin* cnt = getOutput("CNT");
    if (cnt != nullptr && cnt->isOn()) {
        cnt->turnOff();
        _cntActivated = false;
        if (lockedRotor) {
            Log.error("HP", "CNT shut down: locked rotor fault");
        } else {
            Log.error("HP", "CNT shut down: overcurrent protection");
        }
    }

    // Turn on W for auxiliary heat if in HEAT mode (same pattern as LPS fault)
    if (!isOActive() && isYActive()) {
        OutPin* w = getOutput("W");
        if (w != nullptr && !w->isOn()) {
            w->turnOn();
            Log.info("HP", "W turned ON (current fault, HEAT mode)");
        }
    }
}

// FAN fault watchdog: enforce that whenever CNT (compressor contactor) is
// energized, the FAN must actually be moving air. If the FAN current sensor
// reads below FAN_MIN_RUNNING_AMPS continuously for FAN_NO_CURRENT_TIMEOUT_MS
// while CNT is on, shut CNT down and latch an ERROR state for
// FAN_FAULT_ERROR_MS. After that grace period, the fault clears and normal
// activation resumes; if the FAN still fails to draw current within another
// FAN_NO_CURRENT_TIMEOUT_MS window on the next CNT activation, the fault is
// re-latched. This protects the compressor from overheating due to a
// stuck/failed blower even when the FAN output pin is commanded ON.
void GoodmanHP::checkFanFault() {
    OutPin* cnt = getOutput("CNT");
    OutPin* fan = getOutput("FAN");
    CurrentSensor* fanCur = getCurrentSensor("FAN_CURRENT");
    uint32_t now = millis();

    // === ERROR lockout window (3 min): keep CNT off, wait, then recover ===
    if (_fanFault) {
        // Force CNT off during the lockout (validateOutputStates enforces this too,
        // but do it here for immediate effect)
        if (cnt != nullptr && cnt->isOn()) {
            cnt->turnOff();
            _cntActivated = false;
            float ampsDbg = (fanCur && fanCur->isValid()) ? fanCur->getRMSAmps() : -1.0f;
            Log.error("HP", "FAN_FAULT: CNT forced OFF during ERROR lockout (Y=%d O=%d state=%s fanAmps=%.2f)",
                      isYActive() ? 1 : 0, isOActive() ? 1 : 0, getStateString(), ampsDbg);
        }
        uint32_t elapsed = now - _fanFaultStartTick;
        // Periodic heartbeat inside lockout so long ERROR windows are visible in logs
        {
            static uint32_t lastHeartbeat = 0;
            if (now - lastHeartbeat >= 30UL * 1000) {
                lastHeartbeat = now;
                uint32_t remainMs = (elapsed < FAN_FAULT_ERROR_MS) ? (FAN_FAULT_ERROR_MS - elapsed) : 0;
                float ampsDbg = (fanCur && fanCur->isValid()) ? fanCur->getRMSAmps() : -1.0f;
                Log.warn("HP", "FAN_FAULT: ERROR lockout active — %lus remaining (Y=%d fanAmps=%.2f fanPin=%d)",
                         remainMs / 1000UL, isYActive() ? 1 : 0, ampsDbg,
                         (fan != nullptr && fan->isOn()) ? 1 : 0);
            }
        }
        if (elapsed >= FAN_FAULT_ERROR_MS) {
            // Lockout expired — clear the latch. checkYAndActivateCNT will
            // re-energize CNT (and FAN) on the next cycle; the watchdog below
            // will then re-arm and re-check the FAN.
            State oldState = _state;
            _fanFault = false;
            _fanNoCurrentStartTick = 0;
            Log.warn("HP", "FAN_FAULT: ERROR lockout complete after %lums — clearing latch and attempting recovery (Y=%d O=%d state=%s)",
                     elapsed, isYActive() ? 1 : 0, isOActive() ? 1 : 0, getStateString());
            if (isYActive() && fan != nullptr && _state != State::DEFROST) {
                if (!fan->isOn()) {
                    fan->turnOn();
                    Log.info("HP", "FAN_FAULT: FAN turned ON (fault recovery, Y still active) — watchdog will re-arm on next CNT activation");
                }
            } else {
                Log.info("HP", "FAN_FAULT: recovery — no FAN action (Y=%d state=%s)",
                        isYActive() ? 1 : 0, getStateString());
            }
            if (_stateChangeCb) _stateChangeCb(_state, oldState);
        }
        return;
    }

    // === Normal operation: watch for CNT-ON + FAN-current-low ===
    if (cnt == nullptr || !cnt->isOn()) {
        // CNT off → nothing to guard, reset window
        _fanNoCurrentStartTick = 0;
        return;
    }
    if (fanCur == nullptr || !fanCur->isValid()) {
        // Without a valid FAN current reading we cannot judge; do not false-trip
        _fanNoCurrentStartTick = 0;
        return;
    }

    float amps = fanCur->getRMSAmps();
    if (amps >= FAN_MIN_RUNNING_AMPS) {
        // FAN is drawing current — clear the pending timer and consecutive counter
        _fanNoCurrentStartTick = 0;
        _fanLowConsecutive = 0;
        return;
    }

    // Low read — require N consecutive low reads before arming the watchdog window.
    // A single glitch (I2C contention / sample bunching near zero-crossing) is
    // filtered out here without producing any log line at all.
    if (_fanLowConsecutive < 0xFF) _fanLowConsecutive++;
    if (_fanLowConsecutive < FAN_LOW_CONSECUTIVE_REQUIRED) {
        return;
    }

    // FAN current is low while CNT is on — start / continue the 20s window
    if (_fanNoCurrentStartTick == 0) {
        _fanNoCurrentStartTick = now;
        // Nudge the FAN pin ON in case it was commanded OFF by a bug elsewhere
        if (fan != nullptr && !fan->isOn() && _state != State::DEFROST) {
            fan->turnOn();
            Log.warn("HP", "FAN_FAULT: FAN current low (%.2fA < %.2fA) while CNT on, FAN pin was OFF — commanded ON, watching %lus (Y=%d state=%s)",
                     amps, FAN_MIN_RUNNING_AMPS, FAN_NO_CURRENT_TIMEOUT_MS / 1000UL,
                     isYActive() ? 1 : 0, getStateString());
        } else {
            Log.warn("HP", "FAN_FAULT: FAN current low (%.2fA < %.2fA) while CNT on, FAN pin already ON — watching %lus (Y=%d state=%s)",
                     amps, FAN_MIN_RUNNING_AMPS, FAN_NO_CURRENT_TIMEOUT_MS / 1000UL,
                     isYActive() ? 1 : 0, getStateString());
        }
        return;
    }

    if (now - _fanNoCurrentStartTick < FAN_NO_CURRENT_TIMEOUT_MS) {
        // Log a mid-window progress line every 5s so the ramp toward the trip is visible
        static uint32_t lastProgress = 0;
        if (now - lastProgress >= 5000) {
            lastProgress = now;
            uint32_t inWindow = now - _fanNoCurrentStartTick;
            Log.warn("HP", "FAN_FAULT: still low current %.2fA at %lums/%lums of grace window (fanPin=%d)",
                     amps, inWindow, FAN_NO_CURRENT_TIMEOUT_MS,
                     (fan != nullptr && fan->isOn()) ? 1 : 0);
        }
        return;  // Still within grace window
    }

    // === Trip: FAN not running for > 20s while CNT is on ===
    uint32_t windowElapsed = now - _fanNoCurrentStartTick;
    Log.error("HP", "FAN_FAULT: TRIP — FAN current %.2fA < %.2fA for %lums (limit %lums) while CNT on (Y=%d O=%d state=%s fanPin=%d) — shutting down compressor",
              amps, FAN_MIN_RUNNING_AMPS, windowElapsed, FAN_NO_CURRENT_TIMEOUT_MS,
              isYActive() ? 1 : 0, isOActive() ? 1 : 0, getStateString(),
              (fan != nullptr && fan->isOn()) ? 1 : 0);

    if (cnt != nullptr && cnt->isOn()) {
        uint32_t cntOnMs = (cnt->getOnTick() > 0) ? (now - cnt->getOnTick()) : 0;
        cnt->turnOff();
        _cntActivated = false;
        Log.error("HP", "FAN_FAULT: CNT shut down after %lums of runtime (no airflow, fanAmps=%.2f)",
                  cntOnMs, amps);
    }
    // Keep the FAN output commanded ON so a recovering blower can re-establish
    // airflow and be detected on the next attempt.
    if (fan != nullptr && !fan->isOn() && _state != State::DEFROST) {
        fan->turnOn();
    }

    State oldState = _state;
    _fanFault = true;
    _fanFaultStartTick = now;
    _fanNoCurrentStartTick = 0;
    _fanLowConsecutive = 0;
    _state = State::ERROR;
    startStateValidation();
    const char* prevStateStr = "?";
    switch (oldState) {
        case State::OFF:      prevStateStr = "OFF"; break;
        case State::COOL:     prevStateStr = "COOL"; break;
        case State::HEAT:     prevStateStr = "HEAT"; break;
        case State::DEFROST:  prevStateStr = "DEFROST"; break;
        case State::ERROR:    prevStateStr = "ERROR"; break;
        case State::LOW_TEMP: prevStateStr = "LOW_TEMP"; break;
    }
    Log.error("HP", "FAN_FAULT: ENTER ERROR state for %lu min lockout (prev state=%s, will auto-recover at millis=%lu)",
              FAN_FAULT_ERROR_MS / 60000UL, prevStateStr,
              now + FAN_FAULT_ERROR_MS);
    if (_stateChangeCb) _stateChangeCb(State::ERROR, oldState);
}

bool GoodmanHP::isFanFaultActive() const {
    return _fanFault;
}

uint32_t GoodmanHP::getFanFaultRemainingMs() const {
    if (!_fanFault) return 0;
    uint32_t elapsed = millis() - _fanFaultStartTick;
    if (elapsed >= FAN_FAULT_ERROR_MS) return 0;
    return FAN_FAULT_ERROR_MS - elapsed;
}

void GoodmanHP::clearFanFault() {
    if (!_fanFault) return;
    uint32_t heldMs = millis() - _fanFaultStartTick;
    _fanFault = false;
    _fanFaultStartTick = 0;
    _fanNoCurrentStartTick = 0;
    _fanLowConsecutive = 0;
    Log.warn("HP", "FAN_FAULT: cleared manually after %lums (state=%s Y=%d)",
             heldMs, getStateString(), isYActive() ? 1 : 0);
    if (_stateChangeCb) _stateChangeCb(_state, _state);
}

void GoodmanHP::update() {
    // RV hold must run even during startup lockout — boot RV hold needs to
    // complete its 30s equalization within the 180s lockout period
    checkRvHold();

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
    checkCurrentProtections();
    checkAmbientTemp();
    checkYAndActivateCNT();
    checkFanFault();
    accumulateHeatRuntime();
    updateState();
    checkDefrostNeeded();
    checkCoolTransition();
    checkHeatTransition();
    checkRvHold();
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
        startStateValidation();
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
    } else if (_lpsFault) {
        // Continuously manage W during active LPS fault — updateState() is blocked
        // W follows Y in HEAT mode (Y active, O not active), never in COOL
        OutPin* w = getOutput("W");
        if (w != nullptr) {
            bool wShouldBeOn = isYActive() && !isOActive();
            if (wShouldBeOn && !w->isOn()) {
                w->turnOn();
                Log.info("HP", "W turned ON for ERROR state (Y activated)");
            } else if (!wShouldBeOn && w->isOn()) {
                w->turnOff();
                Log.info("HP", "W turned OFF for ERROR state (Y/O changed)");
            }
        }
    }
}

void GoodmanHP::checkAmbientTemp() {
    // Don't override higher-priority faults
    if (_compressorOverTemp || _lpsFault) return;

    TempSensor* ambient = getTempSensor("AMBIENT_TEMP");
    if (ambient == nullptr || !ambient->isValid()) return;

    float temp = ambient->getValue();
    uint32_t now = millis();

    if (!_lowTemp) {
        // --- NOT in LOW_TEMP: check for entry ---
        if (temp < _lowTempThreshold) {
            if (!_lowTempPendingEntry) {
                // Start 10-minute entry validation timer
                _lowTempPendingEntry = true;
                _lowTempPendingTick = now;
                Log.info("HP", "Ambient %.1fF < %.1fF, LOW_TEMP pending entry (10 min validation)",
                         temp, _lowTempThreshold);
            } else if (now - _lowTempPendingTick >= LOW_TEMP_VALIDATION_MS) {
                // 10 minutes validated — enter LOW_TEMP
                _lowTempPendingEntry = false;
                _lowTemp = true;
                Log.warn("HP", "Low ambient temp %.1fF validated for 10 min, LOW_TEMP protection active",
                         temp);

                // Immediate output protection
                OutPin* cnt = getOutput("CNT");
                if (cnt != nullptr && cnt->isOn()) {
                    cnt->turnOff();
                    _cntActivated = false;
                    Log.warn("HP", "CNT shut down due to low ambient temp");
                }
                OutPin* fan = getOutput("FAN");
                if (fan != nullptr) fan->turnOff();
                safeRvOff();

                // W requires all 3 gates: LOW_TEMP + checkbox + Y active (O acts like checkbox off)
                if (_lowTempEnableW && isYActive() && !isOActive()) {
                    OutPin* w = getOutput("W");
                    if (w != nullptr) {
                        w->turnOn();
                        Log.info("HP", "W turned ON for LOW_TEMP mode (Y active)");
                    }
                }

                // AUX/LT must be OFF while ambient is below the low-temp threshold
                if (_lowTempEnableAux) {
                    OutPin* aux = getOutput("AUX");
                    if (aux != nullptr) {
                        aux->turnOff();
                        Log.info("HP", "AUX turned OFF for LOW_TEMP protection");
                    }
                }

                // Gate state label change by validation timer
                if (canTransitionToNormalState()) {
                    State oldState = _state;
                    _state = State::LOW_TEMP;
                    startStateValidation();
                    Log.info("HP", "State: %s -> LOW_TEMP (validation: %lus)",
                             oldState == State::OFF ? "OFF" : oldState == State::HEAT ? "HEAT" :
                             oldState == State::COOL ? "COOL" : "OTHER", _stateValidationMs / 1000UL);
                    if (_stateChangeCb) _stateChangeCb(State::LOW_TEMP, oldState);
                }
            }
            // Cancel any pending exit (shouldn't happen, but defensive)
            _lowTempPendingExit = false;
        } else {
            // Temp back above threshold — cancel pending entry
            if (_lowTempPendingEntry) {
                _lowTempPendingEntry = false;
                Log.info("HP", "Ambient %.1fF >= %.1fF, LOW_TEMP entry cancelled", temp, _lowTempThreshold);
            }
        }
    } else {
        // --- IN LOW_TEMP: manage outputs and check for exit ---
        if (temp >= _lowTempThreshold) {
            if (!_lowTempPendingExit) {
                // Start 10-minute exit validation timer
                _lowTempPendingExit = true;
                _lowTempPendingTick = now;
                Log.info("HP", "Ambient %.1fF >= %.1fF, LOW_TEMP pending exit (10 min validation)",
                         temp, _lowTempThreshold);
            } else if (now - _lowTempPendingTick >= LOW_TEMP_VALIDATION_MS) {
                // 10 minutes validated — exit LOW_TEMP
                _lowTempPendingExit = false;
                _lowTemp = false;
                Log.info("HP", "Ambient temp %.1fF validated above %.1fF for 10 min, exiting LOW_TEMP",
                         temp, _lowTempThreshold);

                // Turn off W
                OutPin* w = getOutput("W");
                if (w != nullptr) w->turnOff();

                // AUX/LT resumes normal (ON) once ambient is back above threshold
                OutPin* aux = getOutput("AUX");
                if (aux != nullptr) {
                    aux->turnOn();
                    Log.info("HP", "AUX turned ON, LOW_TEMP protection cleared");
                }

                // Don't set _state here — let updateState() determine the correct state
            }
            // Cancel any pending entry (shouldn't happen, but defensive)
            _lowTempPendingEntry = false;
        } else {
            // Temp dropped back below threshold — cancel pending exit
            if (_lowTempPendingExit) {
                _lowTempPendingExit = false;
                Log.info("HP", "Ambient %.1fF < %.1fF, LOW_TEMP exit cancelled", temp, _lowTempThreshold);
            }
        }

        // Re-check deferred state label if outputs are protected but state not yet LOW_TEMP
        if (_state != State::LOW_TEMP && canTransitionToNormalState()) {
            State oldState = _state;
            _state = State::LOW_TEMP;
            startStateValidation();
            Log.info("HP", "State: %s -> LOW_TEMP (deferred, validation: %lus)",
                     oldState == State::OFF ? "OFF" : oldState == State::HEAT ? "HEAT" :
                     oldState == State::COOL ? "COOL" : "OTHER", _stateValidationMs / 1000UL);
            if (_stateChangeCb) _stateChangeCb(State::LOW_TEMP, oldState);
        }

        // Continuously manage W based on 3 gates:
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
    OutPin* cnt = getOutput("CNT");

    if (cnt == nullptr) {
        return;
    }

    bool yActive = isYActive();

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
            // W off, RV held for pressure equalization — _softwareDefrost stays set
            // so defrost resumes on next Y activation in HEAT mode
            OutPin* w = getOutput("W");
            if (w != nullptr) w->turnOff();
            safeRvOff();
            _defrostTransition = false;
            _defrostCntPending = false;
            _defrostStartTick = 0;
            Log.info("HP", "Y dropped during defrost, system shutdown (defrost pending)");
        }
        if (_defrostExiting) {
            // Cancel exit transition — all outputs already off from Y-drop above
            OutPin* w = getOutput("W");
            if (w != nullptr) w->turnOff();
            safeRvOff();
            _defrostExiting = false;
            _defrostTransition = false;
            _defrostCntPending = false;
            Log.info("HP", "Y dropped during defrost exit, exit cancelled");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        if (_lpsFault || _lowTemp || _compressorOverTemp || _suctionLowTemp || _rvFail || _fanFault || isOvercurrentActive() || isLockedRotorActive() || _softwareDefrost || _defrostExiting || _coolTransition || _coolCntPending || _heatTransition || _heatCntPending || _rvHoldActive) return;
        // Check if CNT was off for less than 5 minutes - if so, enforce short cycle delay
        uint32_t offElapsed = millis() - cnt->getOffTick();
        bool activated = false;
        if (cnt->getOffTick() > 0 && offElapsed < 5UL * 60 * 1000) {
            // Y still active, CNT off < 5 min - check if short cycle delay has passed
            uint32_t elapsed = millis() - _yActiveStartTick;
            if (elapsed >= _cntShortCycleMs) {
                cnt->turnOn();
                _cntActivated = true;
                activated = true;
                CurrentSensor* compCurrent = getCurrentSensor("COMPRESSOR_CURRENT");
                if (compCurrent) compCurrent->notifyCntActivated();
                Log.info("HP", "Y active for 30s, CNT activated (short cycle protection)");
            }
        } else {
            // CNT off >= 5 min or never turned off - activate immediately
            cnt->turnOn();
            _cntActivated = true;
            activated = true;
            CurrentSensor* compCurrent = getCurrentSensor("COMPRESSOR_CURRENT");
            if (compCurrent) compCurrent->notifyCntActivated();
            Log.info("HP", "Y active, CNT activated immediately (off > 5 min)");
        }
        // Safety invariant: whenever we energize CNT, the FAN must also be running.
        // Historically only the Y-edge branch turned the FAN on, so recovery paths
        // (e.g. HEAT→COOL flip-flop) could leave CNT ON with FAN OFF.
        if (activated && fan != nullptr && _state != State::DEFROST && !fan->isOn()) {
            fan->turnOn();
            Log.warn("HP", "FAN_SAFETY: FAN turned ON alongside CNT activation (invariant: CNT_on ⇒ FAN_on) state=%s Y=%d O=%d",
                     getStateString(), isYActive() ? 1 : 0, isOActive() ? 1 : 0);
        }
        // Restart the FAN no-current watchdog window on CNT activation
        if (activated) {
            _fanNoCurrentStartTick = 0;
            _fanLowConsecutive = 0;
        }
    }
}

void GoodmanHP::updateState() {
    // Don't compute new state while faulted or low temp
    if (_lpsFault || _lowTemp) return;

    InputPin* dft = getInput("DFT");

    // Use helper methods (respects CAN mode)
    bool yActive = isYActive();
    bool oActive = isOActive();

    // DFT still requires physical pin (local safety device)
    if (dft == nullptr) {
        return;
    }

    State newState = State::OFF;

    if (_softwareDefrost && yActive && !oActive) {
        newState = State::DEFROST;
    } else if (_softwareDefrost && yActive && oActive) {
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
    } else if (yActive && oActive) {
        newState = State::COOL;
    } else if (yActive) {
        // If _softwareDefrost is set, re-enter DEFROST is handled above
        newState = State::HEAT;
    }

    // High ambient HEAT lockout — never run HEAT above threshold (default 70°F).
    // Uses ambient value from AMBIENT_TEMP sensor (with fallback chain applied by _tskCheckTemps).
    {
        TempSensor* amb = getTempSensor("AMBIENT_TEMP");
        bool lockout = false;
        if (amb != nullptr && amb->isValid() && amb->getValue() > _highAmbientHeatLockoutF) {
            lockout = true;
        }
        if (lockout && (newState == State::HEAT || newState == State::DEFROST)) {
            if (!_highAmbientHeatLockout) {
                Log.warn("HP", "HEAT blocked: ambient %.1fF > %.1fF lockout",
                         amb->getValue(), _highAmbientHeatLockoutF);
            }
            newState = State::OFF;
        }
        if (_highAmbientHeatLockout != lockout) {
            if (!lockout) {
                Log.info("HP", "High-ambient HEAT lockout cleared (ambient %.1fF <= %.1fF)",
                         amb != nullptr ? amb->getValue() : 0.0f, _highAmbientHeatLockoutF);
            }
            _highAmbientHeatLockout = lockout;
        }
    }

    if (newState != _state) {
        // Gate normal-priority transitions by validation timer
        if (newState != State::OFF && newState != State::ERROR) {
            if (!canTransitionToNormalState()) {
                return;  // Block HEAT/COOL/DEFROST until validation completes
            }
        }

        State oldState = _state;
        Log.info("HP", "State: %s -> %s (validation: %lus)",
                 getStateString(),
                 newState == State::OFF ? "OFF" :
                 newState == State::COOL ? "COOL" :
                 newState == State::HEAT ? "HEAT" :
                 newState == State::DEFROST ? "DEFROST" : "ERROR",
                 _stateValidationMs / 1000UL);
        _state = newState;
        startStateValidation();

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
                if (newState == State::HEAT && oldState == State::COOL && rv->isOn()) {
                    // COOL→HEAT: sequenced transition like defrost
                    OutPin* cnt = getOutput("CNT");
                    OutPin* fan = getOutput("FAN");
                    if (cnt != nullptr && cnt->isOn()) { cnt->turnOff(); _cntActivated = false; }
                    if (fan != nullptr) fan->turnOff();
                    _heatTransition = true;
                    _heatTransitionStart = millis();
                    _heatCntPending = false;
                    Log.info("HP", "Starting HEAT transition (%lu s RV short cycle)", _rvShortCycleMs / 1000UL);
                } else if (newState == State::HEAT || newState == State::OFF) {
                    safeRvOff();
                }
            }
        }

        // Resume defrost from Phase 1 when Y returns in HEAT mode
        // MUST run before W control so _defrostTransition is set before W is evaluated
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

        // Control FAN: OFF during DEFROST and COOL transition, restore when leaving DEFROST if Y active
        OutPin* fan = getOutput("FAN");
        if (fan != nullptr) {
            if (newState == State::DEFROST) {
                fan->turnOff();
                Log.info("HP", "FAN turned OFF for DEFROST mode");
            } else if (oldState == State::DEFROST && yActive && !_defrostExiting && !_coolTransition) {
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
    if (_canMode) {
        // Timeout safety: if CAN messages stop, shut down
        if (millis() - _canLastRxTick > CAN_TIMEOUT_MS) return false;
        return _canYActive;
    }
    InputPin* y = getInput("Y");
    return y != nullptr && y->isActive();
}

bool GoodmanHP::isOActive() {
    if (_canMode) {
        if (millis() - _canLastRxTick > CAN_TIMEOUT_MS) return false;
        return _canOActive;
    }
    InputPin* o = getInput("O");
    return o != nullptr && o->isActive();
}

void GoodmanHP::setCANMode(bool enabled) {
    _canMode = enabled;
    Log.info("HP", "CAN mode %s", enabled ? "enabled" : "disabled");
}

bool GoodmanHP::isCANMode() const {
    return _canMode;
}

void GoodmanHP::setCANInputState(bool yActive, bool oActive) {
    _canYActive = yActive;
    _canOActive = oActive;
    _canLastRxTick = millis();
}

uint32_t GoodmanHP::getCANLastRxTick() const {
    return _canLastRxTick;
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

uint32_t GoodmanHP::getDefrostElapsedMs() const {
    if (!_softwareDefrost || _defrostStartTick == 0 || _state != State::DEFROST) return 0;
    return millis() - _defrostStartTick;
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

bool GoodmanHP::isHeatTransitionActive() const {
    return _heatTransition;
}

bool GoodmanHP::isHeatCntPendingActive() const {
    return _heatCntPending;
}

uint32_t GoodmanHP::getHeatTransitionRemainingMs() const {
    if (!_heatTransition) return 0;
    uint32_t elapsed = millis() - _heatTransitionStart;
    if (elapsed >= _rvShortCycleMs) return 0;
    return _rvShortCycleMs - elapsed;
}

uint32_t GoodmanHP::getHeatCntPendingRemainingMs() const {
    if (!_heatCntPending) return 0;
    uint32_t elapsed = millis() - _heatCntPendingStart;
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
        OutPin* cnt = getOutput("CNT");
        OutPin* fan = getOutput("FAN");
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (fan != nullptr) fan->turnOff();
        safeRvOff();
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

void GoodmanHP::checkHeatTransition() {
    if (!_heatTransition && !_heatCntPending) return;

    // Abort if state changed away from HEAT (fault or LOW_TEMP took over)
    if (_state != State::HEAT) {
        _heatTransition = false;
        _heatCntPending = false;
        Log.info("HP", "HEAT transition cancelled (state: %s)", getStateString());
        // Safety: FAN was forced OFF when the HEAT transition started, but if Y
        // is still active (e.g. spurious O flip flipped us back to COOL) we
        // must restore FAN so the compressor doesn't run without airflow when
        // short-cycle protection re-activates CNT.
        if (isYActive() && _state != State::DEFROST && _state != State::OFF &&
            _state != State::LOW_TEMP) {
            OutPin* fan = getOutput("FAN");
            if (fan != nullptr && !fan->isOn()) {
                fan->turnOn();
                Log.warn("HP", "FAN_SAFETY: FAN restored ON after HEAT transition cancelled while Y still active (prevents CNT-on/FAN-off overheat) state=%s O=%d",
                         getStateString(), isOActive() ? 1 : 0);
            }
        }
        return;
    }

    // Abort if Y dropped
    if (!isYActive()) {
        OutPin* cnt = getOutput("CNT");
        OutPin* fan = getOutput("FAN");
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (fan != nullptr) fan->turnOff();
        safeRvOff();
        _heatTransition = false;
        _heatCntPending = false;
        Log.info("HP", "HEAT transition cancelled (Y dropped)");
        return;
    }

    uint32_t now = millis();

    // Phase 1: Pressure equalization (CNT+FAN off, RV still on from COOL, wait RV SC)
    if (_heatTransition) {
        if (now - _heatTransitionStart >= _rvShortCycleMs) {
            _heatTransition = false;
            Log.info("HP", "HEAT Phase 1 complete, RV off, waiting %lu s CNT short cycle",
                     _cntShortCycleMs / 1000UL);
            OutPin* rv = getOutput("RV");
            if (rv != nullptr) rv->turnOff();
            _heatCntPending = true;
            _heatCntPendingStart = now;
        }
        return;
    }

    // Phase 2: RV off, waiting CNT short cycle
    if (_heatCntPending) {
        if (now - _heatCntPendingStart >= _cntShortCycleMs) {
            _heatCntPending = false;
            Log.info("HP", "HEAT Phase 2 complete, CNT+FAN on — HEAT mode active");
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

void GoodmanHP::safeRvOff() {
    OutPin* rv = getOutput("RV");
    if (rv == nullptr || !rv->isOn()) return;

    OutPin* cnt = getOutput("CNT");
    if (cnt == nullptr) {
        rv->turnOff();
        return;
    }

    // If CNT has been off for at least rvShortCycleMs, pressure is equalized
    uint32_t cntOffElapsed = millis() - cnt->getOffTick();
    if (!cnt->isOn() && cntOffElapsed >= _rvShortCycleMs) {
        rv->turnOff();
        Log.info("HP", "RV OFF (pressure equalized, CNT off %lus)", cntOffElapsed / 1000UL);
    } else {
        // CNT recently active — hold RV on until pressure equalizes
        _rvHoldActive = true;
        uint32_t remaining = cnt->isOn() ? _rvShortCycleMs :
                             (_rvShortCycleMs - cntOffElapsed);
        Log.info("HP", "RV hold: waiting %lus for pressure equalization", remaining / 1000UL);
    }
}

void GoodmanHP::checkRvHold() {
    if (!_rvHoldActive) return;

    OutPin* rv = getOutput("RV");
    if (rv == nullptr || !rv->isOn()) {
        // RV already off or doesn't exist
        _rvHoldActive = false;
        return;
    }

    // Cancel hold if a transition took over RV management
    if (_coolTransition || _coolCntPending || _heatTransition || _heatCntPending ||
        _defrostTransition || _defrostCntPending || _defrostExiting) {
        _rvHoldActive = false;
        return;
    }

    OutPin* cnt = getOutput("CNT");
    if (cnt != nullptr && cnt->isOn()) {
        // CNT turned back on — normal operation resumed
        _rvHoldActive = false;
        return;
    }

    if (cnt == nullptr) {
        rv->turnOff();
        _rvHoldActive = false;
        return;
    }

    uint32_t cntOffElapsed = millis() - cnt->getOffTick();
    if (cntOffElapsed >= _rvShortCycleMs) {
        rv->turnOff();
        _rvHoldActive = false;
        Log.info("HP", "RV hold complete, RV OFF (CNT off %lus)", cntOffElapsed / 1000UL);
    }
}

bool GoodmanHP::isRvHoldActive() const {
    return _rvHoldActive;
}

bool GoodmanHP::isAmbientFallbackActive() const {
    return _ambientSource != AmbientSource::SENSOR;
}

GoodmanHP::AmbientSource GoodmanHP::getAmbientSource() const {
    return _ambientSource;
}

void GoodmanHP::setWeatherTemp(float tempF) {
    _weatherTempF = tempF;
    _weatherTempValid = true;
    _weatherTempTick = millis();
}

void GoodmanHP::setWeatherStaleMs(uint32_t ms) {
    _weatherStaleMs = ms;
}

void GoodmanHP::setInternalTempOffsetF(float offset) {
    _internalTempOffsetF = offset;
}

float GoodmanHP::getInternalTempOffsetF() const {
    return _internalTempOffsetF;
}

void GoodmanHP::setAmbientFailoverTest(bool on) {
    _ambientFailoverTest = on;
    if (!on) {
        _failoverTestStartTick = 0;
        Log.info("HP", "Ambient failover test ended");
    } else {
        _failoverTestStartTick = millis();
        Log.info("HP", "Ambient failover test started (30 min)");
    }
}

bool GoodmanHP::isAmbientFailoverTestActive() const {
    return _ambientFailoverTest;
}

uint32_t GoodmanHP::getFailoverTestRemainingSec() const {
    if (!_ambientFailoverTest || _failoverTestStartTick == 0) return 0;
    uint32_t elapsed = millis() - _failoverTestStartTick;
    uint32_t durationMs = 30UL * 60 * 1000;
    if (elapsed >= durationMs) return 0;
    return (durationMs - elapsed) / 1000;
}

float GoodmanHP::getWeatherTempF() const {
    return _weatherTempF;
}

bool GoodmanHP::isWeatherTempValid() const {
    return _weatherTempValid;
}

uint32_t GoodmanHP::getWeatherTempAgeSec() const {
    if (!_weatherTempValid) return 0;
    return (millis() - _weatherTempTick) / 1000;
}

uint32_t GoodmanHP::getRvHoldRemainingMs() const {
    if (!_rvHoldActive) return 0;
    auto it = _outputMap.find("CNT");
    if (it == _outputMap.end() || it->second == nullptr) return 0;
    uint32_t cntOffElapsed = millis() - it->second->getOffTick();
    if (cntOffElapsed >= _rvShortCycleMs) return 0;
    return _rvShortCycleMs - cntOffElapsed;
}

void GoodmanHP::handleBootRvHold(bool rvWasOn, bool cntWasOn) {
    if (!rvWasOn || !cntWasOn) {
        Log.info("HP", "Boot GPIO: CNT=%s RV=%s — no RV hold needed",
                 cntWasOn ? "ON" : "OFF", rvWasOn ? "ON" : "OFF");
        return;
    }

    // Both CNT and RV were energized before reboot — re-energize RV
    // for pressure equalization before allowing it to turn off
    OutPin* rv = getOutput("RV");
    if (rv == nullptr) return;

    rv->turnOn();
    _rvHoldActive = true;
    Log.warn("HP", "Boot: CNT+RV were on before reboot, holding RV for %lus pressure equalization",
             _rvShortCycleMs / 1000UL);
}


void GoodmanHP::validateOutputStates() {
    uint32_t now = millis();
    if (now - _lastValidateTick < STATE_VALIDATE_MS) return;
    _lastValidateTick = now;

    // Skip during startup lockout, manual override, and state validation hold
    // (state label doesn't reflect actual operating mode during validation window)
    if (_startupLockout || _manualOverride || _stateValidationActive) return;

    // Skip if LOW_TEMP outputs are protected but state label is deferred
    if (_lowTemp && _state != State::LOW_TEMP) return;

    // Log state validation progress during hold (debug-level)
    if (_stateValidationActive) {
        OutPin* cntL = getOutput("CNT"); OutPin* fanL = getOutput("FAN");
        OutPin* wL = getOutput("W"); OutPin* rvL = getOutput("RV");
        Log.debug("HP", "State validation active: %s [FAN=%s CNT=%s RV=%s W=%s] [Y=%d O=%d LPS=%d DFT=%d]",
                 getStateString(),
                 fanL && fanL->isPinOn() ? "ON" : "OFF",
                 cntL && cntL->isPinOn() ? "ON" : "OFF",
                 rvL && rvL->isPinOn() ? "ON" : "OFF",
                 wL && wL->isPinOn() ? "ON" : "OFF",
                 isYActive(), isOActive(), isLPSActive(), isDFTActive());
    }

    OutPin* cnt = getOutput("CNT");
    OutPin* fan = getOutput("FAN");
    OutPin* w   = getOutput("W");
    OutPin* rv  = getOutput("RV");
    bool corrected = false;

    // === GLOBAL INVARIANTS (always enforced) ===

    // Safety: if no ambient sensor data available at all, enforce LOW_TEMP outputs
    TempSensor* ambSensor = getTempSensor("AMBIENT_TEMP");
    if (ambSensor == nullptr || !ambSensor->isValid()) {
        if (cnt != nullptr && cnt->isOn() && _state != State::DEFROST) {
            Log.error("HP", "STATE CHECK: CNT on with no ambient data — forcing OFF for safety");
            cnt->turnOff(); _cntActivated = false; corrected = true;
        }
    }

    // W must NEVER be on when O is active
    if (w != nullptr && w->isOn() && isOActive()) {
        Log.error("HP", "STATE CHECK: W on while O active — forcing OFF");
        w->turnOff();
        corrected = true;
    }

    // === FAULT PRIORITY TABLE: CNT must be OFF during any blocking fault ===
    // Priority: 1=compressorOverTemp, 2=suctionLowTemp(COOL), 3=lpsFault, 4=lowTemp, latched=rvFail
    if (cnt != nullptr && cnt->isOn()) {
        const char* faultName = nullptr;
        if (_compressorOverTemp)          faultName = "compressor overtemp";
        else if (_suctionLowTemp)         faultName = "suction low temp";
        else if (_lpsFault)               faultName = "LPS fault";
        else if (_fanFault)               faultName = "FAN fault";
        else if (_lowTemp)                faultName = "LOW_TEMP";
        else if (_rvFail && !_softwareDefrost) faultName = "RV fail";

        if (faultName) {
            Log.error("HP", "STATE CHECK: CNT on during %s — forcing OFF", faultName);
            cnt->turnOff(); _cntActivated = false; corrected = true;
        }
    }

    // Airflow safety invariant: whenever CNT is energized the FAN must be on
    // (except during DEFROST, where the outdoor fan is intentionally off).
    if (cnt != nullptr && cnt->isPinOn() && fan != nullptr && !fan->isPinOn() &&
        _state != State::DEFROST) {
        CurrentSensor* fanCurChk = getCurrentSensor("FAN_CURRENT");
        float ampsChk = (fanCurChk && fanCurChk->isValid()) ? fanCurChk->getRMSAmps() : -1.0f;
        Log.error("HP", "FAN_SAFETY: STATE CHECK detected CNT_pin=ON with FAN_pin=OFF — forcing FAN ON (state=%s Y=%d O=%d fanAmps=%.2f)",
                  getStateString(), isYActive() ? 1 : 0, isOActive() ? 1 : 0, ampsChk);
        fan->turnOn();
        corrected = true;
    }

    // === TABLE-DRIVEN PER-STATE VALIDATION ===
    // Expected outputs: -1=don't care, 0=OFF, 1=ON
    int8_t expFAN = -1, expCNT = -1, expRV = -1, expW = -1;
    const char* row = "UNKNOWN";

    // === TRANSITION PHASE ROWS (checked first, override per-state) ===
    if (_coolTransition) {
        // HEAT→COOL Phase 1: CNT+FAN off, RV still off (from HEAT), pressure equalization
        row = "COOL Trans Ph1"; expFAN = 0; expCNT = 0; expRV = 0; expW = 0;
    } else if (_coolCntPending) {
        // HEAT→COOL Phase 2: RV on, CNT off (waiting short cycle)
        row = "COOL Trans Ph2"; expFAN = 0; expCNT = 0; expRV = 1; expW = 0;
    } else if (_heatTransition) {
        // COOL→HEAT Phase 1: CNT+FAN off, RV still on (from COOL), pressure equalization
        row = "HEAT Trans Ph1"; expFAN = 0; expCNT = 0; expRV = 1; expW = 0;
    } else if (_heatCntPending) {
        // COOL→HEAT Phase 2: RV off, CNT off (waiting short cycle)
        row = "HEAT Trans Ph2"; expFAN = 0; expCNT = 0; expRV = 0; expW = 0;
    } else if (_defrostTransition && !_defrostExiting) {
        // Defrost entry Phase 1: all off, pressure equalization
        row = "DEFROST Ph1"; expFAN = 0; expCNT = 0; expRV = 0; expW = 0;
    } else if (_defrostCntPending && !_defrostExiting) {
        // Defrost entry Phase 2: RV+W on, CNT off (waiting short cycle)
        row = "DEFROST Ph2"; expFAN = 0; expCNT = 0; expRV = 1; expW = 1;
    } else if (_defrostTransition && _defrostExiting) {
        // Defrost exit Phase 1: CNT+FAN+W off, RV stays on
        row = "DFX Exit Ph1"; expFAN = 0; expCNT = 0; expRV = 1; expW = 0;
    } else if (_defrostCntPending && _defrostExiting) {
        // Defrost exit Phase 2: RV off (W already off from exit start), CNT off
        row = "DFX Exit Ph2"; expFAN = 0; expCNT = 0; expRV = 0; expW = 0;
    } else {
        // === STEADY-STATE ROWS ===
        switch (_state) {
            case State::OFF:
                row = "OFF"; expFAN = 0; expCNT = 0; expRV = 0; expW = 0;
                break;

            case State::COOL:
                row = "COOL"; expRV = 1; expW = 0;
                if (cnt != nullptr && cnt->isOn()) expFAN = 1;
                break;

            case State::HEAT:
                row = _rvFail ? "HEAT+rvFail" : "HEAT";
                expRV = 0;
                expW = _rvFail && isYActive() && !isOActive() ? 1 : 0;
                if (_rvFail) { expCNT = 0; expFAN = 1; }
                if (cnt != nullptr && cnt->isOn()) expFAN = 1;
                break;

            case State::DEFROST:
                if (_softwareDefrost) {
                    row = "DEFROST Ph3"; expFAN = 0; expCNT = 1; expRV = 1; expW = 1;
                }
                break;

            case State::ERROR:
                row = "ERROR"; expCNT = 0;
                expW = (isYActive() && !isOActive()) ? 1 : 0;
                break;

            case State::LOW_TEMP:
                row = "LOW_TEMP"; expFAN = 0; expCNT = 0; expRV = 0;
                if (!(_lowTempEnableW && isYActive() && !isOActive())) expW = 0;
                break;
        }
    }

    // RV hold: don't enforce RV off while waiting for pressure equalization
    if (_rvHoldActive) expRV = -1;

    // Check and auto-correct each output against expected table value
    struct { OutPin* pin; int8_t expected; const char* name; } checks[] = {
        {fan, expFAN, "FAN"}, {cnt, expCNT, "CNT"}, {rv, expRV, "RV"}, {w, expW, "W"}
    };

    for (auto& c : checks) {
        if (c.pin == nullptr || c.expected == -1) continue;
        bool isOn = c.pin->isPinOn();
        bool shouldBeOn = (c.expected == 1);
        if (isOn != shouldBeOn) {
            Log.error("HP", "STATE CHECK: %s %s in %s — forcing %s",
                      c.name, isOn ? "on" : "off", row, shouldBeOn ? "ON" : "OFF");
            if (shouldBeOn) c.pin->turnOn(); else c.pin->turnOff();
            if (strcmp(c.name, "CNT") == 0 && !shouldBeOn) _cntActivated = false;
            corrected = true;
        }
    }

    if (corrected) {
        if (_stateChangeCb) _stateChangeCb(_state, _state);
        // Reset throttle so next clean pass logs sooner
        _lastValidationLogTick = now;
    } else if (now - _lastValidationLogTick >= 30000UL) {
        // Throttled info every 30s when outputs are correct
        _lastValidationLogTick = now;
        Log.info("HP", "State check OK: %s [FAN=%s CNT=%s RV=%s W=%s]",
                 getStateString(),
                 fan && fan->isPinOn() ? "ON" : "OFF",
                 cnt && cnt->isPinOn() ? "ON" : "OFF",
                 rv && rv->isPinOn() ? "ON" : "OFF",
                 w && w->isPinOn() ? "ON" : "OFF");
    }
}

void GoodmanHP::setColdMaxTempF(float f) {
    // Defrost's Cold band must never reach below the LOW_TEMP safety threshold —
    // below that, the compressor is blocked entirely and defrost timing is moot.
    if (f < _lowTempThreshold) {
        Log.warn("HP", "Defrost cold max temp %.1fF below low temp threshold %.1fF, clamping to %.1fF",
                 f, _lowTempThreshold, _lowTempThreshold);
        f = _lowTempThreshold;
    }
    // Cold's ceiling must stay below Warm's floor — otherwise the Cold/Warm
    // band ordering inverts and the Mid band range becomes nonsensical.
    if (f >= _warmMinTempF) {
        float clamped = _warmMinTempF - 1.0f;
        Log.warn("HP", "Defrost cold max temp %.1fF at/above warm min temp %.1fF, clamping to %.1fF",
                 f, _warmMinTempF, clamped);
        f = clamped;
    }
    _coldMaxTempF = f;
    Log.info("HP", "Defrost cold max temp set to %.1fF", f);
}

float GoodmanHP::getColdMaxTempF() const {
    return _coldMaxTempF;
}

void GoodmanHP::setWarmMinTempF(float f) {
    // Keep the invariant symmetric: if lowering Warm's floor would drop it
    // to/below the current Cold ceiling, raise it to maintain the gap.
    if (f <= _coldMaxTempF) {
        float clamped = _coldMaxTempF + 1.0f;
        Log.warn("HP", "Defrost warm min temp %.1fF at/below cold max temp %.1fF, raising to %.1fF",
                 f, _coldMaxTempF, clamped);
        f = clamped;
    }
    _warmMinTempF = f;
    Log.info("HP", "Defrost warm min temp set to %.1fF", f);
}

float GoodmanHP::getWarmMinTempF() const {
    return _warmMinTempF;
}

void GoodmanHP::setDefrostBand(DefrostBandName band, const DefrostBand& params) {
    int idx = (int)band;
    _defrostBands[idx] = params;
    static const char* names[] = {"COLD", "MID", "WARM"};
    Log.info("HP", "Defrost band %s: runtime=%lu min, minDefrost=%lu s, exit=%.1fF",
             names[idx], params.runtimeThresholdMs / 60000UL,
             params.minRuntimeMs / 1000UL, params.exitTempF);
}

GoodmanHP::DefrostBand GoodmanHP::getDefrostBand(DefrostBandName band) const {
    return _defrostBands[(int)band];
}

GoodmanHP::DefrostBandName GoodmanHP::getActiveDefrostBand() const {
    return _activeDefrostBand;
}

const char* GoodmanHP::getActiveDefrostBandString() const {
    static const char* names[] = {"Cold", "Mid", "Warm"};
    return names[(int)_activeDefrostBand];
}

uint32_t GoodmanHP::getActiveRuntimeThresholdMs() const {
    return _defrostBands[(int)_activeDefrostBand].runtimeThresholdMs;
}

uint32_t GoodmanHP::getActiveMinRuntimeMs() const {
    return _defrostSnapshotBand.minRuntimeMs;
}

float GoodmanHP::getActiveExitTempF() const {
    return _defrostSnapshotBand.exitTempF;
}

GoodmanHP::DefrostBandName GoodmanHP::selectDefrostBand() {
    TempSensor* ambient = getTempSensor("AMBIENT_TEMP");
    if (ambient == nullptr || !ambient->isValid()) {
        return DefrostBandName::WARM;  // No ambient data = most conservative
    }
    float temp = ambient->getValue();
    if (temp <= _coldMaxTempF) return DefrostBandName::COLD;
    if (temp >= _warmMinTempF) return DefrostBandName::WARM;
    return DefrostBandName::MID;
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

        // Stop defrost — RV held for pressure equalization
        safeRvOff();
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
    // Keep the invariant symmetric: if raising the floor pushes it above the
    // current defrost Cold band ceiling, bring the ceiling up to match.
    if (_coldMaxTempF < _lowTempThreshold) {
        Log.warn("HP", "Low temp threshold %.1fF raised above defrost cold max temp %.1fF, raising cold max temp to match",
                 threshold, _coldMaxTempF);
        _coldMaxTempF = _lowTempThreshold;
    }
    Log.info("HP", "Low temp threshold set to %.1fF", threshold);
}

float GoodmanHP::getLowTempThreshold() const {
    return _lowTempThreshold;
}

void GoodmanHP::setHighAmbientHeatLockoutF(float threshold) {
    _highAmbientHeatLockoutF = threshold;
    Log.info("HP", "High-ambient HEAT lockout set to %.1fF", threshold);
}

float GoodmanHP::getHighAmbientHeatLockoutF() const {
    return _highAmbientHeatLockoutF;
}

bool GoodmanHP::isHighAmbientHeatLockoutActive() const {
    return _highAmbientHeatLockout;
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

bool GoodmanHP::isLowTempPendingEntry() const {
    return _lowTempPendingEntry;
}

bool GoodmanHP::isLowTempPendingExit() const {
    return _lowTempPendingExit;
}

uint32_t GoodmanHP::getLowTempPendingRemainingMs() const {
    if (!_lowTempPendingEntry && !_lowTempPendingExit) return 0;
    uint32_t elapsed = millis() - _lowTempPendingTick;
    if (elapsed >= LOW_TEMP_VALIDATION_MS) return 0;
    return LOW_TEMP_VALIDATION_MS - elapsed;
}

bool GoodmanHP::isStateValidating() const {
    return _stateValidationActive;
}

uint32_t GoodmanHP::getStateValidationRemainingMs() const {
    if (!_stateValidationActive) return 0;
    uint32_t elapsed = millis() - _stateValidationStart;
    if (elapsed >= _stateValidationMs) return 0;
    return _stateValidationMs - elapsed;
}

void GoodmanHP::setStateValidationMs(uint32_t ms) {
    _stateValidationMs = ms;
    Log.info("HP", "State validation delay set to %lu ms", ms);
}

uint32_t GoodmanHP::getStateValidationMs() const {
    return _stateValidationMs;
}

bool GoodmanHP::canTransitionToNormalState() {
    if (!_stateValidationActive) return true;
    if (_manualOverride) return true;  // Skip validation during manual override
    uint32_t now = millis();
    uint32_t elapsed = now - _stateValidationStart;
    if (elapsed >= _stateValidationMs) {
        _stateValidationActive = false;
        Log.info("HP", "State validation complete: %s confirmed", getStateString());
        return true;
    }
    uint32_t remaining = (_stateValidationMs - elapsed) / 1000UL;
    Log.debug("HP", "State validation blocked: transition (%lus remaining)", remaining);
    // Throttled info-level log every 30s
    if (now - _lastValidationLogTick >= 30000UL) {
        _lastValidationLogTick = now;
        Log.info("HP", "State validation hold: %s (%lus remaining)", getStateString(), remaining);
    }
    return false;
}

void GoodmanHP::startStateValidation() {
    if (_stateValidationMs == 0) return;  // Disabled
    if (_manualOverride) return;  // Skip during manual override
    _stateValidationActive = true;
    _stateValidationStart = millis();
    _lastValidationLogTick = millis();
}

void GoodmanHP::restoreSoftwareDefrost() {
    _softwareDefrost = true;
    // Snapshot band on restore (will re-snapshot when defrost actually starts)
    _activeDefrostBand = selectDefrostBand();
    _defrostSnapshotBand = _defrostBands[(int)_activeDefrostBand];
    Log.warn("HP", "Software defrost state restored from config (band=%s)", getActiveDefrostBandString());
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

    // DFT off means temps > 32°F — no ice on coils, clear runtime after 30s debounce
    if (!isDFTActive() && !_softwareDefrost) {
        if (_dftOffStartTick == 0) {
            _dftOffStartTick = now;
        } else if (_heatRuntimeMs > 0 && now - _dftOffStartTick >= 30000UL) {
            Log.info("HP", "DFT off for 30s (temps > 32F), resetting heat runtime (%lu min accumulated)", _heatRuntimeMs / 60000UL);
            resetHeatRuntime();
            _heatRuntimeLastTick = now;
            return;
        }
    } else {
        _dftOffStartTick = 0;  // DFT back on or defrost active, reset debounce
    }

    // Only accumulate in HEAT mode when CNT is on, DFT is active (coils cold enough for ice),
    // and not currently in software defrost
    OutPin* cnt = getOutput("CNT");
    if (_state == State::HEAT && cnt != nullptr && cnt->isOn() && !_softwareDefrost && isDFTActive()) {
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

    // Safety: abort any defrost/exit sequence if Y is not active
    bool yActive = isYActive();
    if (!yActive && (_defrostExiting || _defrostTransition || _defrostCntPending)) {
        if (_defrostExiting || _defrostTransition || _defrostCntPending) {
            OutPin* w = getOutput("W");
            OutPin* cnt = getOutput("CNT");
            OutPin* fan = getOutput("FAN");
            if (w != nullptr) w->turnOff();
            if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
            if (fan != nullptr) fan->turnOff();
            safeRvOff();
            Log.warn("HP", "Y inactive during defrost/exit sequence, all outputs OFF");
        }
        _defrostExiting = false;
        _defrostTransition = false;
        _defrostCntPending = false;
        // _softwareDefrost stays set for resume on next Y in HEAT mode
        return;
    }

    // Exit Phase 1: Pressure equalization after defrost (CNT+FAN+W off, RV still on)
    if (_defrostTransition && _defrostExiting) {
        if (now - _defrostTransitionStart >= _rvShortCycleMs) {
            _defrostTransition = false;
            Log.info("HP", "Exit Phase 1 complete, RV off, waiting %lu s CNT short cycle",
                     _cntShortCycleMs / 1000UL);
            OutPin* rv = getOutput("RV");
            if (rv != nullptr) rv->turnOff();
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

    // If software defrost is active, check exit conditions using snapshotted band
    if (_softwareDefrost) {
        // Guard: _defrostStartTick is only set when Phase 3 begins.
        // If still 0, defrost never reached active phase (Y dropped during Phase 1/2).
        // Don't check timeout — defrost will resume from Phase 1 when Y reactivates.
        if (_defrostStartTick == 0) {
            return;
        }

        uint32_t elapsed = now - _defrostStartTick;

        // Enforce minimum runtime (from snapshotted band)
        if (elapsed < _defrostSnapshotBand.minRuntimeMs) {
            return;
        }

        // Safety timeout
        if (elapsed >= DEFROST_TIMEOUT_MS) {
            Log.error("HP", "Defrost timeout (%lu min), forcing stop", DEFROST_TIMEOUT_MS / 60000UL);
            stopSoftwareDefrost();
            return;
        }

        // Check condenser temp every 1 minute (exit temp from snapshotted band)
        if (now - _defrostLastCondCheckTick >= DEFROST_COND_CHECK_MS) {
            _defrostLastCondCheckTick = now;
            TempSensor* condenser = getTempSensor("CONDENSER_TEMP");
            if (condenser != nullptr && condenser->isValid()) {
                float condTemp = condenser->getValue();
                Log.info("HP", "Defrost condenser check: %.1fF (target > %.1fF, band=%s, elapsed %lu sec)",
                         condTemp, _defrostSnapshotBand.exitTempF,
                         getActiveDefrostBandString(), elapsed / 1000UL);
                if (condTemp >= _defrostSnapshotBand.exitTempF) {
                    Log.info("HP", "Defrost complete: condenser %.1fF >= %.1fF (band=%s)",
                             condTemp, _defrostSnapshotBand.exitTempF,
                             getActiveDefrostBandString());
                    stopSoftwareDefrost();
                    return;
                }
            }
        }
        return;
    }

    // Always update active band for dashboard display
    _activeDefrostBand = selectDefrostBand();

    // Only trigger new defrost while in HEAT state (Y active, compressor running)
    if (_state != State::HEAT) return;
    const DefrostBand& band = _defrostBands[(int)_activeDefrostBand];

    // Check if heat runtime threshold reached (band-specific)
    if (_heatRuntimeMs >= band.runtimeThresholdMs) {
        Log.info("HP", "Heat runtime %lu min >= %lu min threshold (band=%s), starting defrost",
                 _heatRuntimeMs / 60000UL, band.runtimeThresholdMs / 60000UL,
                 getActiveDefrostBandString());
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

    // Snapshot active band params for stable exit criteria during defrost
    _defrostSnapshotBand = _defrostBands[(int)_activeDefrostBand];
    Log.info("HP", "Starting defrost transition (band=%s, minRT=%lu s, exit=%.1fF, %lu s RV short cycle)",
             getActiveDefrostBandString(),
             _defrostSnapshotBand.minRuntimeMs / 1000UL,
             _defrostSnapshotBand.exitTempF,
             _rvShortCycleMs / 1000UL);

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
    if (!isYActive()) {
        Log.info("HP", "Defrost stopped (Y inactive), no exit transition");
        if (cnt != nullptr) { cnt->turnOff(); _cntActivated = false; }
        if (fan != nullptr) fan->turnOff();
        OutPin* w = getOutput("W");
        if (w != nullptr) w->turnOff();
        safeRvOff();
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

    // Turn off CNT, FAN, and W — only RV stays on during exit Phase 1
    if (cnt != nullptr) {
        cnt->turnOff();
        _cntActivated = false;
    }
    if (fan != nullptr) fan->turnOff();
    OutPin* w = getOutput("W");
    if (w != nullptr) w->turnOff();

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

String GoodmanHP::setManualOverride(bool on) {
    if (on && _startupLockout) {
        uint32_t remainSec = (getStartupLockoutRemainingMs() + 999) / 1000;
        Log.warn("HP", "Manual override blocked: startup lockout (%us remaining)", remainSec);
        return "Startup lockout: " + String(remainSec) + "s remaining";
    }
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
        _heatTransition = false;
        _heatCntPending = false;
    } else if (!on && _manualOverride) {
        _manualOverride = false;
        // Turn all outputs off and let state machine resume
        for (auto& pair : _outputMap) {
            if (pair.second != nullptr) {
                pair.second->turnOff();
            }
        }
        _cntActivated = false;
        // Reset Y tracking so normal activation sequence (FAN on, 30s CNT delay) resumes
        _yWasActive = false;
        Log.warn("HP", "MANUAL OVERRIDE disabled, all outputs OFF");
    }
    return "";
}

String GoodmanHP::setManualOutput(const String& name, bool on) {
    if (!_manualOverride) return "Manual override not active";
    if (_startupLockout) return "Startup lockout active";

    OutPin* pin = getOutput(name);
    if (pin == nullptr) return "Output not found: " + name;

    if (on && name == "CNT") {
        // Apply short cycle protection
        if (pin->getOffTick() > 0) {
            uint32_t offElapsed = millis() - pin->getOffTick();
            if (offElapsed < _cntShortCycleMs) {
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
    if (_heatTransition || _heatCntPending) return "HEAT mode transition in progress";
    if (_state != State::HEAT) return "Must be in HEAT mode (current: " + String(getStateString()) + ")";
    if (_lpsFault) return "LPS fault active";
    if (_compressorOverTemp) return "Compressor over-temp active";
    if (_lowTemp) return "Low temp protection active";
    if (_rvFail) return "RV fail active";

    _activeDefrostBand = selectDefrostBand();
    Log.warn("HP", "FORCE DEFROST initiated from web interface (band=%s)", getActiveDefrostBandString());
    startSoftwareDefrost();
    return "";
}

float GoodmanHP::getSubcoolingF() const {
    auto it1 = _tempSensorMap.find("CONDENSER_TEMP");
    auto it2 = _tempSensorMap.find("LIQUID_TEMP");
    if (it1 == _tempSensorMap.end() || it2 == _tempSensorMap.end()) return 0.0f;
    if (!it1->second->isValid() || !it2->second->isValid()) return 0.0f;
    return it1->second->getValue() - it2->second->getValue();
}

bool GoodmanHP::isSubcoolingValid() const {
    auto it1 = _tempSensorMap.find("CONDENSER_TEMP");
    auto it2 = _tempSensorMap.find("LIQUID_TEMP");
    if (it1 == _tempSensorMap.end() || it2 == _tempSensorMap.end()) return false;
    if (!it1->second->isValid() || !it2->second->isValid()) return false;
    return (_state == State::HEAT || _state == State::COOL || _state == State::DEFROST);
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
