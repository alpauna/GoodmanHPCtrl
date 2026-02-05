#ifndef OUTPIN_H
#define OUTPIN_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>

class OutPin;
typedef bool (*OutputPinCallback)(OutPin *pin, bool on, bool inCallback, float &newPercent, float lastPercent);

class OutPin
{
  private:
    Scheduler *_ts;
    Task *_tsk;
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
    void initPin();
    void turnOff();
    void turnOn();
    void turnOn(float percent);
};

#endif
