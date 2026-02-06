#include <Arduino.h>
#include <ArxContainer.h>
#include <OneWire.h>
#include <WiFiServer.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include "SdFat.h"
#include <ESPAsyncWebServer.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <AsyncTCP.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>
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

// NTP Configuration
const char* _ntpServer1 = "pool.ntp.org";
const char* _ntpServer2 = "time.nist.gov";
const long _gmtOffset_sec = 0;        // UTC offset in seconds (adjust for your timezone)
const int _daylightOffset_sec = 0;    // Daylight saving offset in seconds
bool _ntpSynced = false;




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

#define SPI_SPEED SD_SCK_MHZ(SD_SPI_SPEED)
//------------------------------------------------------------------------------
#if SD_FAT_TYPE == 0
SdFat sd;
File file;
#elif SD_FAT_TYPE == 1
SdFat32 sd;
File32 file;
#elif SD_FAT_TYPE == 2
SdExFat sd;
ExFile file;
#elif SD_FAT_TYPE == 3
SdFs sd;
FsFile _configFile;
#else  // SD_FAT_TYPE
#error Invalid SD_FAT_TYPE
#endif  // SD_FAT_TYPE


std::map<String, InputPin*> _isrEvent;

AsyncWebServer server(80);


AsyncWebSocket wes("/ws"); // access at ws://[esp ip]/ws
AsyncEventSource events("/events"); // event source (Server-Sent events)

// Scheduler
Scheduler ts, hts;
u_int32_t _nextIdlePrintTime = 0;
u_int32_t updateTotalSize, updateCurrentSize;

u_int32_t _idleLoopCount = 0;
u_int32_t _workLoopCount = 0;
volatile bool InISR, InitialPinStateSet, FinalPinSetState;

bool updateInProgress;

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

struct CurrentTemp;
typedef void (*CurrentTempCallback)(CurrentTemp *temp);
typedef std::map<String, CurrentTemp *> TempMap;
TempMap _tempSens;
struct CurrentTemp{
  String description;
  uint8_t *deviceAddress;
  float Value; 
  float Previous;
  bool Valid;
  CurrentTempCallback onCurrTempUpdate;
  CurrentTempCallback onCurrTempChanged;
};
struct ProjectInfo{
  String name;
  String createdOnDate;
  String description;
  String encrypt;
  bool encrpytped;
};

void currentTempCallback(CurrentTemp *temp);
void currentTempChangeCallback(CurrentTemp *temp);


void onInput(InputPin *pin);
bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent);

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
std::map<String, InputPin* > activePins;

// GoodmanHP controller instance - contains input and output pin maps
GoodmanHP hpController(&ts); 

OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);


bool _shouldReboot;
AsyncMqttClient _mqttClient;

typedef enum AC_STATE { OFF, COOL, HEAT, DEFROST } ACState;
static String AC_STATE_STR[] = {"OFF", "COOL", "HEAT", "DEFROST"};
static String BOOL_STR[] = {"TRUE", "FALSE"};

ProjectInfo proj = {"Goodman Heatpump Control", compile_date, "Control Goodman heatpump including defrost mode.", "", false };


ACState _acState = OFF;
bool OnReadInputsEnable();
void OnReadInputsDisable();
bool OnInputChangeEnable();
void OnInputChangeDisable();
void OnRunTimeUpdate();
void getDeviceAddress(uint8_t *devAddr, String  temp);
String getStrDeviceAddress(DeviceAddress temp);
void webAsyncFunctions();
void getTempSensors();
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len);
void connectToMqtt();
bool onWifiWaitEnable();
void onWifiWaitDisable();
bool onMqttWaitEnable();
void onMqttDisable();
bool saveConfiguration(const char* filename, TempMap& config, ProjectInfo& proj);
void clearConfig(TempMap &config);
bool CheckTickTime(InputPin *pin);
void onCheckInputQueue();

void onInput(InputPin *pin){

  cout << "Input Pin name:" << pin->getName() << " Value:" << pin->getValue() << endl;

  if(pin->getName() == "Y"){
    OutPin* outPin = hpController.getOutput("CNT");
    if(outPin != nullptr){
      if(pin->isActive()){
        cout << "Activating output: " << outPin->getName() << endl;
        outPin->turnOn();
      }else{
        cout << "Deactivating output: " << outPin->getName() << endl;
        outPin->turnOff();
      }
    }
  }
}

bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent){
  //cout << "Output pin:" << pin->getName() << " On:" << on << endl; 
  return true;
}

Task tCheckTemps(10 * TASK_SECOND, TASK_FOREVER, [](){
  sensors.requestTemperatures(); 
  for(auto& mp : _tempSens){
    if(mp.second->onCurrTempUpdate == nullptr) continue;
    mp.second->onCurrTempUpdate(mp.second);   
  } 
}, &ts, false);

Task tWaitOnWiFi(TASK_SECOND, 60, [](){
  cout << ".";
}, &ts, false, onWifiWaitEnable, onWifiWaitDisable);


Task tRuntime(TASK_MINUTE, TASK_FOREVER, &OnRunTimeUpdate, &ts, false);

Task _tGetInputs(500 * TASK_MILLISECOND, TASK_FOREVER, &onCheckInputQueue, &ts, false);

// NTP Time Sync Task - runs every 2 hours once enabled
void syncNtpTime();
Task tNtpSync(2 * TASK_HOUR, TASK_FOREVER, &syncNtpTime, &ts, false);

Task tConnectMQQT(TASK_SECOND, TASK_FOREVER, [](){
  connectToMqtt();
  while(!_mqttClient.connected()){
    yield();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  cout << "Mqtt connected:" << _mqttClient.connected() << endl;
  if(_mqttClient.connected()){
    tConnectMQQT.disable();
  }
  
}, &ts, false, onMqttWaitEnable, onMqttDisable);



void syncNtpTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Log.warn("NTP", "WiFi not connected, skipping NTP sync");
    return;
  }

  Log.info("NTP", "Syncing time from NTP servers...");
  configTime(_gmtOffset_sec, _daylightOffset_sec, _ntpServer1, _ntpServer2);

  // Wait for time to be set (max 10 seconds)
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    yield();
    vTaskDelay(pdMS_TO_TICKS(1000));
    retry++;
  }

  if (retry < 10) {
    _ntpSynced = true;
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Log.info("NTP", "Time synced: %s", timeStr);
  } else {
    Log.error("NTP", "Failed to sync time from NTP");
  }
}

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

void onRequest(AsyncWebServerRequest *request){
  //Handle Unknown Request
  request->send(404);
}

void onBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
  //Handle body
}

void onUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  //Handle upload
}

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  //Handle WebSocket event
}

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

bool initSDCard(){
  if (!sd.begin(SS, SPI_SPEED)) {
    if (sd.card()->errorCode()) {
      cout << 
          "\nSD initialization failed.\n"
          "Do not reformat the card!\n"
          "Is the card correctly inserted?\n"
          "Is chipSelect set to the correct value?\n"
          "Does another SPI device need to be disabled?\n"
          "Is there a wiring/soldering problem?\n";
      cout << "\nerrorCode: " << hex << showbase;
      cout << int(sd.card()->errorCode());
      cout << ", errorData: " << int(sd.card()->errorData());
      cout << dec << noshowbase << endl;
      return false;
    }
    cout << "\nCard successfully initialized.\n";
    if (sd.vol()->fatType() == 0) {
      cout << "Can't find a valid FAT16/FAT32/exFAT partition.\n";
      return false;
    }
    cout << "Can't determine error type\n";
    return false;
  }
  cout << "\nCard successfully initialized.\n";
  cout << endl;

  uint32_t size = sd.card()->sectorCount();
  if (size == 0) {
    cout << "Can't determine the card size.\n";
    return false;
  }
  return true;
}

