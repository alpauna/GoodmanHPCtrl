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
#include "CurrentSensor.h"

class GoodmanHP {
  public:
    enum class State { OFF, COOL, HEAT, DEFROST, ERROR, LOW_TEMP };
    enum class AmbientSource { SENSOR, WEATHER, INTERNAL };
    enum class DefrostBandName { COLD, MID, WARM };
    struct DefrostBand {
        uint32_t runtimeThresholdMs;
        uint32_t minRuntimeMs;
        float exitTempF;
    };
    typedef std::function<void(State newState, State oldState)> StateChangeCallback;
    typedef std::function<void(bool active)> LPSFaultCallback;

    // Defrost constants — per-band defaults
    static const uint32_t DEFROST_COLD_RUNTIME_MS = 30UL * 60 * 1000;    // 30 min
    static const uint32_t DEFROST_COLD_MIN_RUNTIME_MS = 2UL * 60 * 1000; // 2 min
    static constexpr float DEFROST_COLD_EXIT_F = 50.0f;
    static const uint32_t DEFROST_MID_RUNTIME_MS = 60UL * 60 * 1000;     // 60 min
    static const uint32_t DEFROST_MID_MIN_RUNTIME_MS = 90UL * 1000;      // 1.5 min
    static constexpr float DEFROST_MID_EXIT_F = 55.0f;
    static const uint32_t DEFROST_WARM_RUNTIME_MS = 90UL * 60 * 1000;    // 90 min
    static const uint32_t DEFROST_WARM_MIN_RUNTIME_MS = 60UL * 1000;     // 1 min
    static constexpr float DEFROST_WARM_EXIT_F = 60.0f;
    static constexpr float DEFROST_COLD_MAX_TEMP_F = 23.0f;
    static constexpr float DEFROST_WARM_MIN_TEMP_F = 31.0f;
    static const uint32_t DEFROST_TIMEOUT_MS = 15UL * 60 * 1000;         // 15 min safety timeout
    static const uint32_t DEFROST_COND_CHECK_MS = 60UL * 1000;            // 1 min condenser recheck
    static constexpr float DEFAULT_LOW_TEMP_F = 20.0f;
    static const uint32_t LOW_TEMP_VALIDATION_MS = 10UL * 60 * 1000;  // 10 min validation delay

    // High suction temp / RV fail detection during defrost
    static constexpr float DEFAULT_HIGH_SUCTION_TEMP_F = 140.0f;
    static const uint32_t DEFAULT_RV_SHORT_CYCLE_MS = 30UL * 1000;   // 30s pressure equalization
    static const uint32_t DEFAULT_CNT_SHORT_CYCLE_MS = 30UL * 1000;  // 30s (configurable Y_DELAY_MS)

    // Startup lockout — keep all outputs OFF until sensors stabilize
    static const uint32_t STARTUP_LOCKOUT_MS = 3UL * 60 * 1000;  // 3 min

    // Manual override timeout
    static const uint32_t MANUAL_OVERRIDE_TIMEOUT_MS = 30UL * 60 * 1000;  // 30 min

    // Compressor over-temperature protection
    static constexpr float COMPRESSOR_OVERTEMP_ON_F = 240.0f;   // Shut down CNT above this
    static constexpr float COMPRESSOR_OVERTEMP_OFF_F = 190.0f;  // Resume CNT below this
    static const uint32_t COMPRESSOR_OVERTEMP_CHECK_MS = 60UL * 1000;  // 1 min recheck

    // Suction low-temperature protection (COOL mode only)
    static constexpr float SUCTION_WARN_F = 34.0f;       // Warn below this
    static constexpr float SUCTION_CRITICAL_F = 32.0f;    // Shut down CNT below this
    static constexpr float SUCTION_RESUME_F = 40.0f;      // Resume above this
    static const uint32_t SUCTION_CHECK_MS = 60UL * 1000; // 1 min recheck

    // State table validation interval
    static const uint32_t STATE_VALIDATE_MS = 10UL * 1000;  // 10s periodic check

