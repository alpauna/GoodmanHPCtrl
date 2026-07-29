#include "CurrentSensor.h"
#include "Logger.h"

CurrentSensor::CurrentSensor(const String& name, uint8_t channel, float ctRatio)
    : _name(name)
    , _channel(channel)
    , _ctRatio(ctRatio)
    , _rmsAmps(0.0f)
    , _peakAmps(0.0f)
    , _rmsMillivolts(0.0f)
    , _previous(0.0f)
    , _valid(false)
    , _overcurrentThreshold(0.0f)
    , _overcurrent(false)
    , _overcurrentStartTick(0)
    , _overcurrentDelayMs(5000)
    , _lockedRotorThreshold(0.0f)
    , _lockedRotor(false)
    , _lockedRotorTimeoutMs(5000)
    , _cntActivateTick(0)
    , _cntJustActivated(false)
{
}

void CurrentSensor::startConversion(Adafruit_ADS1115* ads) {
    uint16_t mux = (_channel == 0) ? ADS1X15_REG_CONFIG_MUX_DIFF_0_1
                                    : ADS1X15_REG_CONFIG_MUX_DIFF_2_3;
    ads->startADCReading(mux, /*continuous=*/false);
}

void CurrentSensor::beginSample(Adafruit_ADS1115* ads) {
    if (ads == nullptr) {
        _valid = false;
        return;
    }

    // Set gain for ±2.048V range (1 bit = 0.0625mV)
    // SCT-013-030 outputs 0-1V AC for 0-30A
    ads->setGain(GAIN_TWO);

    // 128 SPS (the ADS1115 library default) — verified against a clamp meter
    // to read meaningfully more accurately than 860 SPS, which widens the
    // ADC's internal filter bandwidth and lets more line noise/harmonics
    // through into each sample. At ~7.8ms/sample this can no longer block
    // the main loop (see BUG-014) because sampling is spread across many
    // non-blocking tick() calls instead of one 60-sample busy-wait loop.
    ads->setDataRate(RATE_ADS1115_128SPS);

    _sumV = 0.0f;
    _sumSquares = 0.0f;
    _peakAccum = 0.0f;
    _nonZeroCount = 0;
    _sampleIndex = 0;
    startConversion(ads);
    _sampleState = SampleState::CONVERTING;
}

bool CurrentSensor::tick(Adafruit_ADS1115* ads) {
    if (_sampleState != SampleState::CONVERTING) return false;
    if (ads == nullptr) {
        _valid = false;
        _sampleState = SampleState::IDLE;
        return true;
    }
    if (!ads->conversionComplete()) return false;  // not ready — check again next tick

    int16_t raw = ads->getLastConversionResults();
    // Convert to voltage: at GAIN_TWO, 1 bit = 0.0625mV = 0.0000625V
    float voltage = raw * 0.0000625f;
    _sumV += voltage;
    _sumSquares += voltage * voltage;
    float absV = fabsf(voltage);
    if (absV > _peakAccum) _peakAccum = absV;
    if (raw != 0) _nonZeroCount++;
    _sampleIndex++;

    // Sample ~60 readings over ~3 full 60Hz cycles (~470ms at 128 SPS, spread
    // non-blocking). Increased from 20 to 60 to avoid "sample bunching" near
    // a zero crossing (caused by I2C jitter from WiFi/AsyncTCP/log flush
    // interrupts) that previously produced spurious 0.00A readings on the
    // FAN channel.
    if (_sampleIndex < NUM_RMS_SAMPLES) {
        startConversion(ads);
        return false;
    }

    _sampleState = SampleState::IDLE;

    // Reject "all zeros" reads (I2C NACK / bus contention signature) — mark
    // sample invalid instead of publishing a fake 0.00A that would trigger
    // spurious FAN_FAULT warnings.
    if (_nonZeroCount == 0) {
        _valid = false;
        Log.warn("CURRENT", "%s read invalid: all-zero samples (I2C contention?)", _name.c_str());
        return true;
    }

    // Proper AC-RMS: subtract DC mean before squaring. The prior implementation
    // (sqrt(mean(v^2))) is only correct if the differential is perfectly
    // centered on 0V; ADS1115 offset drift + non-symmetric sample windows
    // caused occasional near-zero RMS on the FAN channel.
    float mean = _sumV / NUM_RMS_SAMPLES;
    float variance = _sumSquares / NUM_RMS_SAMPLES - mean * mean;
    if (variance < 0.0f) variance = 0.0f;  // guard against FP round-off
    float vRMS = sqrtf(variance);

    // Convert voltage RMS to current: I = V * ctRatio
    // For SCT-013-030: 1V output = 30A, so ctRatio = 30.0
    _previous = _rmsAmps;
    _rmsMillivolts = vRMS * 1000.0f;
    _rmsAmps = vRMS * _ctRatio;
    _peakAmps = _peakAccum * _ctRatio;
    _valid = true;
    return true;
}

void CurrentSensor::checkProtections() {
    if (!_valid) return;

    // --- Overcurrent detection ---
    if (_overcurrentThreshold > 0.0f && !_lockedRotor) {
        if (_rmsAmps > _overcurrentThreshold) {
            if (!_overcurrent) {
                if (_overcurrentStartTick == 0) {
                    _overcurrentStartTick = millis();
                } else if (millis() - _overcurrentStartTick >= _overcurrentDelayMs) {
                    _overcurrent = true;
                    Log.error("CURRENT", "%s overcurrent: %.1fA > %.1fA for %lums",
                              _name.c_str(), _rmsAmps, _overcurrentThreshold, _overcurrentDelayMs);
                }
            }
        } else {
            if (_overcurrent) {
                _overcurrent = false;
                Log.info("CURRENT", "%s overcurrent cleared: %.1fA", _name.c_str(), _rmsAmps);
            }
            _overcurrentStartTick = 0;
        }
    }

    // --- Locked rotor detection ---
    // After CNT activation, if current stays above lockedRotorThreshold
    // for longer than lockedRotorTimeoutMs, latch a locked rotor fault
    if (_lockedRotorThreshold > 0.0f && _cntJustActivated && !_lockedRotor) {
        uint32_t elapsed = millis() - _cntActivateTick;
        if (elapsed > _lockedRotorTimeoutMs) {
            if (_rmsAmps > _lockedRotorThreshold) {
                _lockedRotor = true;
                Log.error("CURRENT", "%s LOCKED ROTOR: %.1fA > %.1fA after %lums (LATCHED)",
                          _name.c_str(), _rmsAmps, _lockedRotorThreshold, elapsed);
            } else {
                // Inrush settled normally, stop monitoring
                _cntJustActivated = false;
            }
        }
    }
}

void CurrentSensor::setOvercurrentThreshold(float amps) {
    _overcurrentThreshold = amps;
    if (amps <= 0.0f) {
        _overcurrent = false;
        _overcurrentStartTick = 0;
    }
}

void CurrentSensor::notifyCntActivated() {
    _cntActivateTick = millis();
    _cntJustActivated = true;
}

void CurrentSensor::clearLockedRotor() {
    if (_lockedRotor) {
        _lockedRotor = false;
        _cntJustActivated = false;
        Log.info("CURRENT", "%s locked rotor fault cleared", _name.c_str());
    }
}
