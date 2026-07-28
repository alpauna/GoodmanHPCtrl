#ifndef CURRENTSENSOR_H
#define CURRENTSENSOR_H

#include <Arduino.h>
#include <map>
#include <Adafruit_ADS1X15.h>

class CurrentSensor;
typedef std::map<String, CurrentSensor*> CurrentSensorMap;

class CurrentSensor {
  public:
    CurrentSensor(const String& name, uint8_t channel, float ctRatio = 30.0f);

    // Non-blocking RMS acquisition. Sampling the ADS1115 60x per read to
    // compute RMS current used to block synchronously (~470ms per sensor
    // at 128 SPS) — see BUG-014. beginSample() starts a new 60-sample
    // acquisition; tick() advances it by one non-blocking conversion per
    // call and must be called repeatedly (e.g. every loop pass) until it
    // returns true, at which point getRMSAmps()/getPeakAmps()/isValid()
    // reflect the completed read.
    void beginSample(Adafruit_ADS1115* ads);
    bool tick(Adafruit_ADS1115* ads);
    bool isSampling() const { return _sampleState == SampleState::CONVERTING; }

    // Check overcurrent and locked rotor protections
    void checkProtections();

    // Getters
    String getName() const { return _name; }
    uint8_t getChannel() const { return _channel; }
    float getRMSAmps() const { return _rmsAmps; }
    float getPeakAmps() const { return _peakAmps; }
    float getPrevious() const { return _previous; }
    bool isValid() const { return _valid; }
    bool isOvercurrent() const { return _overcurrent; }
    bool isLockedRotor() const { return _lockedRotor; }
    float getOvercurrentThreshold() const { return _overcurrentThreshold; }
    uint32_t getOvercurrentDelayMs() const { return _overcurrentDelayMs; }
    float getLockedRotorThreshold() const { return _lockedRotorThreshold; }
    uint32_t getLockedRotorTimeoutMs() const { return _lockedRotorTimeoutMs; }
    float getCtRatio() const { return _ctRatio; }

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
    uint8_t _channel;         // ADS1115 differential pair (0 = A0-A1, 1 = A2-A3)
    float _ctRatio;           // CT clamp ratio (30.0 for SCT-013-030)
    float _rmsAmps;
    float _peakAmps;
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

    // Non-blocking RMS acquisition state
    enum class SampleState { IDLE, CONVERTING };
    SampleState _sampleState = SampleState::IDLE;
    static const int NUM_RMS_SAMPLES = 60;
    int _sampleIndex = 0;
    float _sumV = 0.0f;
    float _sumSquares = 0.0f;
    float _peakAccum = 0.0f;
    int _nonZeroCount = 0;

    void startConversion(Adafruit_ADS1115* ads);
};

#endif