    // FAN fault protection (CNT running but FAN not moving air)
    // If FAN current stays below FAN_MIN_RUNNING_AMPS for FAN_NO_CURRENT_TIMEOUT_MS
    // while the compressor (CNT) is energized, shut CNT down and latch an ERROR
    // state for FAN_FAULT_ERROR_MS. After that grace period, if Y is still active
    // and CNT is being re-energized, FAN must draw current within the same
    // FAN_NO_CURRENT_TIMEOUT_MS window or the fault is re-latched.
    static constexpr float FAN_MIN_RUNNING_AMPS = 0.3f;              // Below this = FAN not running
    static const uint32_t FAN_NO_CURRENT_TIMEOUT_MS = 20UL * 1000;   // 20s to see FAN current
    static const uint32_t FAN_FAULT_ERROR_MS       = 3UL * 60 * 1000; // 3 min ERROR lockout

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

    // Current sensor management
    void setADS1115(Adafruit_ADS1115* ads) { _ads1115 = ads; }
    void addCurrentSensor(const String& name, CurrentSensor* sensor);
    CurrentSensor* getCurrentSensor(const String& name);
    CurrentSensorMap& getCurrentSensorMap();
    void readCurrentSensors();
    bool isOvercurrentActive() const;
    bool isLockedRotorActive() const;

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
    uint32_t getDefrostElapsedMs() const;
    bool isLPSFaultActive() const;
    bool isLowTempActive() const;
    bool isCompressorOverTempActive() const;
    bool isSuctionLowTempActive() const;
    void setLowTempThreshold(float threshold);
    float getLowTempThreshold() const;
    void setLowTempEnableW(bool enable);
    bool getLowTempEnableW() const;
    void setLowTempEnableAux(bool enable);
    bool getLowTempEnableAux() const;
    bool isLowTempPendingEntry() const;
    bool isLowTempPendingExit() const;
    uint32_t getLowTempPendingRemainingMs() const;

    bool isStartupLockoutActive() const;
    uint32_t getStartupLockoutRemainingMs() const;
    bool isShortCycleProtectionActive() const;

    // FAN fault detection (no fan current while compressor running)
    bool isFanFaultActive() const;
    uint32_t getFanFaultRemainingMs() const;
    void clearFanFault();

    // RV fail / high suction temp detection
    bool isRvFailActive() const;
    bool isHighSuctionTempActive() const;
    bool isDefrostTransitionActive() const;
    bool isDefrostCntPendingActive() const;
    bool isDefrostExitingActive() const;
    uint32_t getDefrostCntPendingRemainingMs() const;
    void clearRvFail();
    void setRvFail();
    void setHighSuctionTempThreshold(float f);
    float getHighSuctionTempThreshold() const;
    void setRvShortCycleMs(uint32_t ms);
    uint32_t getRvShortCycleMs() const;
    void setCntShortCycleMs(uint32_t ms);
    uint32_t getCntShortCycleMs() const;
    uint32_t getDefrostTransitionRemainingMs() const;

    // HEAT→COOL transition (RV switch sequencing)
    bool isCoolTransitionActive() const;
    bool isCoolCntPendingActive() const;
    uint32_t getCoolTransitionRemainingMs() const;
    uint32_t getCoolCntPendingRemainingMs() const;

    // RV pressure equalization hold
    bool isRvHoldActive() const;
    uint32_t getRvHoldRemainingMs() const;
    void handleBootRvHold(bool rvWasOn, bool cntWasOn);

    // Ambient sensor failsafe (weather / ESP32 internal temp)
    bool isAmbientFallbackActive() const;
    AmbientSource getAmbientSource() const;
    void setWeatherTemp(float tempF);
    void setWeatherStaleMs(uint32_t ms);
    void setInternalTempOffsetF(float offset);
    float getInternalTempOffsetF() const;
    void setAmbientFailoverTest(bool on);
    bool isAmbientFailoverTestActive() const;
    uint32_t getFailoverTestRemainingSec() const;
    float getWeatherTempF() const;
    bool isWeatherTempValid() const;
    uint32_t getWeatherTempAgeSec() const;

