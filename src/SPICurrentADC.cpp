#include "SPICurrentADC.h"
#include "CurrentSensor.h"
#include "Logger.h"
#include <esp_timer.h>

// Register map (SBAS890D). Only what's needed for bring-up + basic config.
static const uint8_t REG_ID = 0x00;
static const uint8_t REG_STATUS = 0x01;
static const uint8_t REG_MODE = 0x02;
static const uint8_t REG_CLOCK = 0x03;

static const uint16_t CLOCK_RESET_DEFAULT = 0x0F0E;  // all 4 channels enabled, OSR=1024, PWR=high-res
static const uint16_t MODE_RESET_DEFAULT = 0x0510;

// OSR[2:0] -> samples/sec (Table 8-x, SBAS890D). fMOD = fCLKIN/2 = 4.096MHz.
static const uint16_t OSR_VALUES[8] = {128, 256, 512, 1024, 2048, 4096, 8192, 16256};

SPICurrentADC::SPICurrentADC(const Pins& pins, uint8_t osr, uint32_t spiHz)
    : _pins(pins)
    , _osr(osr & 0x7)
    , _spiHz(spiHz)
    , _spi(HSPI)
    , _spiSettings(spiHz, MSBFIRST, SPI_MODE1)
    , _present(false)
    , _commsFault(false)
    , _crcErrorCount(0)
    , _droppedFrameCount(0)
    , _lastValidFrameMs(0)
    , _expectedFramePeriodUs(0)
    , _channelSensor{nullptr, nullptr, nullptr, nullptr}
    , _frameQueue(nullptr)
    , _acqTaskHandle(nullptr)
    , _zxAboveThreshold(false)
    , _zxPrevRaw(0)
    , _zxPrevTimestampUs(0)
    , _zxLastEdgeUs(0)
    , _zxEdgeCount(0)
    , _lineHalfPeriodUs(0.0f)
    , _lineFrequencyHz(0.0f)
    , _lineFrequencyLastUpdateUs(0)
{
}

SPICurrentADC::~SPICurrentADC() {
    if (_acqTaskHandle != nullptr) vTaskDelete(_acqTaskHandle);
    if (_frameQueue != nullptr) vQueueDelete(_frameQueue);
}

// ---------------------------------------------------------------------------
// SPI/register primitives -- ported from bringup/ads131m04/main.cpp, proven
// working live against the daughter board (ID/STATUS reads, RESET# recovery).
// ---------------------------------------------------------------------------

uint16_t SPICurrentADC::cmdRREG(uint8_t addr, uint8_t count) {
    return (0x5 << 13) | ((addr & 0x3F) << 7) | ((count - 1) & 0x7F);
}

uint16_t SPICurrentADC::cmdWREG(uint8_t addr, uint8_t count) {
    return (0x3 << 13) | ((addr & 0x3F) << 7) | ((count - 1) & 0x7F);
}

void SPICurrentADC::spiFrame(const uint16_t* outWords, uint8_t numOutWords, uint32_t resp[FRAME_WORDS]) {
    digitalWrite(_pins.cs, LOW);
    _spi.beginTransaction(_spiSettings);

    for (int w = 0; w < FRAME_WORDS; w++) {
        uint32_t out = (w < numOutWords) ? ((uint32_t)outWords[w] << 8) : 0;  // 24-bit word, left-justified
        uint32_t in = 0;
        for (int b = 0; b < WORD_BYTES; b++) {
            uint8_t txByte = (out >> (16 - 8 * b)) & 0xFF;
            uint8_t rxByte = _spi.transfer(txByte);
            in = (in << 8) | rxByte;
        }
        resp[w] = in;
    }

    _spi.endTransaction();
    digitalWrite(_pins.cs, HIGH);
    delayMicroseconds(2);
}

uint32_t SPICurrentADC::readRegister(uint8_t addr) {
    uint32_t resp[FRAME_WORDS];
    uint16_t cmd = cmdRREG(addr, 1);
    spiFrame(&cmd, 1, resp);
    spiFrame(nullptr, 0, resp);
    return resp[0];
}

