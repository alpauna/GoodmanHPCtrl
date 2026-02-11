#ifndef OUTPIN_H
#define OUTPIN_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>

class OutPin;
typedef bool (*OutputPinCallback)(OutPin *pin, bool on, bool inCallback, float &newPercent, float lastPercent);
typedef bool (*RuntimeCallback)(OutPin *pin, uint32_t onDuration);

class OutPin
{
  private:
    Scheduler *_ts;
    Task *_tsk;
    Task *_tskRuntime;
    int8_t _pin;
    String _name;
    String _boardPin;
    bool _inverse;
    bool _openDrain;
    bool _changed;
    bool _pwm;
    float _percentOn;
    uint32_t _onCount = 0;
    uint32_t _pwmFreq;
    uint32_t _changeOnTick;
    uint32_t _changeOffTick;
    OutputPinCallback _clbk = nullptr;
    RuntimeCallback _runtimeClbk = nullptr;
    uint32_t _runtimeInterval = 1000;
    bool _transitioning = false;
    bool _lastPwmHigh = false;
  protected:
    uint8_t percent_to_byte_float(float percent);
    void turnOnPercent(float percent);
  public:
    OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, OutputPinCallback clbk);
    OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, float percentOn, OutputPinCallback clbk);
    OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, bool pwm, OutputPinCallback clbk);
    OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, bool inverse, bool openDrain, bool pwm, float percentOn, uint32_t freq, OutputPinCallback clbk);
    void Callback();
    String getName();
    String getBoardPin();
    int8_t getPin();
    void updateDelay(u_int32_t delay);
    bool getChanged();
    bool getPWM();
    void resetChanged();
    uint32_t getOnTick();
    uint32_t getOffTick();
    uint32_t getOnCount();
    void resetOnCount();
    float getOnPercent();
    Task * getTask();
    bool isOn();
    bool isPinOn();
    void initPin();
    void turnOff();
    void turnOn();
    void turnOn(float percent);
    void setRuntimeCallback(RuntimeCallback clbk, uint32_t intervalMs = 1000);
    void runtimeCallback();
};

#endif
