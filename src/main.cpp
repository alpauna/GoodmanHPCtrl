#include <Arduino.h>
#include <ArxContainer.h>
#include <OneWire.h>
#include <WiFiServer.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include "SdFat.h"
#include <AsyncMqttClient.h>
#include <StringStream.h>
#include "sdios.h"
#include <TaskSchedulerDeclarations.h>
#include "CircularBuffer.hpp"
#include "esp32-hal-psram.h"
#include "Logger.h"
#include "OutPin.h"
#include "InputPin.h"
#include "GoodmanHP.h"
#include "Config.h"
#include "WebHandler.h"
bool psramInited = false;
bool serialInited = false;

// Override global new and delete operators
void* operator new(size_t size) {
  if(!serialInited){
    Serial.begin(115200);
    serialInited = true;
  }
  if(!psramInited){
    if( psramInit()){
      psramInited = true;
      Serial.println("PSRAM intiated!!!");
    }else{
      Serial.println("PSRAM Failed!!!"); 
    }
  }
  if(psramInited){
    void* ptr = ps_malloc(size); // Use ps_malloc
    if (!ptr) {
        // Handle allocation failure if necessary
        #ifdef ARDUINO
        ets_printf("ps_malloc failed\n");
        #else
        abort();
        #endif
    }
    return ptr;
  }
  void* ptr = malloc(size); // Use ps_malloc
  if (!ptr) {
      // Handle allocation failure if necessary
      #ifdef ARDUINO
      ets_printf("malloc failed\n");
      #else
      abort();
      #endif
  } 
  return ptr;
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

// ... (also override new[] and delete[] for arrays)
void* operator new[](size_t size) {
    return operator new(size);
}

void operator delete[](void* ptr) noexcept {
    operator delete(ptr);
}

const char compile_date[] = __DATE__ " " __TIME__;
ArduinoOutStream cout(Serial);
IPAddress _MQTT_HOST = IPAddress(192, 168, 0, 46);
const char* _filename = "/config.txt";
const float MB_MULTIPLIER = 1.0/(1024.0*1024.0);
const u_int16_t n_elements = 2000;


#if CIRCULAR_BUFFER_INT_SAFE
#else
#error "Needs to set CIRCULAR_BUFFER_INT_SAFE"
#endif

#if ASYNC_TCP_SSL_ENABLED
#define MQTT_SECURE true
#define MQTT_SERVER_FINGERPRINT {0x6f, 0xa2, 0x20, 0x02, 0xe9, 0x7e, 0x99, 0x2f, 0xc5, 0xdb, 0x3d, 0xbe, 0xac, 0x48, 0x51, 0x5b, 0x5d, 0x47, 0xa7, 0x99}
#define MQTT_PORT 8883
#else
u_int16_t _MQTT_PORT = 1883;
String _MQTT_USER = "debian";
String _MQTT_PASSWORD = "";
#endif

String _WIFI_SSID = "";
String _WIFI_PASSWORD = "";




#if defined (ARDUINO_ARCH_AVR)
#include <MemoryFree.h>
#elif defined(__arm__)
extern "C" char* sbrk(int incr);
static int freeMemory() {
  char top = 't';
  return &top - reinterpret_cast<char*>(sbrk(0));
}
#elif defined (ARDUINO_ARCH_ESP8266) || defined (ARDUINO_ARCH_ESP32)
int freeMemory() { return ESP.getFreeHeap();}
#else
//  Supply your own freeMemory method
int freeMemory() { return 0;}
#endif

//#define _TASK_MICRO_RES

// Config instance for SD card and configuration management
Config config;


std::map<String, InputPin*> _isrEvent;

// Scheduler
Scheduler ts, hts;
u_int32_t _nextIdlePrintTime = 0;

u_int32_t _idleLoopCount = 0;
u_int32_t _workLoopCount = 0;
volatile bool InISR, InitialPinStateSet, FinalPinSetState;

u_int32_t _wifiStartMillis = 0;

u_long runTimeStart;
u_long currentRuntime;  

// put function declarations here:
#if defined (BOARD_ESP32_S3_WROOM)
const u_int8_t _lpsPin = GPIO_NUM_15;
const u_int8_t _dftPin = GPIO_NUM_16;
const u_int8_t _yPin = GPIO_NUM_17;
const u_int8_t _oPin = GPIO_NUM_18;
const u_int8_t _fanPin = GPIO_NUM_4;
const u_int8_t _CNTPin = GPIO_NUM_5;
const u_int8_t _WPin = GPIO_NUM_6;
const u_int8_t _RVPin = GPIO_NUM_7;
#elif  defined (BOARD_ESP32_ROVER)
const u_int8_t _dftPin = GPIO_NUM_16;
const u_int8_t _yPin = GPIO_NUM_17;
const u_int8_t _oPin = GPIO_NUM_18;
#else
#error "BUILD_ENV_NAME NOT RECOGNIZED"
#endif
const u_int8_t ONE_WIRE_BUS = GPIO_NUM_21;

// ProjectInfo is defined in Config.h

void tempSensorUpdateCallback(TempSensor *sensor);
void tempSensorChangeCallback(TempSensor *sensor);


void onInput(InputPin *pin);
bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent);

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
std::map<String, InputPin* > activePins;

