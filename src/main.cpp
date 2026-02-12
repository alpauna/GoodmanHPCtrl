#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <ArxContainer.h>
#include <OneWire.h>
#include <WiFiServer.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <SimpleFTPServer.h>
#include <StringStream.h>
#include <TaskSchedulerDeclarations.h>
#include "CircularBuffer.hpp"
#include "Logger.h"
#include "OutPin.h"
#include "InputPin.h"
#include "GoodmanHP.h"
#include "Config.h"
#include "WebHandler.h"
#include "MQTTHandler.h"
#include "TempHistory.h"

#ifndef AP_PASSWORD
#error "AP_PASSWORD not defined — create secrets.ini with: -D AP_PASSWORD=\\\"yourpassword\\\""
#endif

extern const char compile_date[] = __DATE__ " " __TIME__;
IPAddress _MQTT_HOST_DEFAULT = IPAddress(192, 168, 0, 46);
const char* _filename = "/config.txt";
const float MB_MULTIPLIER = 1.0/(1024.0*1024.0);
const u_int16_t n_elements = 2000;


#if CIRCULAR_BUFFER_INT_SAFE
#else
#error "Needs to set CIRCULAR_BUFFER_INT_SAFE"
#endif

u_int16_t _MQTT_PORT = 1883;
String _MQTT_USER = "debian";
String _MQTT_PASSWORD = "";

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

FtpServer ftpSrv;
bool ftpActive = false;
unsigned long ftpStopTime = 0;


std::map<String, InputPin*> _isrEvent;

// Scheduler
Scheduler ts, hts;
u_int32_t _nextIdlePrintTime = 0;

u_int32_t _idleLoopCount = 0;
u_int32_t _workLoopCount = 0;
volatile bool InISR, InitialPinStateSet, FinalPinSetState;

// CPU load monitoring via FreeRTOS idle hooks
static volatile uint32_t _idleCountCore0 = 0;
static volatile uint32_t _idleCountCore1 = 0;
static uint32_t _maxIdleCore0 = 0;
static uint32_t _maxIdleCore1 = 0;
static uint8_t _cpuLoadCore0 = 0;
static uint8_t _cpuLoadCore1 = 0;

static bool idleHookCore0() { _idleCountCore0++; return false; }
static bool idleHookCore1() { _idleCountCore1++; return false; }

uint8_t getCpuLoadCore0() { return _cpuLoadCore0; }
uint8_t getCpuLoadCore1() { return _cpuLoadCore1; }

u_int32_t _wifiStartMillis = 0;

// WiFi AP fallback mode
static uint32_t _wifiDisconnectCount = 0;
bool _apModeActive = false;

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
const u_int8_t _sdaPin = GPIO_NUM_8;
const u_int8_t _sclPin = GPIO_NUM_9;
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
MQTTHandler mqttHandler(&ts);
TempHistory tempHistory;

OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);

// MCP9600 I2C thermocouple amplifier for LIQUID_TEMP
Adafruit_MCP9600 mcp9600;


typedef enum AC_STATE { OFF, COOL, HEAT, DEFROST, ERROR, LOW_TEMP } ACState;
static String AC_STATE_STR[] = {"OFF", "COOL", "HEAT", "DEFROST", "ERROR", "LOW_TEMP"};
static String BOOL_STR[] = {"TRUE", "FALSE"};

ProjectInfo proj = {
  "Goodman Heatpump Control",
  compile_date,
  "Control Goodman heatpump including defrost mode.",
  "",
  false,
  50 * 1024 * 1024,  // maxLogSize: 50MB default
  10,                 // maxOldLogCount: 10 files default
  0,                  // heatRuntimeAccumulatedMs: restored from config
  -21600,             // gmtOffsetSec: UTC-6 (US Central)
  3600,               // daylightOffsetSec: 1hr DST
  20.0f,              // lowTempThreshold: 20°F default
  600,                // apFallbackSeconds: 10 minutes
  "dark"              // theme: dark default
};


ACState _acState = OFF;
bool OnReadInputsEnable();
void OnReadInputsDisable();
bool OnInputChangeEnable();
void OnInputChangeDisable();
void OnRunTimeUpdate();
void getTempSensors();
void getTempSensors(TempSensorMap& tempMap);
bool onWifiWaitEnable();
void onWifiWaitDisable();
bool CheckTickTime(InputPin *pin);
void onCheckInputQueue();

