#ifndef INPUTPIN_H
#define INPUTPIN_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>

enum class InputResistorType{
  NONE,
  IT_PULLUP,
  IT_PULLDOWN
};

enum class InputPinType{
  IT_DIGITAL,
  IT_ANALOG
};

class InputPin;
typedef void (*InputPinCallback)(InputPin *pin);

class InputPin{
  private:
    InputPinType _it;
    Task *_tsk;
    int8_t _pin;
    String _name;
    String _boardPin;
    InputResistorType _pullupType;
    uint16_t _preValue;
    uint16_t _value;
    bool _confirmedActive;    // Debounced/validated pin state
    int8_t _pendingState;     // -1=none, 0=pending inactive, 1=pending active
    u_int32_t _changedAtTick;
    u_int32_t _verifiedAtTick;
    u_int32_t _lastActiveTick;
    u_int32_t _lastInactiveTick;
    InputPinCallback _clbk;
  protected:
    float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
    void Callback();
  public:
    InputPin(Scheduler *ts, uint32_t delay, InputResistorType pullup, InputPinType it, int8_t pin, String name, String boardPin, InputPinCallback clbk);
    void initPin();
    uint8_t getPin();
    String getName();
    Task * getTask();
    float getPinState(float in_min, float in_max, float out_min, float out_max);
    uint16_t getPinState();
    uint16_t setPrevValue();
    uint16_t syncValue();
    uint16_t setValue();
    uint16_t getPreValue();
    uint16_t getValue();
    float mapValue(float in_min, float in_max, float out_min, float out_max);
    uint32_t changedAtTick();
    uint32_t verifiedAt();
    uint32_t lastActiveAt();
    uint32_t lastInactiveAt();
    bool isActive();
    bool readLiveState();     // Read live GPIO state (bypasses debounce)
    void setPendingState(int8_t state);  // Set expected state for validation (-1/0/1)
    int8_t getPendingState();
    void setDelay(uint32_t ms);
    uint32_t getDelay();
    void changedNow();
    void verifiedNow();
    void activeNow();
    void inactiveNow();
    void fireCallback();
};

#endif
