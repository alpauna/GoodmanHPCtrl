#include "OutPin.h"
#include "Logger.h"

uint8_t OutPin::percent_to_byte_float(float percent) {
  // Ensure the input is within the valid range [0.0, 100.0]
  if (percent < 0.0) percent = 0.0;
  if (percent > 100.0) percent = 100.0;

  // Scale the percentage (0-100) to the byte range (0-255)
  // Formula: (percent / 100.0) * 255.0
  float value = (percent / 100.0) * 255.0;

  // Cast to unsigned char. The cast truncates the value,
  // which is often the desired behavior for color values.
  return (uint8_t)value;
}

void OutPin::turnOnPercent(float percent){
  float origPercent = _percentOn;
  _percentOn = percent;
  _transitioning = true;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), true, _percentOn, origPercent)){
      _transitioning = false;
      return;
    }
  }
  _changeOnTick = millis();

  if(!_pwm){
    if(percent > 0.0){
      digitalWrite(_pin, _inverse ? LOW : HIGH);
    }else{
      digitalWrite(_pin, _inverse ? HIGH : LOW);
    }
  }else{
    analogWrite(_pin, percent_to_byte_float(percent));
  }
  _transitioning = false;
}

OutPin::OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, OutputPinCallback clbk) {
  _ts = ts;
  _pin = pin;
  _name = name;
  _boardPin = boardPin;
  _inverse = false;
  _openDrain = false;
  _pwm = false;
  _pwmFreq = 1000;
  _clbk = clbk;
  _percentOn = 0.0;

  _tsk = new Task(delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);

  _tskRuntime = new Task(_runtimeInterval, TASK_FOREVER, [this]() {
      this->runtimeCallback();
  }, ts, false);
}

OutPin::OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, float percentOn, OutputPinCallback clbk){
  _ts = ts;
  _pin = pin;
  _name = name;
  _boardPin = boardPin;
  _inverse = false;
  _openDrain = false;
  _pwm = false;
  _pwmFreq = 1000;
  _clbk = clbk;
  _percentOn = percentOn;

  _tsk = new Task(delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);

  _tskRuntime = new Task(_runtimeInterval, TASK_FOREVER, [this]() {
      this->runtimeCallback();
  }, ts, false);
}

OutPin::OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, bool pwm, OutputPinCallback clbk){
  _ts = ts;
  _pin = pin;
  _name = name;
  _boardPin = boardPin;
  _inverse = false;
  _openDrain = false;
  _pwm = pwm;
  _pwmFreq = 1000;
  _clbk = clbk;
  _percentOn = 0.0;

  _tsk = new Task(delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);

  _tskRuntime = new Task(_runtimeInterval, TASK_FOREVER, [this]() {
      this->runtimeCallback();
  }, ts, false);
}

OutPin::OutPin(Scheduler *ts, uint32_t delay, int8_t pin, String name, String boardPin, bool inverse, bool openDrain, bool pwm, float percentOn, uint32_t freq, OutputPinCallback clbk){
  _ts = ts;
  _pin = pin;
  _name = name;
  _boardPin = boardPin;
  _inverse = inverse;
  _openDrain = openDrain;
  _pwm = pwm;
  _pwmFreq = freq;
  _clbk = clbk;
  _percentOn = percentOn;
  if(pwm){
      analogWriteFrequency(_pwmFreq);
  }

  _tsk = new Task(delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);

  _tskRuntime = new Task(_runtimeInterval, TASK_FOREVER, [this]() {
      this->runtimeCallback();
  }, ts, false);
}

void OutPin::Callback(){
  _onCount++;
  turnOnPercent(_percentOn);
}

String OutPin::getName() {return _name;}
String OutPin::getBoardPin() {return _boardPin;}
int8_t OutPin::getPin(){ return _pin;}

void OutPin::updateDelay(u_int32_t delay){
  _tsk->setInterval(delay);
}