void SPICurrentADC::writeRegister(uint8_t addr, uint16_t value) {
    uint32_t resp[FRAME_WORDS];
    uint16_t words[2] = {cmdWREG(addr, 1), value};
    spiFrame(words, 2, resp);
    spiFrame(nullptr, 0, resp);
}

void SPICurrentADC::forceReset() {
    digitalWrite(_pins.reset, LOW);
    delay(10);
    digitalWrite(_pins.reset, HIGH);
    delay(10);
}

int32_t SPICurrentADC::signExtend24(uint32_t raw) {
    if (raw & 0x800000) raw |= 0xFF000000;
    return (int32_t)raw;
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) per SBAS890D.
uint16_t SPICurrentADC::crc16Ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// CRC covers every word except the CRC word itself, over the same
// left-justified 24-bit-word byte stream actually clocked on the wire.
// NOTE: the CRC word's own left/right justification within its 24-bit word
// is inferred from the command-word convention, not yet confirmed against a
// live capture -- verify on the bench before trusting mismatches as real
// (see docs/ads131m04-current-sensor-design.md open question #6).
uint16_t SPICurrentADC::computeFrameCrc(const uint32_t resp[FRAME_WORDS]) const {
    uint8_t bytes[(FRAME_WORDS - 1) * WORD_BYTES];
    for (int w = 0; w < FRAME_WORDS - 1; w++) {
        bytes[w * 3 + 0] = (resp[w] >> 16) & 0xFF;
        bytes[w * 3 + 1] = (resp[w] >> 8) & 0xFF;
        bytes[w * 3 + 2] = resp[w] & 0xFF;
    }
    return crc16Ccitt(bytes, sizeof(bytes));
}