// GoodmanHP controller instance - contains input and output pin maps
GoodmanHP hpController(&ts);
WebHandler webHandler(80, &ts, &hpController);

OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);


AsyncMqttClient _mqttClient;

typedef enum AC_STATE { OFF, COOL, HEAT, DEFROST } ACState;
static String AC_STATE_STR[] = {"OFF", "COOL", "HEAT", "DEFROST"};
static String BOOL_STR[] = {"TRUE", "FALSE"};

ProjectInfo proj = {
  "Goodman Heatpump Control",
  compile_date,
  "Control Goodman heatpump including defrost mode.",
  "",
  false,
  50 * 1024 * 1024,  // maxLogSize: 50MB default
  10,                 // maxOldLogCount: 10 files default
  0                   // heatRuntimeAccumulatedMs: restored from config
};


ACState _acState = OFF;
bool OnReadInputsEnable();
void OnReadInputsDisable();
bool OnInputChangeEnable();
void OnInputChangeDisable();
void OnRunTimeUpdate();
void getTempSensors();
void getTempSensors(TempSensorMap& tempMap);
void connectToMqtt();
bool onWifiWaitEnable();
void onWifiWaitDisable();
bool onMqttWaitEnable();
void onMqttDisable();
bool CheckTickTime(InputPin *pin);
void onCheckInputQueue();

void onInput(InputPin *pin){
  cout << "Input Pin name:" << pin->getName() << " Value:" << pin->getValue() << endl;
}

bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent){
  cout << "Output pin:" << pin->getName() << " On:" << on << endl; 
  cout << "Last On Tick:" << pin->getOnTick() << endl;
  cout << "Last Off Tick:" << pin->getOffTick() << endl;
  return true;
}

Task tWaitOnWiFi(TASK_SECOND, 60, [](){
  cout << ".";
}, &ts, false, onWifiWaitEnable, onWifiWaitDisable);


Task tRuntime(TASK_MINUTE, TASK_FOREVER, &OnRunTimeUpdate, &ts, false);

Task _tGetInputs(500 * TASK_MILLISECOND, TASK_FOREVER, &onCheckInputQueue, &ts, false);

// Save heat runtime to SD card every 5 minutes
void onSaveRuntime();
Task tSaveRuntime(5 * TASK_MINUTE, TASK_FOREVER, &onSaveRuntime, &ts, false);

Task tConnectMQQT(TASK_SECOND, TASK_FOREVER, [](){
  if(_mqttClient.connected()){
    tConnectMQQT.disable();
    return;
  }
  connectToMqtt();
}, &ts, false, onMqttWaitEnable, onMqttDisable);



