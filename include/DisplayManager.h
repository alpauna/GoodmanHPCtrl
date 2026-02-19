#ifndef DISPLAYMANAGER_H
#define DISPLAYMANAGER_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>

class GoodmanHP;
class Adafruit_SSD1306;

class DisplayManager {
  public:
    static const uint8_t NUM_PAGES = 5;
    static const uint8_t SCREEN_WIDTH = 128;
    static const uint8_t SCREEN_HEIGHT = 64;

    DisplayManager(Scheduler* ts);
    ~DisplayManager();

    bool begin(uint8_t addr = 0x3C);
    void setController(GoodmanHP* hp) { _hp = hp; }
    void setPageInterval(uint32_t sec);
    uint32_t getPageInterval() const { return _pageIntervalSec; }
    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled; }
    bool isConnected() const { return _connected; }

  private:
    Scheduler* _ts;
    GoodmanHP* _hp;
    Adafruit_SSD1306* _display;
    Task* _tFlipPage;
    Task* _tUpdateDisplay;

    uint8_t _currentPage;
    uint32_t _pageIntervalSec;
    bool _enabled;
    bool _connected;

    void flipPage();
    void updateDisplay();
    void drawPageStatus();
    void drawPageTemps();
    void drawPageIO();
    void drawPageSystem();
    void drawPageProtections();
    void drawPageIndicator();
};

#endif
