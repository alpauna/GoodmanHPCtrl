#include "CurrentSensor.h"
#include "SPICurrentADC.h"
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
    , _windowActive(false)
    , _windowStartMs(0)
    , _windowStartEdgeCount(0)
    , _sumV(0.0f)
    , _sumSquares(0.0f)
    , _peakAccum(0.0f)
    , _nonZeroCount(0)
    , _sampleCount(0)
    , _windowJustCompleted(false)
    , _ownHavePrev(false)
    , _ownPrevSign(0)
    , _ownPrevRaw(0)
    , _ownPrevTimeUs(0)
    , _phaseSumDeg(0.0f)
    , _phaseSampleCount(0)
    , _powerFactor(0.0f)
    , _phaseAngleDeg(0.0f)
    , _pfValid(false)
    , _lastFinalizedRmsAmps(-1.0f)
    , _lastFinalizedPeakAmps(-1.0f)
    , _frozenWindowCount(0)
    , _staleMarked(false)
    , _lastAllZeroLogMs(0)
    , _adc(nullptr)
{
}

void CurrentSensor::beginWindow() {
    _sumV = 0.0f;
    _sumSquares = 0.0f;
    _peakAccum = 0.0f;
    _nonZeroCount = 0;
    _sampleCount = 0;
    _phaseSumDeg = 0.0f;
    _phaseSampleCount = 0;
    _windowStartMs = millis();
    _windowStartEdgeCount = (_adc != nullptr) ? _adc->getZxEdgeCount() : 0;
    _windowActive = true;
}

void CurrentSensor::accumulateSample(int32_t rawCounts, uint64_t /*sampleTimeUs*/) {
    float voltage = (float)rawCounts * SPICurrentADC::LSB_VOLTS;
    _sumV += voltage;
    _sumSquares += voltage * voltage;
    float absV = fabsf(voltage);
    if (absV > _peakAccum) _peakAccum = absV;
    if (rawCounts != 0) _nonZeroCount++;
    _sampleCount++;
}

void CurrentSensor::detectOwnZeroCross(int32_t rawCounts, uint64_t sampleTimeUs) {
    bool posNow = rawCounts > OWN_ZC_DEADBAND;
    bool negNow = rawCounts < -OWN_ZC_DEADBAND;

    if (_ownHavePrev && posNow && _ownPrevSign <= 0 && rawCounts != _ownPrevRaw) {
        // Rising zero-cross between the previous and current sample —
        // interpolate the crossing instant for phase resolution.
        float frac = (float)(0 - _ownPrevRaw) / (float)(rawCounts - _ownPrevRaw);
        frac = constrain(frac, 0.0f, 1.0f);
        uint64_t tCross = _ownPrevTimeUs + (uint64_t)(frac * (float)(sampleTimeUs - _ownPrevTimeUs));

        if (_adc != nullptr && _adc->isLineFrequencyValid()) {
            uint64_t zxEdge = _adc->getLastZxEdgeUs();
            float periodUs = _adc->getLinePeriodUs();
            if (periodUs > 0.0f) {
                float halfPeriod = periodUs / 2.0f;
                float delta = (float)((int64_t)tCross - (int64_t)zxEdge);
                while (delta > halfPeriod) delta -= periodUs;
                while (delta < -halfPeriod) delta += periodUs;
                float phaseDeg = delta / periodUs * 360.0f;
                _phaseSumDeg += phaseDeg;
                _phaseSampleCount++;
            }
        }
    }

    if (posNow) _ownPrevSign = 1;
    else if (negNow) _ownPrevSign = -1;
    _ownPrevRaw = rawCounts;
    _ownPrevTimeUs = sampleTimeUs;
    _ownHavePrev = true;
}

void CurrentSensor::pushSample(int32_t rawCounts, uint64_t sampleTimeUs) {
    if (_staleMarked) return;  // latched fault — don't resume publishing without a reboot

    if (!_windowActive) beginWindow();

    accumulateSample(rawCounts, sampleTimeUs);
    detectOwnZeroCross(rawCounts, sampleTimeUs);

    uint32_t edgesNow = (_adc != nullptr) ? _adc->getZxEdgeCount() : 0;
    bool doneByEdges = (_adc != nullptr) && (edgesNow - _windowStartEdgeCount >= ZX_EDGES_PER_WINDOW);
    bool doneByTimeout = (millis() - _windowStartMs) >= WINDOW_TIMEOUT_MS;

    if (doneByEdges || doneByTimeout) {
        finalizeWindow();
        beginWindow();
    }
}

void CurrentSensor::finalizeWindow() {
    _windowActive = false;

    // Reject "all zeros" reads (dead channel / bus contention signature, or
    // simply nothing clamped on / no ZX reference yet) — mark invalid
    // instead of publishing a fake 0.00A. Rate-limited: see
    // ALL_ZERO_LOG_INTERVAL_MS comment in the header.
    if (_nonZeroCount == 0) {
        _valid = false;
        uint32_t now = millis();
        if (now - _lastAllZeroLogMs >= ALL_ZERO_LOG_INTERVAL_MS) {
            _lastAllZeroLogMs = now;
            Log.warn("CURRENT", "%s read invalid: all-zero samples", _name.c_str());
        }
        _windowJustCompleted = true;
        return;
    }

    // Proper AC-RMS: subtract DC mean before squaring.
    float n = (float)_sampleCount;
    float mean = _sumV / n;
    float variance = _sumSquares / n - mean * mean;
    if (variance < 0.0f) variance = 0.0f;  // guard against FP round-off
    float vRMS = sqrtf(variance);

    _previous = _rmsAmps;
    _rmsMillivolts = vRMS * 1000.0f;
    _rmsAmps = vRMS * _ctRatio;
    _peakAmps = _peakAccum * _ctRatio;
    _valid = true;

    // Power factor: average phase angle observed this window, only if the
    // ADC has a locked line-frequency reference and this channel actually
    // crossed zero at least once.
    if (_phaseSampleCount > 0 && _adc != nullptr && _adc->isLineFrequencyValid()) {
        _phaseAngleDeg = _phaseSumDeg / (float)_phaseSampleCount;
        _powerFactor = cosf(radians(_phaseAngleDeg));
        _pfValid = true;
    } else {
        _pfValid = false;
    }

    // Frozen-value guard — a stuck channel behind an otherwise-live bus
    // produces bit-identical finalized readings every window, since the
    // raw samples feeding the math never change.
    if (_rmsAmps == _lastFinalizedRmsAmps && _peakAmps == _lastFinalizedPeakAmps) {
        _frozenWindowCount++;
    } else {
        _frozenWindowCount = 0;
    }
    _lastFinalizedRmsAmps = _rmsAmps;
    _lastFinalizedPeakAmps = _peakAmps;
    if (_frozenWindowCount >= FROZEN_WINDOWS_REQUIRED) {
        Log.warn("CURRENT", "%s read invalid: frozen value (SPI channel stuck?)", _name.c_str());
        _staleMarked = true;
        _valid = false;
    }

    _windowJustCompleted = true;
}

bool CurrentSensor::consumeWindowCompleted() {
    bool result = _windowJustCompleted;
    _windowJustCompleted = false;
    return result;
}

void CurrentSensor::markStale() {
    _staleMarked = true;
    _valid = false;
    _pfValid = false;
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