bool openConfigFile(const char *filename, TempMap &config){
  if (!sd.exists(filename) || (_configFile.open(filename, O_RDONLY) && _configFile.size() == 0)) {
    _configFile.close();
    return saveConfiguration(filename, config, proj);
  }
  _configFile.close();
  
  return _configFile.open(filename, O_RDONLY);
}
bool loadTempConfig(const char *filename, TempMap &config)
{
  if(!_configFile.isOpen()){ return false; }
  _configFile.rewind();
  // Allocate a temporary JsonDocument
  // Stream& input;

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, _configFile);

  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return false;
  }

  const char* project = doc["project"]; // "Goodman"
  const char* created = doc["created"]; // nullptr
  const char* description = doc["description"]; // nullptr

  const char* wifi_ssid = doc["wifi"]["ssid"]; // "MEGA"
  const char* wifi_password = doc["wifi"]["password"]; // nullptr
  _WIFI_SSID = wifi_ssid != nullptr ? wifi_ssid : "";
  _WIFI_PASSWORD = wifi_password != nullptr ? wifi_password : "";
  cout << "Read WiFi SSID:" << wifi_ssid << endl;

  JsonObject mqtt = doc["mqtt"];
  const char* mqtt_user = mqtt["user"]; // "debian"
  const char* mqtt_password = mqtt["password"]; // nullptr
  const char* mqtt_host = mqtt["host"]; // "192.168.1.1"
  int mqtt_port = mqtt["port"]; // 1883
  _MQTT_PORT = mqtt_port;
  _MQTT_USER = mqtt_user != nullptr ? mqtt_user : "";
  _MQTT_PASSWORD = mqtt_password != nullptr ? mqtt_password : "";
  _MQTT_HOST.fromString(mqtt_host != nullptr ? mqtt_host : "192.168.1.2");
  cout << "Read mqtt SSID:" << _MQTT_HOST << endl;

  clearConfig(config);
  for (JsonPair sensors_temp_item : doc["sensors"]["temp"].as<JsonObject>()) {
    const char* key = sensors_temp_item.key().c_str(); // "288514B2000000EA", ...
    const char* description = sensors_temp_item.value()["description"];
    int last_value = sensors_temp_item.value()["last-value"]; // 0, 0, 0, 0
    const char* name = sensors_temp_item.value()["name"]; // "AMBIENT_TEMP", ...
    cout << "Key:" << key << "\t";
    cout << "Description:" << description << "\t";
    cout << "Last Value:" << last_value << endl;
    cout << "Name: " << name << endl;
    CurrentTemp *tmp = new CurrentTemp();
    uint8_t *devAddr = new uint8_t[sizeof(DeviceAddress)];
    config[name] = tmp;
    tmp->deviceAddress = devAddr;
    tmp->description = String(description);
    String devaddrStr = String(key);
    cout << "Devstr:" << devaddrStr  << endl;
    getDeviceAddress(devAddr, devaddrStr);
    tmp->Value = last_value;
    tmp->Previous = tmp->Value;
    tmp->Valid = true; 
    tmp->onCurrTempChanged = currentTempChangeCallback;
    tmp->onCurrTempUpdate = currentTempCallback;
    cout << "JSON description: " << tmp->description << "\tID:" << getStrDeviceAddress(devAddr)  << "\t Value:" << tmp->Value << endl;
  }
  // // Close the file (Curiously, File's destructor doesn't close the file)
  // file.close();
  _configFile.close();
  return true;
}
void clearConfig(TempMap &config)
{
  for (auto k : config)
  {
    if (k.second == nullptr)
      continue;
    if (k.second->deviceAddress != nullptr)
      delete k.second->deviceAddress;
    delete k.second;
  }
  config.clear();
}

