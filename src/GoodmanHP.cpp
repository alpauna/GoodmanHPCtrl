#include "GoodmanHP.h"
#include "Logger.h"

GoodmanHP::GoodmanHP(Scheduler *ts, InputPin *lps, InputPin *dft, InputPin *y, InputPin *o, OutPin *cnt)
    : _ts(ts)
    , _lps(lps)
    , _dft(dft)
    , _y(y)
    , _o(o)
    , _cnt(cnt)
    , _state(State::OFF)
    , _yActiveStartTick(0)
    , _yWasActive(false)
    , _cntActivated(false)
{
    _tskUpdate = new Task(500, TASK_FOREVER, [this]() {
        this->update();
    }, ts, false);
}

void GoodmanHP::begin() {
    _tskUpdate->enable();
    Log.info("HP", "GoodmanHP controller started");
}

void GoodmanHP::update() {
    checkYAndActivateCNT();
    updateState();
}

void GoodmanHP::checkYAndActivateCNT() {
    bool yActive = _y->isActive();

    if (yActive && !_yWasActive) {
        // Y just became active - record start time
        _yActiveStartTick = millis();
        _yWasActive = true;
        Log.info("HP", "Y input activated, starting 30s timer");
    } else if (!yActive && _yWasActive) {
        // Y just became inactive - reset
        _yWasActive = false;
        _yActiveStartTick = 0;
        if (_cntActivated) {
            _cnt->turnOff();
            _cntActivated = false;
            Log.info("HP", "Y input deactivated, CNT turned off");
        }
    } else if (yActive && _yWasActive && !_cntActivated) {
        // Y still active - check if 30 seconds have passed
        uint32_t elapsed = millis() - _yActiveStartTick;
        if (elapsed >= Y_DELAY_MS) {
            _cnt->turnOn();
            _cntActivated = true;
            Log.info("HP", "Y active for 30s, CNT activated");
        }
    }
}

void GoodmanHP::updateState() {
    State newState = State::OFF;

    if (_dft->isActive()) {
        newState = State::DEFROST;
    } else if (_y->isActive() && _o->isActive()) {
        newState = State::HEAT;
    } else if (_y->isActive()) {
        newState = State::COOL;
    }

    if (newState != _state) {
        Log.info("HP", "State changed: %s -> %s", getStateString(),
                 newState == State::OFF ? "OFF" :
                 newState == State::COOL ? "COOL" :
                 newState == State::HEAT ? "HEAT" : "DEFROST");
        _state = newState;
    }
}

GoodmanHP::State GoodmanHP::getState() {
    return _state;
}

const char* GoodmanHP::getStateString() {
    switch (_state) {
        case State::OFF: return "OFF";
        case State::COOL: return "COOL";
        case State::HEAT: return "HEAT";
        case State::DEFROST: return "DEFROST";
        default: return "UNKNOWN";
    }
}

bool GoodmanHP::isYActive() {
    return _y->isActive();
}

bool GoodmanHP::isOActive() {
    return _o->isActive();
}

bool GoodmanHP::isLPSActive() {
    return _lps->isActive();
}

bool GoodmanHP::isDFTActive() {
    return _dft->isActive();
}

uint32_t GoodmanHP::getYActiveTime() {
    if (_yWasActive) {
        return millis() - _yActiveStartTick;
    }
    return 0;
}
