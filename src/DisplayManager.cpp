#include "DisplayManager.h"
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "GoodmanHP.h"
#include "Logger.h"

extern uint8_t getCpuLoadCore0();
extern uint8_t getCpuLoadCore1();
extern bool _apModeActive;
extern String _apPassword;

DisplayManager::DisplayManager(Scheduler* ts)
    : _ts(ts), _hp(nullptr), _display(nullptr),
      _tFlipPage(nullptr), _tUpdateDisplay(nullptr),
      _currentPage(0), _pageIntervalSec(10),
      _enabled(true), _connected(false)
{
}

DisplayManager::~DisplayManager() {
    delete _display;
    delete _tFlipPage;
    delete _tUpdateDisplay;
}

bool DisplayManager::begin(uint8_t addr) {
    // Probe I2C address
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
        Log.info("OLED", "SSD1306 not found at 0x%02X", addr);
        return false;
    }

    _display = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    if (!_display->begin(SSD1306_SWITCHCAPVCC, addr)) {
        Log.error("OLED", "SSD1306 init failed");
        delete _display;
        _display = nullptr;
        return false;
    }

    _connected = true;
    _display->clearDisplay();
    _display->setTextColor(SSD1306_WHITE);
    _display->setCursor(0, 0);
    _display->setTextSize(2);
    _display->println(F("GoodmanHP"));
    _display->setTextSize(1);
    _display->println(F("Starting..."));
    _display->display();

    _tFlipPage = new Task(_pageIntervalSec * TASK_SECOND, TASK_FOREVER, [this]() {
        this->flipPage();
    }, _ts, false);

    _tUpdateDisplay = new Task(TASK_SECOND, TASK_FOREVER, [this]() {
        this->updateDisplay();
    }, _ts, false);

    if (_enabled) {
        _tFlipPage->enable();
        _tUpdateDisplay->enable();
    }

    Log.info("OLED", "SSD1306 128x64 initialized at 0x%02X", addr);
    return true;
}

void DisplayManager::setPageInterval(uint32_t sec) {
    if (sec < 3) sec = 3;
    if (sec > 60) sec = 60;
    _pageIntervalSec = sec;
    if (_tFlipPage) {
        _tFlipPage->setInterval(_pageIntervalSec * TASK_SECOND);
    }
}

void DisplayManager::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!_connected || !_display) return;

    if (_enabled) {
        _display->ssd1306_command(SSD1306_DISPLAYON);
        if (_tFlipPage) _tFlipPage->enable();
        if (_tUpdateDisplay) _tUpdateDisplay->enable();
    } else {
        if (_tFlipPage) _tFlipPage->disable();
        if (_tUpdateDisplay) _tUpdateDisplay->disable();
        _display->clearDisplay();
        _display->display();
        _display->ssd1306_command(SSD1306_DISPLAYOFF);
    }
}

void DisplayManager::flipPage() {
    _currentPage = (_currentPage + 1) % NUM_PAGES;
    // Hold AP credentials screen 3x longer so password is readable
    if (_currentPage == 0 && _apModeActive && _tFlipPage) {
        _tFlipPage->setInterval(_pageIntervalSec * 3 * TASK_SECOND);
    } else if (_tFlipPage) {
        _tFlipPage->setInterval(_pageIntervalSec * TASK_SECOND);
    }
}

void DisplayManager::updateDisplay() {
    if (!_display || !_connected || !_hp) return;

    _display->clearDisplay();

    switch (_currentPage) {
        case 0: drawPageStatus(); break;
        case 1: drawPageTemps(); break;
        case 2: drawPageIO(); break;
        case 3: drawPageSystem(); break;
        case 4: drawPageProtections(); break;
    }

    drawPageIndicator();
    _display->display();
}

void DisplayManager::drawPageStatus() {
    if (_apModeActive) {
        _display->setTextSize(2);
        _display->setCursor(0, 0);
        _display->print(F("AP MODE"));
        _display->drawLine(0, 20, 127, 20, SSD1306_WHITE);
        _display->setTextSize(1);
        _display->setCursor(0, 26);
        _display->print(F("SSID: GoodmanHP"));
        _display->setCursor(0, 38);
        _display->print(F("Pass: "));
        _display->print(_apPassword);
        _display->setCursor(0, 50);
        _display->print(F("IP: 192.168.4.1"));
        return;
    }

    // State banner in large text
    _display->setTextSize(2);
    _display->setCursor(0, 0);
    _display->print(_hp->getStateString());

    // Separator line
    _display->drawLine(0, 20, 127, 20, SSD1306_WHITE);

    _display->setTextSize(1);

    // WiFi IP
    _display->setCursor(0, 26);
    if (WiFi.isConnected()) {
        _display->print(F("IP: "));
        _display->print(WiFi.localIP().toString());
    } else {
        _display->print(F("WiFi: disconnected"));
    }

    // Uptime
    _display->setCursor(0, 38);
    uint32_t sec = millis() / 1000;
    uint32_t d = sec / 86400; sec %= 86400;
    uint32_t h = sec / 3600; sec %= 3600;
    uint32_t m = sec / 60; sec %= 60;
    if (d > 0) {
        _display->printf("%lud %luh %lum", d, h, m);
    } else {
        _display->printf("%luh %lum %lus", h, m, sec);
    }

    // Heat runtime
    _display->setCursor(0, 50);
    uint32_t rtMin = _hp->getHeatRuntimeMs() / 60000UL;
    _display->printf("Heat RT: %lu min", rtMin);
}

