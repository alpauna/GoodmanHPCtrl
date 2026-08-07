#ifndef CURRENTSENSOR_H
#define CURRENTSENSOR_H

#include <Arduino.h>
#include <map>

class SPICurrentADC;
class CurrentSensor;
typedef std::map<String, CurrentSensor*> CurrentSensorMap;

class CurrentSensor {
  public:
    CurrentSensor(const String& name, uint8_t channel, float ctRatio = 30.0f);

    // Pure accumulator now -- ADC I/O lives in SPICurrentADC, which owns the
    // simultaneous 4-channel SPI transport and pushes each frame's sample
    // to every attached CurrentSensor. Called once per frame (up to ~4kHz)
    // from SPICurrentADC::tick(). Accumulates RMS/peak/own-zero-crossing
    // state and finalizes a window every ZX_EDGES_PER_WINDOW zero-cross
    // pulses (falls back to a wall-clock timeout if ZX is absent/broken).
    void pushSample(int32_t rawCounts, uint64_t sampleTimeUs);

    // Links this sensor to the ADC that feeds it, so pushSample() can read
    // the shared zero-cross timing (getLastZxEdgeUs()/getLinePeriodUs()) for
    // phase-angle/power-factor computation. Set by SPICurrentADC::attachChannel().
    void setSourceADC(const SPICurrentADC* adc) { _adc = adc; }

    // Marks this channel's data invalid immediately -- called by
    // SPICurrentADC on a DRDY-stall watchdog trip (BUG-015-class transport
    // fault), or internally when this channel's own finalized readings are
    // bit-identical across several consecutive windows (stuck-channel-
    // behind-a-live-bus, a different failure mode from a transport stall).
    // Latches until reboot, same "no silent auto-recovery" policy as the
    // ADC-level comms fault.
    void markStale();

    // True once a window has finished accumulating since the last call --
    // GoodmanHP polls this to know when to call checkProtections(), the
    // same call-site ownership tickCurrentSensors() used to have.
    bool consumeWindowCompleted();

    // Check overcurrent and locked rotor protections
    void checkProtections();

    // Getters
    String getName() const { return _name; }
    uint8_t getChannel() const { return _channel; }
    float getRMSAmps() const { return _rmsAmps; }
    float getPeakAmps() const { return _peakAmps; }
    float getRMSMillivolts() const { return _rmsMillivolts; }
    float getPrevious() const { return _previous; }
    bool isValid() const { return _valid; }
    bool isOvercurrent() const { return _overcurrent; }
    bool isLockedRotor() const { return _lockedRotor; }
    float getOvercurrentThreshold() const { return _overcurrentThreshold; }
    uint32_t getOvercurrentDelayMs() const { return _overcurrentDelayMs; }
    float getLockedRotorThreshold() const { return _lockedRotorThreshold; }
    uint32_t getLockedRotorTimeoutMs() const { return _lockedRotorTimeoutMs; }
    float getCtRatio() const { return _ctRatio; }

    // Power factor (displacement, cos(phase angle) between this channel's
    // zero-crossings and the shared ZX reference -- appropriate for a
    // predominantly inductive motor load, see design doc "Power factor and
    // true power"). Only valid once the ADC has a locked line-frequency
    // reference and this channel has seen its own zero-crossing this window.
    float getPowerFactor() const { return _powerFactor; }
    float getPhaseAngleDeg() const { return _phaseAngleDeg; }
    bool isPowerFactorValid() const { return _pfValid; }

    // Setters
    void setOvercurrentThreshold(float amps);
    void setOvercurrentDelayMs(uint32_t ms) { _overcurrentDelayMs = ms; }
    void setCtRatio(float ratio) { _ctRatio = ratio; }
    void setLockedRotorThreshold(float amps) { _lockedRotorThreshold = amps; }
    void setLockedRotorTimeoutMs(uint32_t ms) { _lockedRotorTimeoutMs = ms; }