bool OutPin::getChanged() {return _changed;}
bool OutPin::getPWM() {return _pwm; }
void OutPin::resetChanged() { _changed = false; }
uint32_t OutPin::getOnTick() { return _changeOnTick; }
uint32_t OutPin::getOffTick() { return _changeOffTick; }
uint32_t OutPin::getOnCount() { return _onCount; }
void OutPin::resetOnCount() { _onCount = 0; }
float OutPin::getOnPercent() {return _percentOn;}
Task * OutPin::getTask() {return _tsk;}
bool OutPin::isOn() {
  bool softwareOn = _percentOn > 0.0;
  if (_transitioning) return softwareOn;
  bool hardwareOn = isPinOn();
  if (softwareOn != hardwareOn) {
    Log.warn("OutPin", "%s state mismatch: software=%s hardware=%s, correcting to hardware state",
             _name.c_str(), softwareOn ? "ON" : "OFF", hardwareOn ? "ON" : "OFF");
    _percentOn = hardwareOn ? 100.0f : 0.0f;
    return hardwareOn;
  }
  return softwareOn;
}
bool OutPin::isPinOn() {
  bool pinHigh;
  if (!_pwm) {
    pinHigh = digitalRead(_pin);
  } else {
    // ESP32 ADC: 0-4095. Hysteresis: must reach 800 to go high, must drop below 400 to go low.
    int reading = analogRead(_pin);
    pinHigh = _lastPwmHigh ? (reading >= 400) : (reading >= 800);
    _lastPwmHigh = pinHigh;
  }
  return _inverse ? !pinHigh : pinHigh;
}

void OutPin::initPin(){
  if(!_openDrain){
    pinMode(_pin, OUTPUT);
  }else{
    pinMode(_pin, OUTPUT_OPEN_DRAIN);
  }
  // Immediately drive pin to known OFF state before any callback logic
  digitalWrite(_pin, _inverse ? HIGH : LOW);
  _percentOn = 0.0;
  _changeOffTick = millis();
}

void OutPin::turnOff(){
  float origPercent = _percentOn;
  _percentOn = 0.0;
  _transitioning = true;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      _transitioning = false;
      return;
    }
  }
  _changeOffTick = millis();
  _tsk->disable();
  _tskRuntime->disable();
  digitalWrite(_pin, _inverse ? HIGH : LOW);
  _transitioning = false;
}

void OutPin::turnOn(){
  float origPercent = _percentOn;
  _percentOn = 100.0;
  _transitioning = true;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      _transitioning = false;
      return;
    }
  }
  _tsk->enableIfNot();
  _tsk->restartDelayed();
  if(_runtimeClbk != nullptr){
    _tskRuntime->enableIfNot();
    _tskRuntime->restartDelayed();
  }
  digitalWrite(_pin, _inverse ? LOW : HIGH);
  _transitioning = false;
}

void OutPin::turnOn(float percent){
  float origPercent = _percentOn;
  _percentOn = percent;
  _transitioning = true;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      _transitioning = false;
      return;
    }
  }
  _tsk->enableIfNot();
  _tsk->restartDelayed();
  if(_runtimeClbk != nullptr){
    _tskRuntime->enableIfNot();
    _tskRuntime->restartDelayed();
  }
  _transitioning = false;
}

void OutPin::setRuntimeCallback(RuntimeCallback clbk, uint32_t intervalMs){
  _runtimeClbk = clbk;
  _runtimeInterval = intervalMs;
  _tskRuntime->setInterval(_runtimeInterval);
}

void OutPin::runtimeCallback(){
  if(_runtimeClbk == nullptr || !isOn()){
    _tskRuntime->disable();
    return;
  }
  uint32_t onDuration = millis() - _changeOnTick;
  bool shouldContinue = _runtimeClbk(this, onDuration);
  if(!shouldContinue){
    _tskRuntime->disable();
  }
}
