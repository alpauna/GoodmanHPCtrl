#include "OutPin.h"

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
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), true, _percentOn, origPercent)){
      return;
    }
  }
  _changeOnTick = millis();

  if(!_pwm){
    digitalWrite(_pin, _inverse ? LOW : HIGH);
  }else{
    analogWrite(_pin, percent_to_byte_float(percent));
  }
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
  //initPin();

  _tsk = new Task (delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);

  //_tsk = new Task (delay, TASK_ONCE, Callback, ts, false);
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
  //initPin();
  _tsk = new Task (delay, TASK_ONCE, [this]() {
      this->Callback();
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
  //initPin();
  _tsk = new Task (delay, TASK_ONCE, [this]() {
      this->Callback();
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
  //initPin();
  _tsk = new Task (delay, TASK_ONCE, [this]() {
      this->Callback();
  }, ts, false);
}

void OutPin::Callback(){
  _onCount++;
  Serial.printf("new Percent: %lf,\n", _percentOn);
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
bool OutPin::isOn() { return _percentOn > 0.0;}

void OutPin::initPin(){
  if(!_openDrain){
    pinMode(_pin, OUTPUT);
  }else{
    pinMode(_pin, OUTPUT_OPEN_DRAIN);
  }
  if(!_pwm){
    if(_percentOn > 0.0){
      turnOn();
    }else{
      turnOff();
    }
  }else{
    turnOnPercent(_percentOn);
  }
}

void OutPin::turnOff(){
  float origPercent = _percentOn;
  _percentOn = 0.0;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      return;
    }
  }
  _changeOffTick = millis();
  _tsk->disable();
  digitalWrite(_pin, _inverse ? HIGH : LOW);
}

void OutPin::turnOn(){
  float origPercent = _percentOn;
  _percentOn = 100.0;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      return;
    }
  }
  _tsk->enableIfNot();
  _tsk->restartDelayed();
  digitalWrite(_pin, _inverse ? LOW : HIGH);
}

void OutPin::turnOn(float percent){
  float origPercent = _percentOn;
  _percentOn = percent;
  if(_clbk != nullptr){
    if(!_clbk(this, isOn(), false, _percentOn, origPercent)){
      return;
    }
  }
  _tsk->enableIfNot();
  _tsk->restartDelayed();
}