uint32_t SPICurrentADC::computeFramePeriodUs(uint8_t osrBits) {
    uint16_t osr = OSR_VALUES[osrBits & 0x7];
    float fDataHz = 4096000.0f / (float)osr;  // fMOD = fCLKIN/2 = 4.096MHz
    return (uint32_t)(1000000.0f / fDataHz);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool SPICurrentADC::begin() {
    pinMode(_pins.cs, OUTPUT);
    digitalWrite(_pins.cs, HIGH);
    pinMode(_pins.drdy, INPUT);

    // RESET#/SYNC# bodge wire (GPIO39 on the main board) -- open-drain,
    // idle HIGH releases the pin so the daughter board's own 100k pull-up
    // (R3) holds it at its normal idle state; LOW asserts a hardware reset.
    // See docs/ads131m04-current-sensor-design.md "RESET# bodge wire".
    pinMode(_pins.reset, OUTPUT_OPEN_DRAIN);
    digitalWrite(_pins.reset, HIGH);

    _spi.begin(_pins.sclk, _pins.dout, _pins.din, _pins.cs);
    delay(10);  // let the ADC finish its power-on reset before talking to it

    forceReset();

    uint32_t id = readRegister(REG_ID);
    uint32_t status = readRegister(REG_STATUS);
    _present = (id != 0);
    if (!_present) {
        Log.warn("SPIADC", "ADS131M04 not detected (ID=0x%06lX) -- current sensing unavailable",
                 (unsigned long)id);
        return false;
    }
    Log.info("SPIADC", "ADS131M04 detected: ID=0x%06lX STATUS=0x%06lX", (unsigned long)id, (unsigned long)status);

    // MODE: DRDY_SEL=01b (logic-OR of all enabled channels) on top of the
    // reset default -- all 4 channels need to be captured together anyway.
    writeRegister(REG_MODE, MODE_RESET_DEFAULT | (0x1 << 2));
    // CLOCK: configurable OSR, all other bits (channel enables, PWR) at
    // reset default.
    uint16_t clockVal = (uint16_t)((CLOCK_RESET_DEFAULT & ~(0x7 << 2)) | ((_osr & 0x7) << 2));
    writeRegister(REG_CLOCK, clockVal);

    _expectedFramePeriodUs = computeFramePeriodUs(_osr);
    _lastValidFrameMs = millis();

    _frameQueue = xQueueCreate(256, sizeof(RawFrame));
    if (_frameQueue == nullptr) {
        Log.error("SPIADC", "Failed to allocate frame queue");
        _present = false;
        return false;
    }

    // Acquisition task pinned to core 0 (away from the Arduino loopTask on
    // core 1), priority above loopTask's default (1) so it preempts
    // promptly for DRDY servicing, but well below WiFi/BT system tasks.
    // Priority/stack are a first-pass estimate -- bench-verify frame-drop
    // rate (getDroppedFrameCount()) under real WiFi/MQTT/dashboard load
    // before trusting RMS/PF accuracy (see plan's open risks).
    BaseType_t ok = xTaskCreatePinnedToCore(acquisitionTaskTrampoline, "spiAdcAcq", 4096, this, 3,
                                             &_acqTaskHandle, 0);
    if (ok != pdPASS) {
        Log.error("SPIADC", "Failed to create acquisition task");
        _present = false;
        return false;
    }

    attachInterruptArg(digitalPinToInterrupt(_pins.drdy), drdyISR, this, FALLING);

    return true;
}

void SPICurrentADC::attachChannel(uint8_t channel, CurrentSensor* sensor) {
    if (channel > 3) return;
    _channelSensor[channel] = sensor;
    if (sensor != nullptr) sensor->setSourceADC(this);
}

// ---------------------------------------------------------------------------
// Acquisition task + ISR (producer side)
// ---------------------------------------------------------------------------

void IRAM_ATTR SPICurrentADC::drdyISR(void* arg) {
    SPICurrentADC* self = static_cast<SPICurrentADC*>(arg);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(self->_acqTaskHandle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

void SPICurrentADC::acquisitionTaskTrampoline(void* arg) {
    static_cast<SPICurrentADC*>(arg)->acquisitionTaskLoop();
}

void SPICurrentADC::acquisitionTaskLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t resp[FRAME_WORDS];
        spiFrame(nullptr, 0, resp);
        uint64_t nowUs = (uint64_t)esp_timer_get_time();

        uint16_t computedCrc = computeFrameCrc(resp);
        uint16_t receivedCrc = (uint16_t)((resp[FRAME_WORDS - 1] >> 8) & 0xFFFF);
        if (computedCrc != receivedCrc) {
            _crcErrorCount++;
            continue;  // discard -- don't push a possibly-corrupt frame downstream
        }

        RawFrame frame;
        frame.timestampUs = nowUs;
        for (int ch = 0; ch < 4; ch++) frame.ch[ch] = signExtend24(resp[1 + ch]);

        _lastValidFrameMs = millis();

        if (xQueueSend(_frameQueue, &frame, 0) != pdTRUE) {
            _droppedFrameCount++;  // consumer (tick()) too slow this pass -- counted, not logged (would flood at 4kHz)
        }
    }
}

// ---------------------------------------------------------------------------
// Consumer side (tick(), main-loop context only -- no locking needed below)
// ---------------------------------------------------------------------------

void SPICurrentADC::tick() {
    RawFrame frame;
    while (_frameQueue != nullptr && xQueueReceive(_frameQueue, &frame, 0) == pdTRUE) {
        processFrame(frame);
    }
    checkWatchdog();
}

void SPICurrentADC::processFrame(const RawFrame& frame) {
    detectZeroCross(frame.ch[2], frame.timestampUs);  // AIN2 = ZX
    static const uint8_t kCurrentChannels[3] = {0, 1, 3};
    for (uint8_t ch : kCurrentChannels) {
        CurrentSensor* sensor = _channelSensor[ch];
        if (sensor != nullptr) sensor->pushSample(frame.ch[ch], frame.timestampUs);
    }
}

void SPICurrentADC::detectZeroCross(int32_t ain2, uint64_t timestampUs) {
    bool aboveNow = _zxAboveThreshold ? (ain2 > ZX_FALLING_THRESHOLD) : (ain2 > ZX_RISING_THRESHOLD);

    if (aboveNow && !_zxAboveThreshold && _zxPrevTimestampUs != 0) {
        // Rising edge crossed the threshold between the previous and
        // current sample -- linearly interpolate the crossing instant for
        // better phase resolution than snapping to the sample boundary.
        float frac = 0.0f;
        int32_t span = ain2 - _zxPrevRaw;
        if (span != 0) {
            frac = (float)(ZX_RISING_THRESHOLD - _zxPrevRaw) / (float)span;
            frac = constrain(frac, 0.0f, 1.0f);
        }
        uint64_t tCross = _zxPrevTimestampUs + (uint64_t)(frac * (float)(timestampUs - _zxPrevTimestampUs));

        if (_zxLastEdgeUs != 0) {
            float halfPeriodUs = (float)(tCross - _zxLastEdgeUs);
            // ZX pulses twice per 60Hz cycle -- line period is 2x the
            // pulse-to-pulse interval, not the interval itself.
            float freq = (halfPeriodUs > 0.0f) ? (1e6f / (2.0f * halfPeriodUs)) : 0.0f;
            if (freq >= 50.0f && freq <= 70.0f) {
                _lineHalfPeriodUs = halfPeriodUs;
                _lineFrequencyHz = freq;
                _lineFrequencyLastUpdateUs = tCross;
            }
            // else: reject as a missed/extra-pulse glitch, keep last good value
        }
        _zxLastEdgeUs = tCross;
        _zxEdgeCount++;
    }

    _zxAboveThreshold = aboveNow;
    _zxPrevRaw = ain2;
    _zxPrevTimestampUs = timestampUs;
}

bool SPICurrentADC::isLineFrequencyValid() const {
    if (_zxEdgeCount < 2) return false;
    uint64_t now = (uint64_t)esp_timer_get_time();
    return (now - _lineFrequencyLastUpdateUs) < 1000000ULL;  // stale after 1s without a fresh edge
}

void SPICurrentADC::checkWatchdog() {
    if (!_present) return;

    // _lastValidFrameMs is written by the acquisition task (a different
    // FreeRTOS task/core) and read here from the main-loop task. Each is
    // individually an atomic 32-bit access, but the *pair* isn't: if the
    // acquisition task writes a fresher timestamp in the gap between this
    // millis() call and the _lastValidFrameMs load, "now" can be older than
    // "last valid", and now - lastValid wraps to a huge value instead of
    // going negative -- a real bug hit live on the bench (logged as a
    // ~4294967293ms stall from a genuinely healthy ADC). Clamp instead of
    // trusting the wrap: a "last valid" that looks like it's in the future
    // just means a frame arrived essentially concurrently, i.e. elapsed 0.
    uint32_t now = millis();
    uint32_t lastValid = _lastValidFrameMs;
    uint32_t elapsedMs = (now >= lastValid) ? (now - lastValid) : 0;
    // Floor at 3000ms. Raised twice now, live on the bench: a 20ms floor
    // false-tripped at 21ms during a WiFi reconnect, a 200ms floor
    // false-tripped at 201ms during another one -- WiFi (re)connection's
    // CPU impact on core 0 is more variable than a small fixed floor can
    // safely clear, and guessing a slightly bigger fixed number just moves
    // where the same failure mode reappears. Since the fault latches
    // permanently with no auto-retry, a too-tight floor means current
    // sensing can go dead on effectively any boot/reconnect. This watchdog
    // is a data-integrity flag, not a fast-acting safety protection --
    // there is no real cost to being patient. 3s clears any plausible
    // WiFi-driven scheduling delay with large margin while still being
    // enormously faster than "never detected" (the original BUG-015 gap).
    uint32_t timeoutMs = (5 * _expectedFramePeriodUs) / 1000;
    if (timeoutMs < 3000) timeoutMs = 3000;

    if (elapsedMs > timeoutMs) {
        if (!_commsFault) {
            _commsFault = true;
            Log.error("SPIADC", "DRDY# stalled -- no valid frame in %lums (BUG-015-class fault)", elapsedMs);
            for (int ch = 0; ch < 4; ch++) {
                if (_channelSensor[ch] != nullptr) _channelSensor[ch]->markStale();
            }
        }
        // No auto-retry by design -- stays faulted until reboot.
    }
}
