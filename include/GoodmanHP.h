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
    typedef std::function<void(State newState, State oldState)> StateChangeCallback;
    typedef std::function<void(bool active)> LPSFaultCallback;

    // Defrost constants
    static const uint32_t HEAT_RUNTIME_THRESHOLD_MS = 90UL * 60 * 1000;  // 90 min
    static const uint32_t DEFROST_MIN_RUNTIME_MS = 3UL * 60 * 1000;      // 3 min minimum defrost
    static const uint32_t DEFROST_TIMEOUT_MS = 15UL * 60 * 1000;         // 15 min safety timeout
    static constexpr float DEFROST_EXIT_F = 60.0f;
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

    // COOL→HEAT transition (RV switch sequencing)
    bool isHeatTransitionActive() const;
    bool isHeatCntPendingActive() const;
    uint32_t getHeatTransitionRemainingMs() const;
    uint32_t getHeatCntPendingRemainingMs() const;

    // Configurable defrost parameters
    void setDefrostMinRuntimeMs(uint32_t ms);
    uint32_t getDefrostMinRuntimeMs() const;
    void setDefrostExitTempF(float f);
    float getDefrostExitTempF() const;
    void setHeatRuntimeThresholdMs(uint32_t ms);
    uint32_t getHeatRuntimeThresholdMs() const;

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
    uint32_t _defrostMinRuntimeMs;  // Configurable defrost min runtime (default 3 min)
    float _defrostExitTempF;        // Configurable condenser temp cutoff (default 60°F)
    uint32_t _heatRuntimeThresholdMs; // Configurable heat runtime threshold for defrost (default 90 min)

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
    // State validation timer
    bool _stateValidationActive;
    uint32_t _stateValidationStart;
    uint32_t _stateValidationMs;  // default 30000
    uint32_t _lastValidationLogTick;

    bool _manualOverride;
    uint32_t _manualOverrideStart;
    bool _startupLockout;
    uint32_t _startupTick;
    StateChangeCallback _stateChangeCb;
    LPSFaultCallback _lpsFaultCb;

    void checkLPSFault();
    void checkCurrentProtections();
    void checkAmbientTemp();
    void checkCompressorTemp();
    void checkSuctionTemp();
    void checkHighSuctionTemp();
    void checkYAndActivateCNT();
    void updateState();
    void accumulateHeatRuntime();
    void checkDefrostNeeded();
    void checkCoolTransition();
    void checkHeatTransition();
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
