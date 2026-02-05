#ifndef GOODMANHP_H
#define GOODMANHP_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>
#include "InputPin.h"
#include "OutPin.h"

class GoodmanHP {
  public:
    enum class State { OFF, COOL, HEAT, DEFROST };

    GoodmanHP(Scheduler *ts, InputPin *lps, InputPin *dft, InputPin *y, InputPin *o, OutPin *cnt);

    void begin();
    void update();

    State getState();
    const char* getStateString();

    bool isYActive();
    bool isOActive();
    bool isLPSActive();
    bool isDFTActive();

    uint32_t getYActiveTime();

  private:
    Scheduler *_ts;
    Task *_tskUpdate;

    InputPin *_lps;
    InputPin *_dft;
    InputPin *_y;
    InputPin *_o;

    OutPin *_cnt;

    State _state;
    uint32_t _yActiveStartTick;
    bool _yWasActive;
    bool _cntActivated;

    static const uint32_t Y_DELAY_MS = 30000;  // 30 seconds

    void checkYAndActivateCNT();
    void updateState();
};

#endif
