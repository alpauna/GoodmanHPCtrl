// ADS131M04 minimal SPI bring-up sketch.
//
// Purpose: confirm the daughter board is wired correctly and the ADC
// actually talks over SPI, before writing the real SPICurrentADC driver.
// Does two independent, unrelated checks:
//   1. RREG-read the ID and STATUS registers and print the raw response.
//   2. Watch DRDY# and report whether/how fast it's toggling.
// See docs/ads131m04-current-sensor-design.md for the full protocol
// writeup and pin mapping this is based on.
//
// Build/upload as its own PlatformIO environment, separate from the main
// firmware:
//   pio run -e ads131m04_bringup -t upload -t monitor

#include <Arduino.h>
#include <SPI.h>

// Pin mapping confirmed against the live schematic (H3 7-pin header), see
// "Main-board interconnect" in the design doc. Not shared with any other
// bus on this board.
static const int PIN_DIN  = 2;   // ESP32 MOSI -> ADC DIN
static const int PIN_DOUT = 42;  // ESP32 MISO <- ADC DOUT (also /MTMS; fine as GPIO, JTAG unused here)
static const int PIN_SCLK = 1;   // ESP32 SCK  -> ADC SCLK
static const int PIN_CS   = 45;  // ESP32 GPIO -> ADC CS# (active low, manual toggle)
static const int PIN_DRDY = 46;  // ADC DRDY# -> ESP32 GPIO (active low, push-pull, no pull-up needed)

// Conservative bring-up clock. ADS131M04 supports much faster (see
// datasheet timing table); no reason to push it until basic comms work.
static const uint32_t SPI_HZ = 1000000;

// SPI mode 1: CPOL=0, CPHA=1 (confirmed against SBAS890D).
static SPISettings spiSettings(SPI_HZ, MSBFIRST, SPI_MODE1);

// Reset default word length is 24-bit; every "word" below is 3 bytes.
static const int WORD_BYTES = 3;

// A full frame at the reset default (4 channels enabled, CRC output
// disabled) is 6 words: command/response + CH0..CH3 + CRC. RREG/WREG
// responses land in word 1 of the *next* frame, not the one the command
// was sent in -- see "SPI protocol" in the design doc.
static const int FRAME_WORDS = 6;

// Command word builders (Table 8-11, SBAS890D).
static const uint16_t CMD_NULL = 0x0000;

static uint16_t cmdRREG(uint8_t addr, uint8_t count) {
    // 101a aaaa annn nnnn: opcode(3) | address(6) | (count-1)(7)
    return (0x5 << 13) | ((addr & 0x3F) << 7) | ((count - 1) & 0x7F);
}

// Clocks one full frame. word0 is the command/response word (DIN on the
// way out, previous command's response on the way back); the remaining
// FRAME_WORDS-1 words are clocked with DIN=0 (don't-care per the protocol)
// and their DOUT captured into resp[1..FRAME_WORDS-1].
static void spiFrame(uint16_t word0, uint32_t resp[FRAME_WORDS]) {
    digitalWrite(PIN_CS, LOW);
    SPI.beginTransaction(spiSettings);

    uint32_t out = (uint32_t)word0 << 8;  // 24-bit word, data left-justified
    uint32_t in = 0;
    for (int b = 0; b < WORD_BYTES; b++) {
        uint8_t txByte = (out >> (16 - 8 * b)) & 0xFF;
        uint8_t rxByte = SPI.transfer(txByte);
        in = (in << 8) | rxByte;
    }
    resp[0] = in;

    for (int w = 1; w < FRAME_WORDS; w++) {
        uint32_t wIn = 0;
        for (int b = 0; b < WORD_BYTES; b++) {
            uint8_t rxByte = SPI.transfer(0x00);
            wIn = (wIn << 8) | rxByte;
        }
        resp[w] = wIn;
    }

    SPI.endTransaction();
    digitalWrite(PIN_CS, HIGH);
    delayMicroseconds(2);  // CS high time between frames
}

// Read a single register: send RREG this frame, NULL the next frame, and
// the register value comes back as word0 of that second frame's response.
static uint32_t readRegister(uint8_t addr) {
    uint32_t resp[FRAME_WORDS];
    spiFrame(cmdRREG(addr, 1), resp);
    spiFrame(CMD_NULL, resp);
    return resp[0];
}