void onInput(InputPin *pin){
  Log.info("InputPin", "Name: %s Value: %d", pin->getName(), pin->getValue());
}

bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent){
  //cout << "Output pin:" << pin->getName() << " On:" << pin->isPinOn() << endl; 
  Log.info("OutPin", "Name: %s State: %d Requested State: %d New Percent On: %lf Orig Percent On: %lf", pin->getName(), pin->isPinOn(), on, newPercent, origPercent);
  return true;
}

Task tWaitOnWiFi(TASK_SECOND, 60, [](){
  Serial.print(".");
}, &ts, false, onWifiWaitEnable, onWifiWaitDisable);


Task tRuntime(TASK_MINUTE, TASK_FOREVER, &OnRunTimeUpdate, &ts, false);

Task _tGetInputs(500 * TASK_MILLISECOND, TASK_FOREVER, &onCheckInputQueue, &ts, false);

// Save heat runtime to SD card every 5 minutes
void onSaveRuntime();
Task tSaveRuntime(5 * TASK_MINUTE, TASK_FOREVER, &onSaveRuntime, &ts, false);

// Log temperature history to per-sensor CSV files every 30 seconds
void onLogTempsCSV();
void cleanOldTempFiles(int maxAgeDays);
Task tLogTempsCSV(2 * TASK_MINUTE, TASK_FOREVER, &onLogTempsCSV, &ts, false);
static char _tempsCsvDate[12] = "";

// CPU load calculation every 1 second
void onCalcCpuLoad();
Task tCpuLoad(TASK_SECOND, TASK_FOREVER, &onCalcCpuLoad, &ts, false);

// Backfill temp history from SD after NTP sync
void onBackfillTempHistory();
Task tBackfillTempHistory(5 * TASK_SECOND, 12, &onBackfillTempHistory, &ts, false); // retry every 5s up to 12 times (60s)



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
  mqttHandler.disconnect();
  return true;
}

void startAPMode() {
  _apModeActive = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  const char* apSSID = "GoodmanHP";
  const char* apPass = AP_PASSWORD;
  WiFi.softAP(apSSID, apPass);
  IPAddress apIP = WiFi.softAPIP();
  Log.warn("WiFi", "========================================");
  Log.warn("WiFi", "AP MODE ACTIVE - Could not connect to WiFi");
  Log.warn("WiFi", "SSID: %s", apSSID);
  Log.warn("WiFi", "Password: %s", apPass);
  Log.warn("WiFi", "IP: %s", apIP.toString().c_str());
  Log.warn("WiFi", "========================================");
  Serial.println();
  Serial.println("*** AP MODE ***");
  Serial.printf("SSID: %s\n", apSSID);
  Serial.printf("Pass: %s\n", apPass);
  Serial.printf("IP:   %s\n", apIP.toString().c_str());
  Serial.println("***************");
}

void onWifiWaitDisable(){
  Serial.println();
  if (WiFi.isConnected()) {
    _wifiDisconnectCount = 0;
    Log.info("WiFi", "IP: %s", WiFi.localIP().toString().c_str());
  } else {
    _wifiDisconnectCount += 60;
    Log.warn("WiFi", "Connection timed out (%lu/%lu sec), no IP assigned",
             _wifiDisconnectCount, proj.apFallbackSeconds);
    if (_wifiDisconnectCount >= proj.apFallbackSeconds) {
      startAPMode();
      return;
    }
  }
  mqttHandler.startReconnect();
}

void printAddress(DeviceAddress temp);

void wifiConnected(){
  Serial.printf("WiFi Connected within %lu ms.\n", millis() - _wifiStartMillis);
  mqttHandler.startReconnect();
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info){
  switch(event){
    case SYSTEM_EVENT_STA_GOT_IP:
      _wifiDisconnectCount = 0;
      tWaitOnWiFi.disable();
      webHandler.startNtpSync();
      Log.info("WIFI", "Got ip: %s", webHandler.getWiFiIP());
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      if (_apModeActive) break;
      _wifiStartMillis = millis();
      tWaitOnWiFi.enableDelayed();
      mqttHandler.stopReconnect();
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
      Serial.printf("Activating pin: %s\n", pin->getName());
      //remove from activePins as it is not active any longer.
      pin->activeNow();
      pin->getTask()->restartDelayed( pin->getTask()->getInterval() );
    }else{
      Serial.printf("Deactivated pin: %s\n", pin->getName());
      pin->getTask()->disable();
      pin->inactiveNow();
      pin->fireCallback();
    }
    _isrEvent.erase(m.first);
  }  
}