/**
 * NOTE: ISR logic should be kept simple for both timings and prevent strange core panics.
 * Love this ESP32 ISR as it supports arguments. This allowed me to pass a pointer to
 * ISR function with my input pin structure to help track pin state. 
 */
void IRAM_ATTR inputISRChange(void *arg) {
  InputPin* pinInfo = static_cast<InputPin*>(arg);
  if(pinInfo == nullptr) {
    InISR = false;
    return;
  }
  pinInfo->setPrevValue();
  pinInfo->changedNow();
  if( _isrEvent.find(pinInfo->getName()) == _isrEvent.end())
  {
    _isrEvent[pinInfo->getName()] = pinInfo;
  }
}
bool CheckTickTime(InputPin *pin){
  uint32_t curTime = millis();
  if(pin == nullptr) return false;
  if(curTime >= pin->changedAtTick() + 50 || pin->getPreValue() != pin->getValue()){
    return true;
  }
  else
  { 
    return false;
  }
}

bool onWifiWaitEnable(){
  if(WiFi.isConnected()){
    return false;
  }
  _mqttClient.disconnect();
  return true;
}   

void onWifiWaitDisable(){
  cout << endl;
  cout << "WiFi IP:" << WiFi.localIP() << endl;
  tConnectMQQT.enableDelayed();
}

bool onMqttWaitEnable(){
  cout << "Wifi Down so wait on MQTT." << endl;
  return true;
}
void onMqttDisable(){
  cout << "MQTT connected and disabled MQTT start." << endl;
}
void connectToMqtt() {
  Log.info("MQTT", "Connecting to MQTT...");
  _mqttClient.connect();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Log.warn("MQTT", "Disconnected from MQTT (reason: %d)", (int)reason);

  if (reason == AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT) {
    Log.error("MQTT", "Bad server fingerprint");
  }

  if (WiFi.isConnected()) {
    tConnectMQQT.enableDelayed();
  }
}
void onMqttConnect(bool sessionPresent) {
  Log.info("MQTT", "Connected to MQTT (session present: %s)", sessionPresent ? "yes" : "no");
  Log.info("MQTT", "IP: %s", WiFi.localIP().toString().c_str());
  tConnectMQQT.disable();
}


void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  Serial.println("Publish received.");
  Serial.print("  topic: ");
  Serial.println(topic);
  Serial.print("  qos: ");
  Serial.println(properties.qos);
  Serial.print("  dup: ");
  Serial.println(properties.dup);
  Serial.print("  retain: ");
  Serial.println(properties.retain);
  Serial.print("  len: ");
  Serial.println(len);
  Serial.print("  index: ");
  Serial.println(index);
  Serial.print("  total: ");
  Serial.println(total);
}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void printAddress(DeviceAddress temp);

void wifiConnected(){
  cout << "WiFi Connected within " << millis() - _wifiStartMillis << " ms." << endl;
  tConnectMQQT.enableDelayed();
  
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info){
  switch(event){
    case SYSTEM_EVENT_STA_GOT_IP:
      tWaitOnWiFi.disable();
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      _wifiStartMillis = millis();
      tWaitOnWiFi.enableDelayed();
      tConnectMQQT.disable();
      Log.warn("WIFI", "WiFi lost connection");
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      wifiConnected();
      break;
  }
} 

void connectToWifi() {
  WiFi.begin(_WIFI_SSID, _WIFI_PASSWORD);
}

void setupMQTT(){
  _mqttClient.onConnect(onMqttConnect);
  _mqttClient.onDisconnect(onMqttDisconnect);
  _mqttClient.onSubscribe(onMqttSubscribe);
  _mqttClient.onUnsubscribe(onMqttUnsubscribe);
  _mqttClient.onMessage(onMqttMessage);
  _mqttClient.onPublish(onMqttPublish);
  _mqttClient.setServer(_MQTT_HOST, _MQTT_PORT);
  _mqttClient.setCredentials(_MQTT_USER.c_str(), _MQTT_PASSWORD.c_str());
}