void DisplayManager::drawPageTemps() {
    _display->setTextSize(1);
    _display->setCursor(0, 0);
    _display->println(F("-- Temperatures --"));

    struct { const char* abbr; const char* key; } sensors[] = {
        {"COMP", "COMPRESSOR_TEMP"},
        {"SUCT", "SUCTION_TEMP"},
        {"AMB",  "AMBIENT_TEMP"},
        {"COND", "CONDENSER_TEMP"},
        {"LIQ",  "LIQUID_TEMP"}
    };

    TempSensorMap& temps = _hp->getTempSensorMap();
    int y = 12;
    for (int i = 0; i < 5; i++) {
        _display->setCursor(0, y);
        _display->print(sensors[i].abbr);
        _display->print(": ");
        auto it = temps.find(sensors[i].key);
        if (it != temps.end() && it->second != nullptr && it->second->isValid()) {
            _display->printf("%.1f F", it->second->getValue());
        } else {
            _display->print("---");
        }
        y += 10;
    }
}

void DisplayManager::drawPageIO() {
    _display->setTextSize(1);
    _display->setCursor(0, 0);
    _display->println(F("-- Inputs/Outputs --"));

    // Inputs on left side
    _display->setCursor(0, 12);
    _display->println(F("Inputs:"));
    const char* inputNames[] = {"LPS", "DFT", "Y", "O"};
    int y = 22;
    for (int i = 0; i < 4; i++) {
        InputPin* pin = _hp->getInput(inputNames[i]);
        _display->setCursor(2, y);
        _display->print(inputNames[i]);
        _display->print(": ");
        _display->print(pin && pin->isActive() ? "HI" : "LO");
        y += 10;
    }

    // Outputs on right side
    _display->setCursor(68, 12);
    _display->println(F("Outputs:"));
    const char* outputNames[] = {"FAN", "CNT", "W", "RV"};
    y = 22;
    for (int i = 0; i < 4; i++) {
        OutPin* pin = _hp->getOutput(outputNames[i]);
        _display->setCursor(70, y);
        _display->print(outputNames[i]);
        _display->print(": ");
        _display->print(pin && pin->isPinOn() ? "ON" : "OFF");
        y += 10;
    }
}

void DisplayManager::drawPageSystem() {
    _display->setTextSize(1);
    _display->setCursor(0, 0);
    _display->println(F("-- System --"));

    _display->setCursor(0, 14);
    _display->printf("Heap: %lu KB", ESP.getFreeHeap() / 1024);

    _display->setCursor(0, 26);
    _display->printf("CPU0: %d%%  CPU1: %d%%", getCpuLoadCore0(), getCpuLoadCore1());

    _display->setCursor(0, 38);
    _display->printf("PSRAM: %.1f MB", ESP.getFreePsram() / (1024.0f * 1024.0f));

    _display->setCursor(0, 50);
    if (WiFi.isConnected()) {
        _display->printf("RSSI: %d dBm", WiFi.RSSI());
    } else {
        _display->print(F("WiFi: N/A"));
    }
}

void DisplayManager::drawPageProtections() {
    _display->setTextSize(1);
    _display->setCursor(0, 0);
    _display->println(F("-- Protections --"));

    int y = 12;
    bool any = false;

    if (_hp->isLPSFaultActive()) {
        _display->setCursor(0, y); _display->print(F("* LPS Fault")); y += 10; any = true;
    }
    if (_hp->isLowTempActive()) {
        _display->setCursor(0, y); _display->print(F("* Low Ambient Temp")); y += 10; any = true;
    }
    if (_hp->isRvFailActive()) {
        _display->setCursor(0, y); _display->print(F("* RV Fail (latched)")); y += 10; any = true;
    }
    if (_hp->isSoftwareDefrostActive()) {
        _display->setCursor(0, y); _display->print(F("* Defrost Active")); y += 10; any = true;
    }
    if (_hp->isShortCycleProtectionActive()) {
        _display->setCursor(0, y); _display->print(F("* SC Protect")); y += 10; any = true;
    }
    if (_hp->isCompressorOverTempActive()) {
        _display->setCursor(0, y); _display->print(F("* Comp Over Temp")); y += 10; any = true;
    }
    if (_hp->isSuctionLowTempActive()) {
        _display->setCursor(0, y); _display->print(F("* Suction Low Temp")); y += 10; any = true;
    }
    if (_hp->isStartupLockoutActive()) {
        _display->setCursor(0, y);
        _display->printf("* Startup (%lus)", _hp->getStartupLockoutRemainingMs() / 1000);
        y += 10; any = true;
    }

    if (!any) {
        _display->setCursor(0, 28);
        _display->print(F("No active protections"));
    }
}

void DisplayManager::drawPageIndicator() {
    // Small dots at bottom-right showing current page
    int startX = SCREEN_WIDTH - (NUM_PAGES * 6) - 2;
    int y = SCREEN_HEIGHT - 4;
    for (uint8_t i = 0; i < NUM_PAGES; i++) {
        int x = startX + i * 6;
        if (i == _currentPage) {
            _display->fillCircle(x, y, 2, SSD1306_WHITE);
        } else {
            _display->drawCircle(x, y, 2, SSD1306_WHITE);
        }
    }
}