unsigned char * acc_data_all;
void setup() {
  Serial.begin(115200);

  Wire.begin(_sdaPin, _sclPin);

  // Scan I2C bus for devices
  uint8_t i2cCount = 0;
  Serial.println("I2C scan starting...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\r\n", addr);
      i2cCount++;
    }
  }
  if (i2cCount == 0) {
    Serial.println("I2C scan: no devices found");
  } else {
    Serial.printf("I2C scan: %d device(s) found\r\n", i2cCount);
  }

  // Initialize MCP9600 thermocouple amplifier at 0x67
  bool mcp9600Ready = false;
  if (mcp9600.begin(0x67)) {
    mcp9600.setADCresolution(MCP9600_ADCRESOLUTION_18);
    mcp9600.setThermocoupleType(MCP9600_TYPE_K);
    mcp9600.setFilterCoefficient(3);
    mcp9600.enable(true);
    mcp9600Ready = true;
    Serial.println("MCP9600 thermocouple amplifier initialized at 0x67");
  } else {
    Serial.println("MCP9600 not found at 0x67, LIQUID_TEMP will be unavailable");
  }

  acc_data_all = (unsigned char *) ps_malloc (n_elements * sizeof (unsigned char));
  sprintf((char *)acc_data_all, "Test %d", millis());
  sensors.begin();

  // Set XOR obfuscation key (used as fallback when eFuse HMAC is not available)
  // Use a fixed key from secrets.ini so passwords survive OTA updates
  Config::setObfuscationKey(XOR_KEY);

  // Derive AES-256 key from eFuse HMAC for password encryption
  if (!config.initEncryption()) {
    Serial.println("WARNING: eFuse HMAC key not available. Using XOR obfuscation for passwords.");
    Serial.println("Burn an eFuse key with -D BURN_EFUSE_KEY to enable AES-256-GCM encryption.");
  }

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
      _MQTT_HOST_DEFAULT = config.getMqttHost();
      _MQTT_PORT = config.getMqttPort();
      _MQTT_USER = config.getMqttUser();
      _MQTT_PASSWORD = config.getMqttPassword();
      // Restore accumulated heat runtime from config
      hpController.setHeatRuntimeMs(proj.heatRuntimeAccumulatedMs);
      // Set low temp threshold from config
      hpController.setLowTempThreshold(proj.lowTempThreshold);
    }
    // Load TLS certificates for HTTPS server
    config.loadCertificates("/cert.pem", "/key.pem");
  }
  Serial.println("SD Card is read.");
  WiFi.onEvent(onWiFiEvent);
  connectToWifi();

  config.setProjectInfo(&proj);
  webHandler.setConfig(&config);
  webHandler.setTimezone(proj.gmtOffsetSec, proj.daylightOffsetSec);
  tempHistory.begin();
  webHandler.setTempHistory(&tempHistory);

  bool sdCardReady = config.isSDCardInitialized();

  // FTP control callbacks — SD is already initialized, no swap needed
  webHandler.setFtpControl(
    // Enable callback
    [sdCardReady](int durationMin) {
      if (!sdCardReady) return;
      ftpSrv.begin("admin", "admin");
      ftpActive = true;
      ftpStopTime = millis() + ((unsigned long)durationMin * 60000UL);
      Log.info("FTP", "FTP enabled for %d minutes", durationMin);
    },
    // Disable callback
    []() {
      if (ftpActive) {
        ftpSrv.end();
        ftpActive = false;
        ftpStopTime = 0;
        Log.info("FTP", "FTP disabled");
      }
    },
    // Status callback
    []() -> String {
      int remainingMin = 0;
      if (ftpActive && ftpStopTime > 0) {
        unsigned long now = millis();
        if (ftpStopTime > now) {
          remainingMin = (int)((ftpStopTime - now) / 60000) + 1;
        }
      }
      return "{\"active\":" + String(ftpActive ? "true" : "false") +
             ",\"remainingMinutes\":" + String(remainingMin) + "}";
    }
  );
  webHandler.setFtpState(&ftpActive, &ftpStopTime);

  // Start HTTPS before HTTP so setupRoutes() knows whether to redirect or serve directly
  if (config.hasCertificates()) {
    webHandler.beginSecure(config.getCert(), config.getCertLen(), config.getKey(), config.getKeyLen());
  } else {
    Log.warn("HTTPS", "No certificates on SD card, HTTPS disabled. /config and /update served over HTTP.");
  }
  webHandler.begin();

  // FTP is never auto-started at boot — enable on demand from config page.

  mqttHandler.begin(_MQTT_HOST_DEFAULT, _MQTT_PORT, _MQTT_USER, _MQTT_PASSWORD);
  mqttHandler.setController(&hpController);

  // Initialize Logger
  Log.setLevel(Logger::LOG_INFO);
  Log.setMqttClient(mqttHandler.getClient(), "goodman/log");
  Log.setLogFile("/log.txt", proj.maxLogSize, proj.maxOldLogCount);
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

  // Add MCP9600 LIQUID_TEMP sensor if hardware is present
  if (mcp9600Ready) {
    TempSensor* liquidSensor = new TempSensor("LIQUID_TEMP");
    liquidSensor->setMCP9600(&mcp9600);
    liquidSensor->setUpdateCallback(tempSensorUpdateCallback);
    liquidSensor->setChangeCallback(tempSensorChangeCallback);
    hpController.addTempSensor("LIQUID_TEMP", liquidSensor);
    Log.info("MAIN", "LIQUID_TEMP sensor added (MCP9600 thermocouple)");
  }

  hpController.setStateChangeCallback([](GoodmanHP::State, GoodmanHP::State) {
    mqttHandler.publishState();
  });
  hpController.setLPSFaultCallback([](bool active) {
    mqttHandler.publishFault("LPS",
        active ? "Low refrigerant pressure" : "Low refrigerant pressure cleared",
        active);
  });
  hpController.begin();

  tRuntime.enable();
  _tGetInputs.enable();
  tSaveRuntime.enable();
  tLogTempsCSV.enable();
  tBackfillTempHistory.enableDelayed();

  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);
  tCpuLoad.enable();

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
  mqttHandler.publishTemps();
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