void onCheckInputQueue(){
  for(auto& m : _isrEvent){
    while(InISR){
      vTaskDelay(pdMS_TO_TICKS(1));
      yield();
    }
    InputPin * pin = m.second;
    pin->verifiedAt();
   
    //auto isInActiveMap = activePins.find(pin->getName());
    if(pin->isActive()){
      cout << "Activating pin: " << pin->getName() << endl;
      //remove from activePins as it is not active any longer.
      pin->activeNow();
      pin->getTask()->restartDelayed( pin->getTask()->getInterval() );
    }else{
      cout << "Deactvated pin: " << pin->getName() << endl;
      pin->getTask()->disable();
      pin->inactiveNow();
      pin->fireCallback();
    }
    _isrEvent.erase(m.first);
  }  
}

unsigned char * acc_data_all;
void setup() {
  if(!serialInited){
    Serial.begin(115200);
    serialInited = true;
  }
  if(!psramInited){
    if( psramInit()){
      psramInited = true;
      Serial.println("PSRAM intiated!!!");
    }else{
      Serial.println("PSRAM Failed!!!"); 
    }
  }
  
  acc_data_all = (unsigned char *) ps_malloc (n_elements * sizeof (unsigned char));
  sprintf((char *)acc_data_all, "Test %d", millis());
  sensors.begin();

  // Initialize config and load from SD card
  config.setTempSensorDiscoveryCallback([](TempSensorMap& tempMap) {
    getTempSensors(tempMap);
  });

  if(config.initSDCard()){
    TempSensorMap& tempSensors = hpController.getTempSensorMap();
    if(config.openConfigFile(_filename, tempSensors, proj)){
      config.loadTempConfig(_filename, tempSensors, proj);
      // Update global variables from config
      _WIFI_SSID = config.getWifiSSID();
      _WIFI_PASSWORD = config.getWifiPassword();
      _MQTT_HOST = config.getMqttHost();
      _MQTT_PORT = config.getMqttPort();
      _MQTT_USER = config.getMqttUser();
      _MQTT_PASSWORD = config.getMqttPassword();
      // Restore accumulated heat runtime from config
      hpController.setHeatRuntimeMs(proj.heatRuntimeAccumulatedMs);
    }
  }
  cout << "SD Card is read." << endl;
  WiFi.onEvent(onWiFiEvent);
  connectToWifi();

  webHandler.begin();

  setupMQTT();

  // Initialize Logger
  Log.setLevel(Logger::LOG_INFO);
  Log.setMqttClient(&_mqttClient, "goodman/log");
  Log.setLogFile(config.getSd(), "/log.txt", proj.maxLogSize, proj.maxOldLogCount);
  Log.info("MAIN", "Logger initialized");

  // Add input pins to GoodmanHP controller
  hpController.addInput("LPS", new InputPin(&ts, 3000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _lpsPin, "LPS", "LPS", onInput));
  hpController.addInput("DFT", new InputPin(&ts, 3000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _dftPin, "DFT", "DFT", onInput));
  hpController.addInput("Y", new InputPin(&ts, 3000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _yPin, "Y", "OT-NO", onInput));
  hpController.addInput("O", new InputPin(&ts, 3000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _oPin, "O", "OT-NC", onInput));

  // Add output pins to GoodmanHP controller
  hpController.addOutput("FAN", new OutPin(&ts, 0, _fanPin, "FAN", "FAN", onOutpin));
  hpController.addOutput("CNT", new OutPin(&ts, 3000, _CNTPin, "CNT", "CNT", onOutpin));
  hpController.addOutput("W", new OutPin(&ts, 0, _WPin, "W", "W", onOutpin));
  hpController.addOutput("RV", new OutPin(&ts, 0, _RVPin, "RV", "RV", onOutpin));


  // Start GoodmanHP controller
  hpController.setDallasTemperature(&sensors);
  hpController.begin();

  tRuntime.enable();
  _tGetInputs.enable();
  tSaveRuntime.enable();

  Log.info("MAIN", "Starting Main Loop");
}