bool saveConfiguration(const char* filename, TempMap& config, ProjectInfo& proj){
  if (sd.exists(filename) && _configFile.open(filename, O_RDONLY) && _configFile.size() > 0) {
    _configFile.close();
    return false;
  }
  _configFile.close();
  if (!_configFile.open(filename, O_RDWR | O_TRUNC | O_CREAT)) {
    cout << "open failed: " << "\"" << filename << "\"" << endl;
    return false;
  }
  JsonDocument doc;

  doc["project"] = "Goodman";
  doc["created"] = compile_date;
  doc["description"] = "Goodmand heatpump control board";

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = "MEGA";
  wifi["password"] = "";

  JsonObject mqtt = doc["mqtt"].to<JsonObject>();
  mqtt["user"] = "debian";
  mqtt["password"] = "";
  mqtt["host"] = "192.168.1.1";
  mqtt["port"] = 1883;

  JsonObject sensors = doc["sensors"].to<JsonObject>();

  JsonObject sensors_temp = sensors["temp"].to<JsonObject>();
  if(_tempSens.size() == 0){
    getTempSensors();
  }
  for (auto& mp : _tempSens){
    String id = getStrDeviceAddress(mp.second->deviceAddress);
    JsonObject temp = sensors_temp[id].to<JsonObject>();
    temp["description"] = mp.second->description;
    temp["last-value"] = mp.second->Value;
    temp["name"] = mp.first;
  }
  String output;
  serializeJson(doc, _configFile);
  serializeJsonPretty(doc, output);
  cout << "Temp sensor as json..." << endl;
  cout << output << endl;
  _configFile.close();
  return true;
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

void setupInputs(){
  for (auto& m : hpController.getInputMap()) {
    cout << "Setting up input " << m.second->getName() << endl;
    m.second->initPin();
    attachInterruptArg(m.second->getPin(), inputISRChange, m.second, CHANGE);
  }
}

void SetupWebServer()
{
  // Start NTP sync - run immediately then every 2 hours
  syncNtpTime();
  tNtpSync.enable();

  server.onNotFound(onRequest);
  server.onFileUpload(onUpload);
  server.onRequestBody(onBody);

  wes.onEvent(onWsEvent);
  server.addHandler(&wes);
  // Start the Web server
  server.begin();
  Log.info("HTTP", "HTTP server started");
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

  cout << "Flash Size:" << ESP.getFlashChipSize() * MB_MULTIPLIER << " MB" << endl;
  cout << "Flash Speed:" << ESP.getFlashChipSpeed() / (1000 * 1000) << " MHz" << endl;
  cout << "Flash Chip Mode:" << ESP.getFlashChipMode() << endl;
  cout << "PSRAM Size:" << ESP.getPsramSize() * MB_MULTIPLIER << " MB" << endl;

  
  acc_data_all = (unsigned char *) ps_malloc (n_elements * sizeof (unsigned char));  
  sprintf((char *)acc_data_all, "Test %d", millis());
  sensors.begin();
  if(initSDCard()){
    if(openConfigFile(_filename, _tempSens)){
      loadTempConfig(_filename, _tempSens);
    }
    
  }
  cout << "SD Card is read." << endl;
  WiFi.onEvent(onWiFiEvent);
  connectToWifi();

  SetupWebServer();

  setupMQTT();

  // Initialize Logger
  Log.setLevel(Logger::LOG_INFO);
  Log.setMqttClient(&_mqttClient, "goodman/log");
  Log.setLogFile(&sd, "/log.txt");
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

  setupInputs();

  // Start GoodmanHP controller
  hpController.begin();

  //setInputPinsMode();
  //setOutPinMode();
  
  //tReadWAgainIn4Sec.enable();
   // Define route for the root URL
  webAsyncFunctions();
  pinMode(_fanPin, OUTPUT);
  pinMode(_CNTPin, OUTPUT);
  pinMode(_WPin, OUTPUT);
  digitalWrite(_fanPin, HIGH);
  digitalWrite(_CNTPin, HIGH);
  digitalWrite(_WPin, HIGH);



  // Catch-All Handlers
  // Any request that can not find a Handler that canHandle it
  // ends in the callbacks below.
  


  

  //tReadInputs.enable();
  tRuntime.enable();
  tCheckTemps.enable();
  _tGetInputs.enable();

  Log.info("MAIN", "Starting Main Loop");
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    Serial.println("WebSocket client connected");
    // Send connection status to client
    String json = "{\"status\":\"connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    client->text(json);
  } else if(type == WS_EVT_DISCONNECT){
    Serial.println("WebSocket client disconnected");
  } else if(type == WS_EVT_ERROR){
    Serial.println("WebSocket error");
  } 
  // else if ( type == WS_EVT_DATA ){

  // }
}