static uint8_t _cpuLoadWarmup = 15; // Skip first 15s (WiFi/MQTT/NTP settle)

void onCalcCpuLoad() {
  uint32_t count0 = _idleCountCore0;
  uint32_t count1 = _idleCountCore1;
  _idleCountCore0 = 0;
  _idleCountCore1 = 0;

  if (_cpuLoadWarmup > 0) {
    _cpuLoadWarmup--;
    // Seed max with highest count seen during warmup
    if (count0 > _maxIdleCore0) _maxIdleCore0 = count0;
    if (count1 > _maxIdleCore1) _maxIdleCore1 = count1;
    return;
  }

  // New highs update max immediately
  if (count0 > _maxIdleCore0) _maxIdleCore0 = count0;
  if (count1 > _maxIdleCore1) _maxIdleCore1 = count1;

  // Raw load for this second
  uint8_t raw0 = (_maxIdleCore0 > 0) ? 100 - (count0 * 100 / _maxIdleCore0) : 0;
  uint8_t raw1 = (_maxIdleCore1 > 0) ? 100 - (count1 * 100 / _maxIdleCore1) : 0;

  // EMA smoothing: 25% new + 75% old
  _cpuLoadCore0 = (_cpuLoadCore0 * 3 + raw0 + 2) / 4;
  _cpuLoadCore1 = (_cpuLoadCore1 * 3 + raw1 + 2) / 4;
}

void onBackfillTempHistory() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return;  // NTP not ready, will retry
  tempHistory.backfillFromSD();
  tBackfillTempHistory.disable();  // Done, stop retrying
}

// Sensor key → CSV directory name mapping
struct TempCsvEntry {
    const char* sensorKey;
    const char* dirName;
};
static const TempCsvEntry tempCsvEntries[] = {
    {"AMBIENT_TEMP",    "ambient"},
    {"COMPRESSOR_TEMP", "compressor"},
    {"SUCTION_TEMP",    "suction"},
    {"CONDENSER_TEMP",  "condenser"},
    {"LIQUID_TEMP",     "liquid"}
};