    // COOL→HEAT transition (RV switch sequencing)
    bool isHeatTransitionActive() const;
    bool isHeatCntPendingActive() const;
    uint32_t getHeatTransitionRemainingMs() const;
    uint32_t getHeatCntPendingRemainingMs() const;

    // Adaptive defrost band parameters
    void setColdMaxTempF(float f);
    float getColdMaxTempF() const;
    void setWarmMinTempF(float f);
    float getWarmMinTempF() const;
    void setDefrostBand(DefrostBandName band, const DefrostBand& params);
    DefrostBand getDefrostBand(DefrostBandName band) const;
    DefrostBandName getActiveDefrostBand() const;
    const char* getActiveDefrostBandString() const;
    uint32_t getActiveRuntimeThresholdMs() const;
    uint32_t getActiveMinRuntimeMs() const;
    float getActiveExitTempF() const;

    // State validation timer (prevents rapid state cycling)
    bool isStateValidating() const;
    uint32_t getStateValidationRemainingMs() const;
    void setStateValidationMs(uint32_t ms);
    uint32_t getStateValidationMs() const;

    // Manual override for pin control page
    bool isManualOverrideActive() const;
    uint32_t getManualOverrideRemainingMs() const;
    String setManualOverride(bool on);
    String setManualOutput(const String& name, bool on);
    String forceDefrost();

    // Subcooling calculation (CONDENSER_TEMP - LIQUID_TEMP)
    float getSubcoolingF() const;
    bool isSubcoolingValid() const;

    // CAN mode — replace physical Y/O with CAN-provided mode
    void setCANMode(bool enabled);
    bool isCANMode() const;
    void setCANInputState(bool yActive, bool oActive);
    uint32_t getCANLastRxTick() const;

    void restoreSoftwareDefrost();

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

    // Current sensors (ADS1115 ADC)
    Adafruit_ADS1115* _ads1115;
    CurrentSensorMap _currentSensorMap;

    State _state;
    uint32_t _yActiveStartTick;
    bool _yWasActive;
    bool _cntActivated;

    uint32_t _cntShortCycleMs;  // Configurable CNT short cycle delay (default 30s)
    // Adaptive defrost bands
    float _coldMaxTempF;                  // Band breakpoint: ≤ this = Cold
    float _warmMinTempF;                  // Band breakpoint: ≥ this = Warm
    DefrostBand _defrostBands[3];         // [COLD, MID, WARM]
    DefrostBandName _activeDefrostBand;   // Currently selected band
    DefrostBand _defrostSnapshotBand;     // Snapshot when defrost starts