void webAsyncFunctions()
{
  // Handle WebSocket events
  server.on("/ws", HTTP_GET, [](AsyncWebServerRequest *request){
    wes.handleRequest(request);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", 
      "<html><head><title>ESP32 OTA Update</title>"
      "<style>"
      "body { font-family: Arial, sans-serif; margin: 20px; }"
      ".progress-container { width: 100%; background-color: #f0f0f0; border-radius: 5px; margin: 10px 0; }"
      ".progress-bar { height: 20px; background-color: #4CAF50; border-radius: 5px; transition: width 0.1s; }"
      ".progress-text { text-align: center; margin: 5px 0; }"
      "button { background-color: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }"
      "button:hover { background-color: #45a049; }"
      "</style>"
      "</head>"
      "<body><h1>ESP32 OTA Update Server</h1>"
      "<p>Current IP: " + WiFi.localIP().toString() + "</p>"
      "<p>Use this server to upload new firmware</p>"
      "<a href='/update'>OTA Update Page</a>"
      "</body></html>"); });

  // First request will return 0 results unless you start scan from somewhere else (loop/setup)
  // Do not request more often than 3-5 seconds
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  String json = "[";
  int n = WiFi.scanComplete();
  if(n == -2){
    WiFi.scanNetworks(true);
  } else if(n){
    for (int i = 0; i < n; ++i){
      if(i) json += ",";
      json += "{";
      json += "\"rssi\":"+String(WiFi.RSSI(i));
      json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
      json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
      json += ",\"channel\":"+String(WiFi.channel(i));
      json += ",\"secure\":"+String(WiFi.encryptionType(i));
      //json += ",\"hidden\":"+String(WiFi.isHidden(i)?"true":"false");
      json += "}";
    }
    WiFi.scanDelete();
    if(WiFi.scanComplete() == -2){
      WiFi.scanNetworks(true);
    }
  }
  json += "]";
  request->send(200, "application/json", json);
  json = String(); });

  // Temp data
  server.on("/temps", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  String json = "[";
  bool firstTime = true;
  for (const auto& m: _tempSens){
      if(m.first != nullptr && m.second != nullptr && m.first.length() > 0){ 
        if(!firstTime) json += ",";
        json += "{";
        json += "\"description\":\""+ m.second->description+"\"";
        json += ",\"devid\":\""+getStrDeviceAddress(m.second->deviceAddress)+"\"";
        json += ",\"value\":"+String(m.second->Value);
        json += ",\"previous\":"+String(m.second->Previous);
        json += ",\"valid\":\""+String(m.second->Valid ? "true" : "false" )+"\"";
        json += "}";
      }
      firstTime = false;
  }
  json += "]";
  request->send(200, "application/json", json);
  json = String(); });

  // respond to GET requests on URL /heap
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request)
            {
      String json = "{";
        json += "\"free heap\":" + String(ESP.getFreeHeap());
        json += ",\"free psram MB\":" + String(ESP.getFreePsram() * MB_MULTIPLIER);
        json += ",\"used psram MB\":" + String((ESP.getPsramSize() - ESP.getFreePsram()) * MB_MULTIPLIER);
        json += "}";
  request->send(200, "application/json", json);
  json = String();
            });

  // Log level configuration endpoints
  server.on("/log/level", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"level\":" + String(Log.getLevel());
    json += ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/log/level", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("level")) {
      int level = request->getParam("level")->value().toInt();
      if (level >= 0 && level <= 3) {
        Log.setLevel((Logger::Level)level);
        Log.info("HTTP", "Log level changed to %d", level);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"level must be 0-3\"}");
      }
    } else {
      request->send(400, "application/json", "{\"error\":\"missing level param\"}");
    }
  });

  server.on("/log/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"level\":" + String(Log.getLevel());
    json += ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) + "\"";
    json += ",\"serial\":" + String(Log.isSerialEnabled() ? "true" : "false");
    json += ",\"mqtt\":" + String(Log.isMqttEnabled() ? "true" : "false");
    json += ",\"sdcard\":" + String(Log.isSdCardEnabled() ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/log/config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("serial")) {
      Log.enableSerial(request->getParam("serial")->value() == "true");
    }
    if (request->hasParam("mqtt")) {
      Log.enableMqtt(request->getParam("mqtt")->value() == "true");
    }
    if (request->hasParam("sdcard")) {
      Log.enableSdCard(request->getParam("sdcard")->value() == "true");
    }
    Log.info("HTTP", "Log config updated");
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });
  // Simple Firmware Update Form
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", 
      "<html><head><title>ESP32 OTA Update</title></head>"
      "<body><h1>ESP32 OTA Update</h1>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Update'>"
      "</form>"
      "</body></html>");
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
    // Send status 200 (OK) to tell the browser to continue with the upload
    _shouldReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", _shouldReboot?"OK":"FAIL");
    response->addHeader("Connection", "close");
    request->send(response); 
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    
    if (!index) {
      Log.info("OTA", "Update Start: %s", filename.c_str());
      // Update started
      if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) { // Start with unknown size
        Log.error("OTA", "Update.begin failed");
        Update.printError(Serial);
      }
    }
    
    // Write data to the flash
    if(!Update.hasError()){
      if(Update.write(data, len) != len){
        Update.printError(Serial);
      }
    }

    if (final) {
      if (Update.end(true)) { // true to set the size to the current progress
        Log.info("OTA", "OTA Update Successful");
      } else {
        Log.error("OTA", "OTA Update Failed");
        Update.printError(Serial);
      }
    }
  });
}