void onLogTempsCSV() {
    if (!config.isSDCardInitialized()) return;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;  // No NTP sync yet

    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &timeinfo);

    // Date change: create dirs, clean old files
    if (strcmp(today, _tempsCsvDate) != 0) {
        strncpy(_tempsCsvDate, today, sizeof(_tempsCsvDate));
        if (!SD.exists("/temps")) SD.mkdir("/temps");
        for (int i = 0; i < 5; i++) {
            char dir[32];
            snprintf(dir, sizeof(dir), "/temps/%s", tempCsvEntries[i].dirName);
            if (!SD.exists(dir)) SD.mkdir(dir);
        }
        cleanOldTempFiles(7);
    }

    time_t epoch = mktime(&timeinfo);
    TempSensorMap& temps = hpController.getTempSensorMap();

    for (int i = 0; i < 5; i++) {
        auto it = temps.find(tempCsvEntries[i].sensorKey);
        if (it == temps.end() || it->second == nullptr || !it->second->isValid()) continue;

        char filepath[48];
        snprintf(filepath, sizeof(filepath), "/temps/%s/%s.csv",
                 tempCsvEntries[i].dirName, today);

        float tempVal = it->second->getValue();

        File f = SD.open(filepath, FILE_APPEND);
        if (f) {
            char row[32];
            snprintf(row, sizeof(row), "%ld,%.1f", (long)epoch, tempVal);
            f.println(row);
            f.close();
        }

        tempHistory.addSample(i, (uint32_t)epoch, tempVal);
    }
}

void cleanOldTempFiles(int maxAgeDays) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    time_t now = mktime(&timeinfo);
    time_t cutoff = now - ((time_t)maxAgeDays * 86400);

    for (int s = 0; s < 5; s++) {
        char dirPath[32];
        snprintf(dirPath, sizeof(dirPath), "/temps/%s", tempCsvEntries[s].dirName);

        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) continue;

        String toDelete[64];
        int deleteCount = 0;

        File entry = dir.openNextFile();
        while (entry && deleteCount < 64) {
            String name = entry.name();
            entry.close();

            if (name.endsWith(".csv")) {
                // Extract date from filename (may include path prefix)
                int slashIdx = name.lastIndexOf('/');
                String datePart = (slashIdx >= 0) ? name.substring(slashIdx + 1) : name;
                datePart = datePart.substring(0, 10);  // "YYYY-MM-DD"

                struct tm fileTm = {};
                if (sscanf(datePart.c_str(), "%d-%d-%d",
                           &fileTm.tm_year, &fileTm.tm_mon, &fileTm.tm_mday) == 3) {
                    fileTm.tm_year -= 1900;
                    fileTm.tm_mon -= 1;
                    time_t fileTime = mktime(&fileTm);
                    if (fileTime < cutoff) {
                        String fullPath = String(dirPath) + "/" + datePart + ".csv";
                        toDelete[deleteCount++] = fullPath;
                    }
                }
            }
            entry = dir.openNextFile();
        }
        dir.close();

        for (int i = 0; i < deleteCount; i++) {
            SD.remove(toDelete[i].c_str());
            Log.info("TEMPS", "Deleted old temp file: %s", toDelete[i].c_str());
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
  Serial.printf("Current WiFi IP:%s\n", webHandler.getWiFiIP());
  Serial.printf("Current HP Mode: %s\n", hpController.getStateString());

  // Stats for outpin activation.
  for (auto& out : hpController.getOutputMap()) {
    Serial.printf("Out Pin: %s On Count: %d State: %d\n", out.first.c_str(), out.second->getOnCount(), out.second->isPinOn());
  }
  _nextIdlePrintTime = millis() + 60000;
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

  if (ftpActive && ftpStopTime > 0 && millis() >= ftpStopTime) {
    ftpSrv.end();
    ftpActive = false;
    ftpStopTime = 0;
    Log.info("FTP", "FTP auto-disabled (timeout)");
  }
  if (ftpActive) ftpSrv.handleFTP();

  bool bIdle = ts.execute();
  if (bIdle) {
    _idleLoopCount++;
    printIdleStatus();
    vTaskDelay(1); // Yield to FreeRTOS so idle hooks can fire
  } else {
    _workLoopCount++;
  }
}