    // CNT activation notification for locked rotor timing
    void notifyCntActivated();

    // Manual clear of latched locked rotor fault
    void clearLockedRotor();

  private:
    String _name;
    uint8_t _channel;         // ADS131M04 input channel (0=comp, 1=fan, 3=crankcase)
    float _ctRatio;           // CT clamp ratio (30.0 for SCT-013-030, 5.0 for SCT-013-005)
    float _rmsAmps;
    float _peakAmps;
    float _rmsMillivolts;     // Raw ADC RMS reading before ctRatio scaling — for calibration/diagnostics
    float _previous;
    bool _valid;

    // Overcurrent protection
    float _overcurrentThreshold;   // Amps threshold (0 = disabled)
    bool _overcurrent;
    uint32_t _overcurrentStartTick;
    uint32_t _overcurrentDelayMs;  // How long overcurrent must persist (default 5000ms)

    // Locked rotor detection
    float _lockedRotorThreshold;   // Amps threshold (0 = disabled)
    bool _lockedRotor;             // Latched fault
    uint32_t _lockedRotorTimeoutMs; // Max inrush settle time (default 5000ms)
    uint32_t _cntActivateTick;     // When CNT was last activated
    bool _cntJustActivated;        // Tracking flag for locked rotor window

    // Accumulation window state (continuous -- a new window starts the
    // instant the previous one finalizes, no idle gap)
    static const uint32_t ZX_EDGES_PER_WINDOW = 6;   // 3 full 60Hz cycles, matches the old ~470ms window
    static const uint32_t WINDOW_TIMEOUT_MS = 500;   // Fallback finalize if ZX is absent/broken
    bool _windowActive;
    uint32_t _windowStartMs;
    uint32_t _windowStartEdgeCount;
    float _sumV;
    float _sumSquares;
    float _peakAccum;
    uint32_t _nonZeroCount;
    uint32_t _sampleCount;
    bool _windowJustCompleted;

    // Own zero-crossing detection (sign-change with deadband -- current
    // channels are true AC-coupled sinusoids centered on 0, unlike ZX)
    static const int32_t OWN_ZC_DEADBAND = 2000;  // raw counts; tune once real CT load data is available
    bool _ownHavePrev;
    int8_t _ownPrevSign;      // -1, 0, or +1
    int32_t _ownPrevRaw;
    uint64_t _ownPrevTimeUs;
    float _phaseSumDeg;
    uint32_t _phaseSampleCount;

    // Power factor / phase angle (finalized once per window)
    float _powerFactor;
    float _phaseAngleDeg;
    bool _pfValid;

    // Frozen-value guard (BUG-015 analogue, per-channel level): a stuck
    // channel behind an otherwise-live SPI bus would keep producing
    // bit-identical finalized readings every window even though frames keep
    // arriving -- the ADC-level DRDY watchdog can't see this on its own.
    static const uint8_t FROZEN_WINDOWS_REQUIRED = 3;
    float _lastFinalizedRmsAmps;
    float _lastFinalizedPeakAmps;
    uint8_t _frozenWindowCount;
    bool _staleMarked;

    // Rate-limits the all-zero-samples warning: with no ZX reference and
    // nothing clamped on, every window can finalize as all-zero, which at
    // the 500ms window timeout is up to 2 log writes/sec (Serial + SD +
    // WebSocket each) per channel -- real I/O pressure on the same task
    // that also has to service the web server, hit live on the bench as a
    // 94-second, 342-line spam burst. Log at most once per 10s per channel.
    static const uint32_t ALL_ZERO_LOG_INTERVAL_MS = 10000;
    uint32_t _lastAllZeroLogMs;

    const SPICurrentADC* _adc;

    void beginWindow();
    void finalizeWindow();
    void accumulateSample(int32_t rawCounts, uint64_t sampleTimeUs);
    void detectOwnZeroCross(int32_t rawCounts, uint64_t sampleTimeUs);
};

#endif