void currentTempCallback(CurrentTemp *temp){
  float tmp = DallasTemperature::rawToFahrenheit(sensors.getTemp(temp->deviceAddress));
  float delta = abs(temp->Previous - tmp);
  if(delta > 0.33f){ 
    temp->Previous = temp->Value;
    temp->Value = tmp;
    temp->Value == DEVICE_DISCONNECTED_F ? temp->Valid = false : temp->Valid = true;
    if(temp->onCurrTempChanged != nullptr) temp->onCurrTempChanged(temp); 
  }
}
void currentTempChangeCallback(CurrentTemp *temp){
  Serial.print(temp->description);

  temp->Valid ? Serial.print(" Temp Updated: ") : Serial.print(" Temp Invalid: ");
  Serial.print("Temp: ");
  Serial.print(temp->Value);
  Serial.print("F Previous Temp: ");
  Serial.print(temp->Previous);
  Serial.println("F");
}

void getTempSensors()
{
  Serial.print("Locating devices...");
  sensors.begin();
  Serial.print("Found ");
  u_int8_t oneWCount = sensors.getDeviceCount();
  Serial.print(oneWCount, DEC);
  Serial.println(" devices.");
  // If ever ran twice this will clean up things.
  clearConfig(_tempSens);
  for(u_int8_t i = 0; i < oneWCount; i++){
    uint8_t *devAddr = new uint8_t[sizeof(DeviceAddress)];
    CurrentTemp *tmp = new CurrentTemp();
    tmp->deviceAddress = devAddr;
    tmp->Previous = 0.0f;
    tmp->Valid = false;
    tmp->Value = 0.0f;
    tmp->onCurrTempChanged = currentTempChangeCallback;
    tmp->onCurrTempUpdate = currentTempCallback; 
    switch (i){
      case 0:
         tmp->description = "LINE_TEMP";
        break;
      case 1:
        tmp->description = "SUCTION_TEMP";
        break;
      case 2:
        tmp->description = "AMBIENT_TEMP";
        break;
      case 3:
        tmp->description = "CONDENSER_TEMP";
        break;
      default:
        tmp->description = "UNKOWN_TEMP";
    }
    if(_tempSens.count(tmp->description) == 0) _tempSens[tmp->description] = tmp;
    if (!sensors.getAddress(_tempSens[tmp->description]->deviceAddress, i))
      Serial.println("Unable to find address for Device 0");
    Serial.print("Device ");
    Serial.print(i);
    Serial.print (" Address: ");
    
    Serial.printf("Current Temp Description: %s ", _tempSens[tmp->description]->description);
    Serial.println(getStrDeviceAddress(devAddr));
  }
}

String getStrDeviceAddress(DeviceAddress temp)
{
  String tmp;
  char hexChar[6];
  
  for (uint8_t i = 0; i < 8; i++)
  {
    //if (temp[i] < 16) tmp+="0";
    sprintf(hexChar, "%02X", temp[i]);
    tmp+=hexChar;
  }
  return tmp; 
}
void getDeviceAddress(uint8_t *devAddr, String  temp){
  char* endptr;
  for(uint8_t i = 0; i < 8; i++){
    uint8_t x = i*2;
    String rs = String(temp[x]) + String(temp[x+1]);
    u_int32_t val = strtol(rs.c_str(), &endptr, 16);
    //cout << rs;
    devAddr[i] = val < 256 ? val : 0;  
  }
  
}

void printAddress(DeviceAddress temp)
{
  Serial.print(" ID: ");
  for (uint8_t i = 0; i < 8; i++)
  {
    if (temp[i] < 16) Serial.print("0");
    Serial.print(temp[i], HEX);
  }
}


void OnRunTimeUpdate(){
  currentRuntime = millis();
  Serial.printf("Current runtime: %ld\r\n", currentRuntime);
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
  if (_shouldReboot) {
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
