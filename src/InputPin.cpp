#include "InputPin.h"
#include "Logger.h"

float InputPin::mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void InputPin::Callback(){
  _verifiedAtTick = millis();

  // Re-read live GPIO to validate the pin is still in the expected state
  bool liveState = (getPinState() > 0);

  if (_pendingState >= 0) {
    bool expectedActive = (_pendingState == 1);
    if (liveState != expectedActive) {
      // Pin state doesn't match what triggered the delay — false trigger, discard
      Log.warn("InputPin", "%s false trigger discarded (expected %s, got %s after %lums delay)",
               _name.c_str(), expectedActive ? "active" : "inactive",
               liveState ? "active" : "inactive", _tsk ? _tsk->getInterval() : 0);
      _pendingState = -1;
      return;
    }
  }

  // Pin validated — update confirmed state
  _confirmedActive = liveState;
  _pendingState = -1;

  if (liveState) {
    _lastActiveTick = millis();
  } else {
    _lastInactiveTick = millis();
  }

  if(_clbk){
    _clbk(this);
  }
}

InputPin::InputPin(Scheduler *ts, uint32_t delay, InputResistorType pullup, InputPinType it, int8_t pin, String name, String boardPin, InputPinCallback clbk){
  _it = it;
  _pullupType = pullup;
  _pin = pin;
  _name = name;
  _boardPin = boardPin;
  _clbk = clbk;
  _confirmedActive = false;
  _pendingState = -1;
  _tsk = new Task (delay, TASK_ONCE, [this]() {
    Callback();
  }, ts, false);
  //initPin();
}

void InputPin::initPin(){
  switch(_pullupType) {
    case InputResistorType::IT_PULLUP :
      pinMode(_pin, INPUT_PULLUP);
      break;
    case InputResistorType::IT_PULLDOWN :
      pinMode(_pin, INPUT_PULLDOWN);
      break;
    default:
      pinMode(_pin, INPUT);
  }
  setPrevValue();
  setValue();
  _confirmedActive = (_value > 0);
  _pendingState = -1;
  changedNow();
}

uint8_t InputPin::getPin() { return _pin; }
String InputPin::getName() { return _name; }
Task * InputPin::getTask(){ return _tsk; }

float InputPin::getPinState(float in_min, float in_max, float out_min, float out_max){
  if(_it == InputPinType::IT_ANALOG){
    return mapFloat(analogRead(_pin), in_min, in_max, out_min, out_max);
  }
  return 0.0;
}

uint16_t InputPin::getPinState(){
  if(_it == InputPinType::IT_ANALOG){
    return analogRead(_pin);
  }
  return digitalRead(_pin);
}

uint16_t InputPin::setPrevValue(){ _preValue = getPinState(); return _preValue; }
uint16_t InputPin::syncValue() { _value = _preValue;  return _value; }
uint16_t InputPin::setValue(){ _value = getPinState(); return _value; }
uint16_t InputPin::getPreValue() { return _preValue; }
uint16_t InputPin::getValue() { return _value; }

float InputPin::mapValue(float in_min, float in_max, float out_min, float out_max) {
  return mapFloat(_value, in_min, in_max, out_min, out_max);
}

uint32_t InputPin::changedAtTick() { return _changedAtTick;}
uint32_t InputPin::verifiedAt() { return _verifiedAtTick;}
uint32_t InputPin::lastActiveAt() { return _lastActiveTick; }
uint32_t InputPin::lastInactiveAt() { return _lastInactiveTick; }

bool InputPin::isActive() {
  return _confirmedActive;
}

bool InputPin::readLiveState() {
  return getPinState() > 0;
}

void InputPin::setPendingState(int8_t state) {
  _pendingState = state;
}

int8_t InputPin::getPendingState() {
  return _pendingState;
}

void InputPin::setDelay(uint32_t ms) {
  if (_tsk) _tsk->setInterval(ms);
}

uint32_t InputPin::getDelay() {
  return _tsk ? _tsk->getInterval() : 0;
}

void InputPin::changedNow() { _changedAtTick = millis(); }
void InputPin::verifiedNow() { _verifiedAtTick = millis(); }
void InputPin::activeNow() {_lastActiveTick = millis(); }
void InputPin::inactiveNow() {_lastInactiveTick = millis();}
void InputPin::fireCallback() {if(_clbk) _clbk(this); }