void tempSensorUpdateCallback(TempSensor *sensor){
  sensor->update(&sensors);
}

void tempSensorChangeCallback(TempSensor *sensor){
  Serial.print(sensor->getDescription());
  sensor->isValid() ? Serial.print(" Temp Updated: ") : Serial.print(" Temp Invalid: ");
  Serial.print("Temp: ");
  Serial.print(sensor->getValue());
  Serial.print("F Previous Temp: ");
  Serial.print(sensor->getPrevious());
  Serial.println("F");
}

void getTempSensors(TempSensorMap& tempMap)
{
  // Clear existing sensors if running twice
  hpController.clearTempSensors();
  TempSensor::discoverSensors(&sensors, tempMap, tempSensorUpdateCallback, tempSensorChangeCallback);
}

void getTempSensors()
{
  getTempSensors(hpController.getTempSensorMap());
}


void OnRunTimeUpdate(){
  currentRuntime = millis();
  Serial.printf("Current runtime: %ld\r\n", currentRuntime);
}

void onSaveRuntime(){
  uint32_t runtimeMs = hpController.getHeatRuntimeMs();
  if (runtimeMs != proj.heatRuntimeAccumulatedMs) {
    proj.heatRuntimeAccumulatedMs = runtimeMs;
    if (config.updateRuntime(_filename, runtimeMs)) {
      Log.debug("MAIN", "Heat runtime saved: %lu ms", runtimeMs);
    }
  }
}

bool OnReadInputsEnable(){
  InitialPinStateSet = false;
  FinalPinSetState = false;
  return true;
}
void OnReadInputsDisable(){
  InitialPinStateSet = true;
}

bool OnInputChangeEnable(){
  FinalPinSetState = false;
  return true;
}
void OnInputChangeDisable(){
  FinalPinSetState = true;
}



/**Run in defrost mode. Do sanity check to make sure inputs are in the correct state to be here.
 * 
 */
int getSignalQuality() {
  int32_t rssi = WiFi.RSSI();
  // Convert RSSI to signal quality percentage
  // RSSI range: -100dBm to -30dBm
  if (rssi <= -100) return 0;
  if (rssi >= -30) return 100;
  return (rssi + 100) * 100 / 70;
}

void printIdleStatus() {
  if (millis() <= _nextIdlePrintTime) {
    return;
  }
  
  // Stats for outpin activation.
  for (auto& out : hpController.getOutputMap()) {
    cout << "Out Pin: " << out.first << " On Count: " << out.second->getOnCount() << endl;
  }
  digitalWrite(_WPin, HIGH);
  _nextIdlePrintTime = millis() + 10000;
  Serial.print(": Idle count:");
  Serial.print(_idleLoopCount);
  Serial.print("\tWC: ");
  Serial.println(_workLoopCount);

  if (!WiFi.isConnected()) {
    int retry = 0;
    while (!WiFi.reconnect() && (retry++) < 10) {
      Serial.printf(": Reconnect failed: %d\r\n", retry);
      yield();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } else {
    Serial.printf("WIFI Signal: %d (%d DBm) Memory %lf\r\n ", getSignalQuality(), (int32_t)WiFi.RSSI(), ESP.getFreePsram() * MB_MULTIPLIER);
  }
}

void loop() {
  if (webHandler.shouldReboot()) {
    Serial.println("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP.restart();
  }

  bool bIdle = ts.execute();
  if (bIdle) {
    _idleLoopCount++;
    printIdleStatus();
  } else {
    _workLoopCount++;
  }
}