    // Heat runtime accumulation & automatic defrost
    uint32_t _heatRuntimeMs;
    uint32_t _heatRuntimeLastTick;
    uint32_t _heatRuntimeLastLogMs;
    uint32_t _dftOffStartTick;        // When DFT first went off (0 = DFT is on)
    bool _softwareDefrost;
    uint32_t _defrostStartTick;
    uint32_t _defrostLastCondCheckTick;
    bool _lpsFault;
    bool _lowTemp;
    float _lowTempThreshold;
    bool _lowTempEnableW;
    bool _lowTempEnableAux;
    bool _lowTempPendingEntry;        // Temp below threshold, waiting validation
    bool _lowTempPendingExit;         // Temp above threshold, waiting validation
    uint32_t _lowTempPendingTick;     // millis() when pending timer started
    bool _compressorOverTemp;
    uint32_t _compressorOverTempStartTick;
    uint32_t _compressorOverTempLastCheckTick;
    bool _suctionLowTemp;
    uint32_t _suctionLowTempStartTick;
    uint32_t _suctionLowTempLastCheckTick;
    bool _rvFail;                     // Latched RV fail flag
    bool _highSuctionTemp;            // Current high suction temp condition
    float _highSuctionTempThreshold;  // Configurable threshold (default 140°F)
    uint32_t _rvShortCycleMs;         // RV short cycle duration (configurable)
    bool _defrostTransition;          // True during Phase 1 (RV pressure equalization)
    uint32_t _defrostTransitionStart; // millis() when Phase 1 started
    bool _defrostCntPending;          // True during Phase 2 (RV+W on, waiting for CNT SC)
    uint32_t _defrostCntPendingStart; // millis() when Phase 2 started
    bool _defrostExiting;             // True during defrost exit transition (reverse 3-phase)
    // HEAT→COOL mode transition (RV switch sequencing)
    bool _coolTransition;             // Phase 1: CNT+W off, pressure equalization before RV switch
    uint32_t _coolTransitionStart;
    bool _coolCntPending;             // Phase 2: RV on, waiting CNT short cycle
    uint32_t _coolCntPendingStart;
    // COOL→HEAT mode transition (RV switch sequencing)
    bool _heatTransition;             // Phase 1: CNT off, pressure equalization before RV switch
    uint32_t _heatTransitionStart;
    bool _heatCntPending;             // Phase 2: RV off, waiting CNT short cycle
    uint32_t _heatCntPendingStart;
    // RV pressure equalization hold (prevents RV switching under pressure)
    bool _rvHoldActive;               // True when RV off is deferred for pressure equalization
    // Ambient sensor failsafe
    AmbientSource _ambientSource;     // Current ambient temp data source
    float _weatherTempF;              // Cached weather temperature in Fahrenheit
    bool _weatherTempValid;           // True when weather cache has been populated
    uint32_t _weatherTempTick;        // millis() when weather temp was last updated
    uint32_t _weatherStaleMs;         // Max weather cache age (default 30 min)
    float _internalTempOffsetF;       // ESP32 internal temp offset in °F (default 0)
    bool _ambientFailoverTest;        // True when failover test is active (forces sensor invalid)
    uint32_t _failoverTestStartTick;  // millis() when failover test started
    // State validation timer
    bool _stateValidationActive;
    uint32_t _stateValidationStart;
    uint32_t _stateValidationMs;  // default 30000
    uint32_t _lastValidationLogTick;

    // CAN mode — virtual Y/O from CAN bus
    bool _canMode = false;
    bool _canYActive = false;
    bool _canOActive = false;
    uint32_t _canLastRxTick = 0;
    static const uint32_t CAN_TIMEOUT_MS = 10000;  // 10s safe shutdown if CAN lost

    // FAN fault (compressor running with no fan current)
    bool _fanFault;                   // True while FAN fault is latched
    uint32_t _fanFaultStartTick;      // millis() when ERROR lockout began
    uint32_t _fanNoCurrentStartTick;  // millis() when FAN current first read low with CNT on (0 = clear)
    uint8_t _fanLowConsecutive;       // Consecutive low-current reads (gates spurious single-sample glitches)
    static const uint8_t FAN_LOW_CONSECUTIVE_REQUIRED = 3;  // Require 3 consecutive low reads (~3s) before arming watchdog

    bool _manualOverride;
    uint32_t _manualOverrideStart;
    bool _startupLockout;
    uint32_t _startupTick;
    StateChangeCallback _stateChangeCb;
    LPSFaultCallback _lpsFaultCb;

    DefrostBandName selectDefrostBand();
    void checkLPSFault();
    void checkCurrentProtections();
    void checkAmbientTemp();
    void checkCompressorTemp();
    void checkSuctionTemp();
    void checkHighSuctionTemp();
    void checkYAndActivateCNT();
    void checkFanFault();
    void updateState();
    void accumulateHeatRuntime();
    void checkDefrostNeeded();
    void checkCoolTransition();
    void checkHeatTransition();
    void safeRvOff();
    void checkRvHold();
    void validateOutputStates();
    void startSoftwareDefrost();
    void stopSoftwareDefrost();
    bool canTransitionToNormalState();
    void startStateValidation();

    uint32_t _lastValidateTick;

    // Runtime callback for OutPins
    static GoodmanHP* _instance;
    static bool outPinRuntimeCallback(OutPin* pin, uint32_t onDuration);
    bool handleOutPinRuntime(OutPin* pin, uint32_t onDuration);
};

#endif