// ADC data is 24-bit two's complement, left-justified in the 24-bit word
// (same left-justified convention as commands -- see datasheet section
// referenced above). Sign-extend from bit 23 to get a real signed value.
static int32_t signExtend24(uint32_t raw) {
    if (raw & 0x800000) raw |= 0xFF000000;
    return (int32_t)raw;
}

static uint32_t drdyToggleCount = 0;
static int lastDrdyState = -1;
static uint32_t framesThisSecond = 0;
static uint32_t rawCh[4] = {0, 0, 0, 0};
static int32_t ain2Min = INT32_MAX;
static int32_t ain2Max = INT32_MIN;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\nADS131M04 bring-up sketch");

    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_DRDY, INPUT);

    SPI.begin(PIN_SCLK, PIN_DOUT, PIN_DIN, PIN_CS);

    delay(10);  // let the ADC finish its power-on reset before talking to it

    uint32_t id = readRegister(0x00);
    uint32_t status = readRegister(0x01);
    Serial.printf("ID register:     0x%06lX\n", (unsigned long)id);
    Serial.printf("STATUS register: 0x%06lX\n", (unsigned long)status);
    Serial.println("Compare against SBAS890D Table 8-12 -- ID should read a");
    Serial.println("nonzero, stable device/revision code; STATUS's low bits");
    Serial.println("are per-channel DRDY flags and should not read all 1s");
    Serial.println("with nothing connected on AIN0-3/ZX.");

    lastDrdyState = digitalRead(PIN_DRDY);
    Serial.println("Watching DRDY# for toggling...");
}

void loop() {
    int s = digitalRead(PIN_DRDY);
    if (s != lastDrdyState) {
        drdyToggleCount++;
        lastDrdyState = s;
    }

    // DRDY_FMT defaults to level mode (low until read) -- it will assert once
    // after the first conversion and then just sit low forever unless
    // something actually services it by clocking a data frame. Do that here
    // with plain NULL frames so DRDY# behaves the way a real driver would
    // make it behave, and grab CH0 along the way as a second data point.
    uint32_t resp[FRAME_WORDS];
    spiFrame(CMD_NULL, resp);
    framesThisSecond++;
    for (int ch = 0; ch < 4; ch++) rawCh[ch] = resp[1 + ch];

    // AIN2 carries the ZX zero-cross reference -- per the design doc this is
    // a 3.3V *logic* square wave into AIN2P with AIN2N tied to AGND, not a
    // signal that swings through 0V. So the thing to look for is min/max
    // swing within a window (does it actually toggle between two levels),
    // not sign changes -- it may never go negative at all, and that's fine.
    int32_t ain2 = signExtend24(rawCh[2]);
    if (ain2 < ain2Min) ain2Min = ain2;
    if (ain2 > ain2Max) ain2Max = ain2;

    static uint32_t lastReport = 0;
    uint32_t now = millis();
    if (now - lastReport >= 1000) {
        lastReport = now;
        // Free-running conversions at the reset-default OSR (1024) give a
        // data rate in the hundreds of Hz -- expect toggles/sec in that
        // ballpark, not 0 (dead bus/no clock) and not absurdly high (noise).
        Serial.printf("DRDY# toggles: %lu/s (pin now %s) | frames read: %lu/s\n",
                      (unsigned long)drdyToggleCount, s ? "HIGH" : "LOW",
                      (unsigned long)framesThisSecond);
        Serial.printf("  CH0(comp): %8ld  CH1(fan): %8ld  CH3(heater): %8ld\n",
                      (long)signExtend24(rawCh[0]), (long)signExtend24(rawCh[1]),
                      (long)signExtend24(rawCh[3]));
        Serial.printf("  CH2(ZX) this window: min=%8ld max=%8ld swing=%8ld\n",
                      (long)ain2Min, (long)ain2Max, (long)(ain2Max - ain2Min));
        drdyToggleCount = 0;
        framesThisSecond = 0;
        ain2Min = INT32_MAX;
        ain2Max = INT32_MIN;
    }
}
