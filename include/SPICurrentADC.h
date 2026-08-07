#ifndef SPICURRENTADC_H
#define SPICURRENTADC_H

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class CurrentSensor;

// Driver for the ADS131M04 4-channel simultaneous-sampling SPI ADC used for
// CT-clamp current sensing (AIN0/AIN1/AIN3) plus a zero-cross line-frequency
// reference (AIN2/ZX). Protocol/pin/register details ported from the proven
// bring-up sketch (bringup/ads131m04/main.cpp) and
// docs/ads131m04-current-sensor-design.md.
//
// DRDY# fires at up to ~4kHz (250us period at the reset-default OSR=1024) —
// too fast to service reliably from a cooperative TaskScheduler task sharing
// a core with WiFi/AsyncTCP. Acquisition instead runs on a dedicated,
// core-0-pinned FreeRTOS task woken by the DRDY# ISR (which does nothing but
// notify the task — no SPI calls from ISR context). Samples cross to the
// consumer (tick(), called from the main Arduino loop) via a FreeRTOS queue.
class SPICurrentADC {
  public:
    struct Pins {
        int8_t din;    // SPI MOSI -> ADC DIN
        int8_t dout;   // SPI MISO <- ADC DOUT
        int8_t sclk;   // SPI clock
        int8_t cs;     // Chip select, active low
        int8_t drdy;   // Data-ready interrupt, active low
        int8_t reset;  // RESET#/SYNC# bodge wire, open-drain, active low
    };

    // osr: ADS131M04 CLOCK register OSR[2:0] value (0-7 -> 128..16256 SPS).
    // Default 3 = reset-default 1024 (4000 SPS) — best margin for catching
    // the narrow ZX pulse; raise only if bench testing shows frame drops.
    explicit SPICurrentADC(const Pins& pins, uint8_t osr = 3, uint32_t spiHz = 4000000);
    ~SPICurrentADC();

    // Must be called before begin() to take effect (OSR is written to the
    // CLOCK register once, during begin()). Not live-adjustable afterward.
    void setOsr(uint8_t osr) { _osr = osr & 0x7; }

    // Pin setup, RESET# pulse, ID/STATUS check, register config, starts the
    // acquisition task + DRDY ISR. Returns false (and leaves the object
    // inert) if the ADC doesn't respond — callers must handle this the same
    // defensive way the old ads1115Ready check did, boot continues either way.
    bool begin();

    bool isPresent() const { return _present; }

    // True once a DRDY-stall watchdog trip has fired (BUG-015 analogue) —
    // stays true until reboot, no auto-retry by design.
    bool isCommsFault() const { return _commsFault; }
    uint32_t getCrcErrorCount() const { return _crcErrorCount; }
    uint32_t getDroppedFrameCount() const { return _droppedFrameCount; }

    // channel: 0=AIN0 (compressor), 1=AIN1 (fan), 3=AIN3 (crankcase).
    // Channel 2 (AIN2/ZX) is reserved internally for zero-cross detection.
    void attachChannel(uint8_t channel, CurrentSensor* sensor);

    // Drains all currently-queued frames and dispatches samples to attached
    // CurrentSensors, then runs the DRDY watchdog check. Call every loop
    // pass — not gated behind TaskScheduler, so consumer throughput isn't
    // tied to ts's scheduling granularity.
    void tick();

    // Line frequency, derived from AIN2/ZX pulse-to-pulse timing (the ZX
    // signal pulses twice per 60Hz cycle, see design doc "Main-board
    // interconnect" -- frequency = 1 / (2 x pulse interval)).
    float getLineFrequencyHz() const { return _lineFrequencyHz; }
    bool isLineFrequencyValid() const;

    // Consumed by CurrentSensor::pushSample() to compute phase angle / PF
    // against the shared ZX reference. Only meaningful while
    // isLineFrequencyValid() is true.
    uint64_t getLastZxEdgeUs() const { return _zxLastEdgeUs; }
    float getLinePeriodUs() const { return 2.0f * _lineHalfPeriodUs; }
    uint32_t getZxEdgeCount() const { return _zxEdgeCount; }

    // ADS131M04, gain=1, +/-1.2V full scale over +/-2^23 codes.
    static constexpr float LSB_VOLTS = 1.2f / 8388608.0f;

  private:
    static const int WORD_BYTES = 3;
    static const int FRAME_WORDS = 6;  // command/response + CH0-3 + CRC, 4-ch/no-extra-CRC-cfg default

    struct RawFrame {
        int32_t ch[4];
        uint64_t timestampUs;
    };

    // --- SPI/register primitives (ported from bringup/ads131m04/main.cpp) ---
    // outWords/numOutWords are clocked out on DIN starting at word 0; any
    // remaining words in the frame are clocked with DIN=0 (don't-care).
    // resp[] captures DOUT for every word. A NULL-command frame (numOutWords
    // == 0) is both a plain data-read cycle and the second half of the
    // RREG/WREG two-frame pattern (response lands in the *next* frame).
    void spiFrame(const uint16_t* outWords, uint8_t numOutWords, uint32_t resp[FRAME_WORDS]);
    uint32_t readRegister(uint8_t addr);
    void writeRegister(uint8_t addr, uint16_t value);
    static uint16_t cmdRREG(uint8_t addr, uint8_t count);
    static uint16_t cmdWREG(uint8_t addr, uint8_t count);
    void forceReset();
    static int32_t signExtend24(uint32_t raw);
    static uint16_t crc16Ccitt(const uint8_t* data, size_t len);
    uint16_t computeFrameCrc(const uint32_t resp[FRAME_WORDS]) const;
    static uint32_t computeFramePeriodUs(uint8_t osrBits);

    // --- Acquisition task / ISR ---
    static void acquisitionTaskTrampoline(void* arg);
    void acquisitionTaskLoop();
    static void IRAM_ATTR drdyISR(void* arg);

    // --- Consumer-side (tick(), main-loop context only) ---
    void processFrame(const RawFrame& frame);
    void detectZeroCross(int32_t ain2, uint64_t timestampUs);
    void checkWatchdog();

    Pins _pins;
    uint8_t _osr;
    uint32_t _spiHz;
    SPIClass _spi;
    SPISettings _spiSettings;

    volatile bool _present;
    volatile bool _commsFault;
    uint32_t _crcErrorCount;
    uint32_t _droppedFrameCount;
    volatile uint32_t _lastValidFrameMs;  // millis(), set by acq task, read by tick() -- 32-bit, atomic
    uint32_t _expectedFramePeriodUs;

    CurrentSensor* _channelSensor[4];  // index 0/1/3 used; 2 (ZX) always null here

    QueueHandle_t _frameQueue;
    TaskHandle_t _acqTaskHandle;

    // Zero-cross / frequency detection state (AIN2/ZX) -- main-loop-context
    // only (touched exclusively inside tick()/processFrame()), no locking
    // needed. ZX is an inverted-logic pulse (baseline low, brief pulses to
    // near full-scale at each mains zero-crossing -- confirmed live on the
    // bench, not a symmetric square wave), so a Schmitt-trigger threshold
    // pair is used instead of a naive sign-change check.
    static const int32_t ZX_RISING_THRESHOLD = 500000;
    static const int32_t ZX_FALLING_THRESHOLD = 100000;
    bool _zxAboveThreshold;
    int32_t _zxPrevRaw;
    uint64_t _zxPrevTimestampUs;
    uint64_t _zxLastEdgeUs;
    uint32_t _zxEdgeCount;
    float _lineHalfPeriodUs;
    float _lineFrequencyHz;
    uint64_t _lineFrequencyLastUpdateUs;
};

#endif
