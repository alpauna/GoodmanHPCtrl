#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <ArxContainer.h>
#include <OneWire.h>
#include <WiFiServer.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <LittleFS.h>
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
#include "DisplayManager.h"
#include "OtaUtils.h"
#include <max6675.h>

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
static String _ftpActivePassword;  // Must persist — SimpleFTPServer stores pointer, not copy


std::map<String, InputPin*> _isrEvent;

// Scheduler
Scheduler ts, hts;
u_int32_t _nextIdlePrintTime = 0;

u_int32_t _idleLoopCount = 0;
u_int32_t _workLoopCount = 0;
volatile bool InISR, InitialPinStateSet, FinalPinSetState;

// CPU load monitoring via FreeRTOS idle hooks (timer-based)
// Tracks actual idle microseconds per second using esp_timer_get_time().
// Consecutive idle hook calls < 200us apart = continuous idle time.
// Gaps > 200us = preempted by higher-priority task (not counted as idle).
static volatile int64_t _lastIdleCore0 = 0;
static volatile int64_t _lastIdleCore1 = 0;
static volatile uint32_t _idleUsCore0 = 0;
static volatile uint32_t _idleUsCore1 = 0;
static uint8_t _cpuLoadCore0 = 0;
static uint8_t _cpuLoadCore1 = 0;

static bool idleHookCore0() {
  int64_t now = esp_timer_get_time();
  int64_t delta = now - _lastIdleCore0;
  _lastIdleCore0 = now;
  if (delta > 0 && delta < 200) _idleUsCore0 += (uint32_t)delta;
  return false;
}
static bool idleHookCore1() {
  int64_t now = esp_timer_get_time();
  int64_t delta = now - _lastIdleCore1;
  _lastIdleCore1 = now;
  if (delta > 0 && delta < 200) _idleUsCore1 += (uint32_t)delta;
  return false;
}

uint8_t getCpuLoadCore0() { return _cpuLoadCore0; }
uint8_t getCpuLoadCore1() { return _cpuLoadCore1; }

u_int32_t _wifiStartMillis = 0;

// WiFi AP fallback mode
static uint32_t _wifiDisconnectCount = 0;
bool _apModeActive = false;
String _apPassword;

// Rapid reboot detection — RTC memory survives software resets
RTC_NOINIT_ATTR static uint32_t _rapidRebootCount;
static const uint32_t RAPID_REBOOT_THRESHOLD = 3;
static const uint32_t REBOOT_STABLE_MS = 5UL * 60 * 1000; // 5 min
static bool _rebootRateLimited = false;

// Boot watchdog — crash detection via RTC memory
RTC_NOINIT_ATTR static uint32_t _crashBootCount;
static const uint32_t CRASH_BOOT_THRESHOLD = 3;
static const uint32_t BOOT_STABLE_MS = 30UL * 1000; // 30s
bool _safeMode = false;

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
const u_int8_t _auxPin = GPIO_NUM_3;
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
DisplayManager displayMgr(&ts);

OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.
DallasTemperature sensors(&oneWire);

// MCP9600 I2C thermocouple amplifier for LIQUID_TEMP
Adafruit_MCP9600 mcp9600;

// MAX6675 SPI thermocouple (software bit-bang SPI, no bus conflict)
MAX6675* max6675Ptr = nullptr;


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
  "CST6CDT,M3.2.0,M11.1.0",  // timezone: US Central with auto DST
  20.0f,              // lowTempThreshold: 20°F default
  true,               // lowTempEnableW: W relay on in LOW_TEMP
  true,               // lowTempEnableAux: AUX signal on in LOW_TEMP
  140.0f,             // highSuctionTempThreshold: 140°F default
  false,              // rvFail: not latched
  30000,              // rvShortCycleMs: 30s default
  30000,              // cntShortCycleMs: 30s default
  180000,             // defrostMinRuntimeMs: 3 min default
  60.0f,              // defrostExitTempF: 60°F default
  5400000,            // heatRuntimeThresholdMs: 90 min default
  false,              // softwareDefrost: not active
  30000,              // stateValidationMs: 30s default
  10000,              // inputDelayMs: 10s default
  600,                // apFallbackSeconds: 10 minutes
  "",                 // apPassword: empty = auto-generate
  "",                 // ftpPassword: empty = default "admin"
  120,                // tempHistoryIntervalSec: 2 minutes default
  "dark",             // theme: dark default
  10,                 // displayPageIntervalSec: 10s default
  true,               // displayEnabled: on by default
  39,                 // max6675Clk: GPIO 39
  40,                 // max6675Cs: GPIO 40
  41,                 // max6675Do: GPIO 41
  true,               // max6675Enabled: on by default
  false,              // forceSafeMode: not forced
  "Goodman HP",       // systemName: default system name
  "goodman",          // mqttPrefix: default MQTT topic prefix
  0,                  // sessionTimeoutMinutes: disabled by default
  2                   // pollIntervalSec: 2 second default
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

// Clear crash boot counter after 30s stable uptime (boot watchdog)
Task tBootStable(BOOT_STABLE_MS, TASK_ONCE, []() {
    _crashBootCount = 0;
    Log.info("MAIN", "Boot stable (30s), crash counter reset to 0");
}, &ts, true);  // enabled immediately on boot

// Clear rapid reboot counter after 5 min stable uptime (DoS protection)
Task tRebootStable(REBOOT_STABLE_MS, TASK_ONCE, []() {
    _rapidRebootCount = 0;
    _rebootRateLimited = false;
    Log.info("MAIN", "Stable uptime (5 min), reboot rate limit cleared");
}, &ts, true);  // enabled immediately on boot

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

// Forward declaration for AP reconnect task
void onAPReconnect();
Task tAPReconnect(TASK_MINUTE, TASK_FOREVER, &onAPReconnect, &ts, false);

void startAPMode() {
  String apSSID = proj.systemName.length() > 0 ? proj.systemName : "Goodman HP";

  // Password: configured > previously generated > new random
  if (proj.apPassword.length() >= 8) {
    _apPassword = proj.apPassword;
  } else if (_apPassword.length() == 0) {
    _apPassword = Config::generateRandomPassword();
  }
  // else: reuse existing _apPassword

  if (!_apModeActive) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSSID.c_str(), _apPassword.c_str());
    _apModeActive = true;
  }

  IPAddress apIP = WiFi.softAPIP();
  Log.warn("WiFi", "========================================");
  Log.warn("WiFi", "AP MODE ACTIVE");
  Log.warn("WiFi", "SSID: %s", apSSID.c_str());
  Log.warn("WiFi", "Password: %s", _apPassword.c_str());
  Log.warn("WiFi", "IP: %s", apIP.toString().c_str());
  Log.warn("WiFi", "========================================");
  Serial.println();
  Serial.println("*** AP MODE ***");
  Serial.printf("SSID: %s\n", apSSID.c_str());
  Serial.printf("Pass: %s\n", _apPassword.c_str());
  Serial.printf("IP:   %s\n", apIP.toString().c_str());
  Serial.println("***************");

  // Start periodic WiFi reconnect attempts using AP fallback interval
  tAPReconnect.setInterval(proj.apFallbackSeconds * (unsigned long)TASK_SECOND);
  tAPReconnect.enableDelayed();

  // Kick off STA connection attempt in background
  if (_WIFI_SSID.length() > 0) {
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
  }
}

String startAPModeTest() {
  String apSSID = proj.systemName.length() > 0 ? proj.systemName : "Goodman HP";
  // Password: configured > previously generated > new random
  if (proj.apPassword.length() >= 8) {
    _apPassword = proj.apPassword;
  } else if (_apPassword.length() == 0) {
    _apPassword = Config::generateRandomPassword();
  }
  // Keep existing WiFi — add AP alongside (AP_STA)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), _apPassword.c_str());
  _apModeActive = true;
  Log.info("WiFi", "AP test mode started - SSID: %s Pass: %s IP: %s",
           apSSID.c_str(), _apPassword.c_str(), WiFi.softAPIP().toString().c_str());
  // Auto-exit after fallback interval when WiFi is still connected
  tAPReconnect.setInterval(proj.apFallbackSeconds * (unsigned long)TASK_SECOND);
  tAPReconnect.enableDelayed();
  return _apPassword;
}

void stopAPMode() {
  _apModeActive = false;
  // Keep _apPassword for reuse
  tAPReconnect.disable();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  Log.info("WiFi", "AP mode stopped");
  _wifiDisconnectCount = 0;
  if (_WIFI_SSID.length() > 0) {
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
  }
}

void onAPReconnect() {
  if (!_apModeActive) {
    tAPReconnect.disable();
    return;
  }

  if (WiFi.isConnected()) {
    // WiFi reconnected — exit AP mode gracefully
    Log.info("WiFi", "WiFi reconnected to %s, exiting AP mode", _WIFI_SSID.c_str());
    _apModeActive = false;
    _wifiDisconnectCount = 0;
    tAPReconnect.disable();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    mqttHandler.startReconnect();
    return;
  }

  // Not connected — retry
  if (_WIFI_SSID.length() > 0) {
    Log.info("WiFi", "AP mode: retrying WiFi connection to %s", _WIFI_SSID.c_str());
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
  }
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
      if (_apModeActive) {
        Log.info("WiFi", "WiFi reconnected while in AP mode, exiting AP");
        _apModeActive = false;
        tAPReconnect.disable();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
      }
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

    // Read live GPIO to determine pending direction
    bool liveActive = pin->readLiveState();

    // Only start delay if the live state differs from confirmed state
    if (liveActive != pin->isActive()) {
      Serial.printf("Input %s change detected (%s), validating in %lums\n",
                     pin->getName().c_str(), liveActive ? "active" : "inactive",
                     pin->getDelay());
      pin->setPendingState(liveActive ? 1 : 0);
      pin->getTask()->restartDelayed( pin->getTask()->getInterval() );
    }
    _isrEvent.erase(m.first);
  }
}

unsigned char * acc_data_all;
void setup() {
  Serial.begin(115200);

  // Boot watchdog: track reset reasons via RTC memory
  esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_BROWNOUT) {
    _rapidRebootCount = 0;
    _crashBootCount = 0;
  } else if (resetReason == ESP_RST_SW) {
    _rapidRebootCount++;
    // SW reset is neutral for crash counter (intentional reboots)
  } else if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
             resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) {
    _crashBootCount++;
  }

  // Rapid SW reboot rate limiting (DoS protection)
  if (_rapidRebootCount >= RAPID_REBOOT_THRESHOLD) {
    _rebootRateLimited = true;
    Serial.printf("WARNING: Rapid reboot detected (%u consecutive SW resets). "
                   "API reboot locked out until %lu min stable uptime.\n",
                   _rapidRebootCount, REBOOT_STABLE_MS / 60000);
  }

  // Crash boot safe mode (boot loop protection)
  if (_crashBootCount >= CRASH_BOOT_THRESHOLD) {
    _safeMode = true;
    Serial.printf("SAFE MODE: %u consecutive crash boots detected (PANIC/WDT). "
                   "HP controller disabled. Fix config and reboot.\n", _crashBootCount);
  }

  Wire.begin(_sdaPin, _sclPin);
  Wire.setTimeOut(1000);  // 1s I2C bus timeout — prevents MCP9600 from hanging boot

  // Initialize MCP9600 FIRST — bare I2C probe (beginTransmission/endTransmission
  // with no data) crashes some MCP9600 chips, corrupting the I2C bus.
  // See: https://forums.adafruit.com/viewtopic.php?t=163742
  // Workaround: skip probe, call begin() directly which does a full register read.
  // Wire timeout above prevents hang if chip locks the bus.
  bool mcp9600Ready = false;
  if (mcp9600.begin(0x67)) {
    mcp9600.setADCresolution(MCP9600_ADCRESOLUTION_18);
    mcp9600.setThermocoupleType(MCP9600_TYPE_K);
    mcp9600.setFilterCoefficient(3);
    mcp9600.enable(true);
    mcp9600Ready = true;
    Serial.println("MCP9600 thermocouple amplifier initialized at 0x67");
  } else {
    Serial.println("MCP9600 not found or hung at 0x67, LIQUID_TEMP unavailable");
    // Attempt I2C bus recovery — toggle SCL to release stuck SDA
    Wire.end();
    pinMode(_sclPin, OUTPUT);
    for (int i = 0; i < 16; i++) {
      digitalWrite(_sclPin, LOW);
      delayMicroseconds(5);
      digitalWrite(_sclPin, HIGH);
      delayMicroseconds(5);
    }
    pinMode(_sclPin, INPUT);
    Wire.begin(_sdaPin, _sclPin);
    Wire.setTimeOut(1000);
    Serial.println("I2C bus recovery attempted (16 clock pulses)");
  }

  // Scan I2C bus for devices — skip 0x67 (MCP9600) to avoid crashing it
  uint8_t i2cCount = mcp9600Ready ? 1 : 0;
  Serial.println("I2C scan starting...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == 0x67) continue;  // MCP9600: bare probe crashes some chips
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\r\n", addr);
      i2cCount++;
    }
  }
  if (mcp9600Ready) Serial.println("I2C device found at 0x67 (MCP9600, probed via driver)");
  if (i2cCount == 0) {
    Serial.println("I2C scan: no devices found");
  } else {
    Serial.printf("I2C scan: %d device(s) found\r\n", i2cCount);
  }

  // Initialize OLED display (SSD1306 128x64 at 0x3C)
  displayMgr.setController(&hpController);
  displayMgr.begin(0x3C);

  // Mount LittleFS for serving web pages from flash
  if (LittleFS.begin(true)) {
    Log.info("MAIN", "LittleFS mounted");
  } else {
    Log.error("MAIN", "LittleFS mount failed");
  }

  acc_data_all = (unsigned char *) ps_malloc (n_elements * sizeof (unsigned char));
  sprintf((char *)acc_data_all, "Test %d", millis());
  sensors.begin();

  // Set XOR obfuscation key (used as fallback when eFuse HMAC is not available)
  // Use a fixed key from secrets.ini so passwords survive OTA updates
  Config::setObfuscationKey(XOR_KEY);

  // Derive AES-256 key from eFuse HMAC for password encryption
  if (config.initEncryption()) {
    Serial.println("AES-256-GCM encryption enabled (eFuse HMAC key found)");
  } else {
    Serial.println("WARNING: eFuse HMAC key not available. Using XOR obfuscation for passwords.");
    Serial.println("Burn an eFuse key with -D BURN_EFUSE_KEY to enable AES-256-GCM encryption.");
  }

  // Initialize config and load from SD card
  config.setTempSensorDiscoveryCallback([](TempSensorMap& tempMap) {
    getTempSensors(tempMap);
  });

  if(config.initSDCard(true)){
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
      // Set heatpump protection settings from config
      hpController.setLowTempThreshold(proj.lowTempThreshold);
      hpController.setLowTempEnableW(proj.lowTempEnableW);
      hpController.setLowTempEnableAux(proj.lowTempEnableAux);
      hpController.setHighSuctionTempThreshold(proj.highSuctionTempThreshold);
      hpController.setRvShortCycleMs(proj.rvShortCycleMs);
      hpController.setCntShortCycleMs(proj.cntShortCycleMs);
      hpController.setDefrostMinRuntimeMs(proj.defrostMinRuntimeMs);
      hpController.setDefrostExitTempF(proj.defrostExitTempF);
      hpController.setHeatRuntimeThresholdMs(proj.heatRuntimeThresholdMs);
      hpController.setStateValidationMs(proj.stateValidationMs);
      if (proj.rvFail) hpController.setRvFail();  // Restore latched state
      if (proj.softwareDefrost) hpController.restoreSoftwareDefrost();  // Resume defrost after reboot
      // Apply temp history capture interval from config
      if (proj.tempHistoryIntervalSec >= 30 && proj.tempHistoryIntervalSec <= 300) {
          tLogTempsCSV.setInterval(proj.tempHistoryIntervalSec * (unsigned long)TASK_SECOND);
      }
      // Apply display settings from config
      displayMgr.setPageInterval(proj.displayPageIntervalSec);
      displayMgr.setEnabled(proj.displayEnabled);
      // Initialize MAX6675 SPI thermocouple if enabled
      if (proj.max6675Enabled) {
        max6675Ptr = new MAX6675(proj.max6675Clk, proj.max6675Cs, proj.max6675Do);
        delay(500);  // MAX6675 needs time to stabilize after power-on
        float testC = max6675Ptr->readCelsius();
        if (isnan(testC)) {
          Serial.println("MAX6675 not responding, disabling");
          delete max6675Ptr;
          max6675Ptr = nullptr;
        } else {
          Serial.printf("MAX6675 initialized (CLK=%d CS=%d DO=%d) test=%.1fC\n",
                        proj.max6675Clk, proj.max6675Cs, proj.max6675Do, testC);
        }
      }
    }
    // Load TLS certificates for HTTPS server
    config.loadCertificates("/cert.pem", "/key.pem");
    if (!config.hasCertificates()) {
      Log.warn("HTTPS", "No certificates found, generating self-signed cert...");
      if (config.generateSelfSignedCert()) {
        Log.info("HTTPS", "Self-signed certificate generated and saved to SD");
      } else {
        Log.error("HTTPS", "Certificate generation failed");
      }
    }
  }
  Serial.println("SD Card is read.");

  // Check forceSafeMode from config (one-shot flag)
  if (proj.forceSafeMode) {
    _safeMode = true;
    // Clear the flag so next boot is normal
    proj.forceSafeMode = false;
    TempSensorMap& tempSensorsForSave = hpController.getTempSensorMap();
    config.updateConfig(_filename, tempSensorsForSave, proj);
    Serial.println("SAFE MODE: forced via config flag (cleared for next boot)");
  }

  // Auto-revert firmware on crash boot loop.
  // NOTE: This safety net only works with OTA updates (POST /update → POST /apply),
  // which create /firmware.bak on SD before flashing. USB uploads (pio run -t upload)
  // bypass this entirely — no backup is created, so auto-revert is not possible.
  // Always prefer OTA for production deployments.
  if (_safeMode && _crashBootCount >= CRASH_BOOT_THRESHOLD && firmwareBackupExists()) {
    // Compare build dates — if backup is the same build as running firmware,
    // reverting would just re-flash the same broken firmware. Skip in that case.
    String backupBuild = getBackupBuildDate();
    if (backupBuild.length() > 0 && backupBuild == compile_date) {
      Serial.printf("AUTO-REVERT: Backup build date (%s) matches running firmware — "
                    "same build, skipping revert. Continuing in safe mode.\n",
                    backupBuild.c_str());
    } else {
      if (backupBuild.length() == 0) {
        Serial.printf("AUTO-REVERT: %u crash boots detected, no build date on backup — "
                      "attempting revert from /firmware.bak...\n", _crashBootCount);
      } else {
        Serial.printf("AUTO-REVERT: %u crash boots detected, backup build: %s, "
                      "running build: %s — reverting...\n",
                      _crashBootCount, backupBuild.c_str(), compile_date);
      }
      if (revertFirmwareFromSD()) {
        Serial.println("AUTO-REVERT: Firmware reverted successfully. Rebooting...");
        _crashBootCount = 0;  // Reset so the reverted firmware boots clean
        delay(500);
        ESP.restart();
        // Does not return
      } else {
        Serial.println("AUTO-REVERT: Firmware revert FAILED. Continuing in safe mode.");
      }
    }
  }

  WiFi.onEvent(onWiFiEvent);
  connectToWifi();

  config.setProjectInfo(&proj);
  webHandler.setConfig(&config);
  webHandler.setRebootRateLimited(&_rebootRateLimited);
  webHandler.setSafeMode(&_safeMode, &_crashBootCount);
  webHandler.setTimezone(proj.timezone);
  tempHistory.begin();
  webHandler.setTempHistory(&tempHistory);
  webHandler.setTempHistoryIntervalCallback([](uint32_t intervalSec) {
      tLogTempsCSV.setInterval(intervalSec * (unsigned long)TASK_SECOND);
      Log.info("MAIN", "Temp history interval changed to %us", intervalSec);
  });
  webHandler.setDisplayConfigCallback([](uint32_t interval, bool enabled) {
      displayMgr.setPageInterval(interval);
      displayMgr.setEnabled(enabled);
      Log.info("MAIN", "Display config: interval=%us enabled=%d", interval, enabled);
  });

  bool sdCardReady = config.isSDCardInitialized();

  // FTP control callbacks — SD is already initialized, no swap needed
  webHandler.setFtpControl(
    // Enable callback
    [sdCardReady, &proj](int durationMin) {
      if (!sdCardReady) return;
      _ftpActivePassword = proj.ftpPassword.length() > 0 ? proj.ftpPassword : "admin";
      ftpSrv.begin("admin", _ftpActivePassword.c_str());
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
    [&proj]() -> String {
      int remainingMin = 0;
      if (ftpActive && ftpStopTime > 0) {
        unsigned long now = millis();
        if (ftpStopTime > now) {
          remainingMin = (int)((ftpStopTime - now) / 60000) + 1;
        }
      }
      String ftpPw = proj.ftpPassword.length() > 0 ? proj.ftpPassword : "admin";
      return "{\"active\":" + String(ftpActive ? "true" : "false") +
             ",\"remainingMinutes\":" + String(remainingMin) +
             ",\"password\":\"" + ftpPw + "\"}";
    }
  );
  webHandler.setFtpState(&ftpActive, &ftpStopTime);

  // AP mode test/stop callbacks
  webHandler.setAPCallbacks(
    []() -> String { return startAPModeTest(); },
    []() { stopAPMode(); }
  );

  // Start HTTPS before HTTP so setupRoutes() knows whether to redirect or serve directly
  if (config.hasCertificates()) {
    webHandler.beginSecure(config.getCert(), config.getCertLen(), config.getKey(), config.getKeyLen());
  } else {
    Log.warn("HTTPS", "No certificates on SD card, HTTPS disabled. /config and /update served over HTTP.");
  }
  webHandler.begin();

  // FTP is never auto-started at boot — enable on demand from config page.

  mqttHandler.setTopicPrefix(proj.mqttPrefix);
  mqttHandler.begin(_MQTT_HOST_DEFAULT, _MQTT_PORT, _MQTT_USER, _MQTT_PASSWORD);
  mqttHandler.setController(&hpController);

  // Initialize Logger
  Log.setLevel(Logger::LOG_INFO);
  String logTopic = proj.mqttPrefix + "/log";
  Log.setMqttClient(mqttHandler.getClient(), logTopic.c_str());
  Log.setLogFile("/log.txt", proj.maxLogSize, proj.maxOldLogCount);
  Log.info("MAIN", "Logger initialized");

  // Log boot watchdog status now that Logger is available
  {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
      case ESP_RST_POWERON:  reasonStr = "POWERON"; break;
      case ESP_RST_SW:       reasonStr = "SW_RESET"; break;
      case ESP_RST_PANIC:    reasonStr = "PANIC"; break;
      case ESP_RST_INT_WDT:  reasonStr = "INT_WDT"; break;
      case ESP_RST_TASK_WDT: reasonStr = "TASK_WDT"; break;
      case ESP_RST_WDT:      reasonStr = "WDT"; break;
      case ESP_RST_BROWNOUT: reasonStr = "BROWNOUT"; break;
      case ESP_RST_DEEPSLEEP: reasonStr = "DEEPSLEEP"; break;
      default: break;
    }
    Log.info("MAIN", "Reset reason: %s, crash boots: %u/%u, rapid reboots: %u/%u",
      reasonStr, _crashBootCount, CRASH_BOOT_THRESHOLD,
      _rapidRebootCount, RAPID_REBOOT_THRESHOLD);
    if (_safeMode) {
      Log.error("SEC", "SAFE MODE ACTIVE: HP controller disabled. "
        "Crash boots: %u, force flag: %s. Fix config and reboot via web UI.",
        _crashBootCount, proj.forceSafeMode ? "was set" : "not set");
    }
    if (_rebootRateLimited) {
      Log.error("SEC", "REBOOT RATE LIMITED: %u consecutive SW resets. "
        "API /reboot locked out for %lu min stable uptime.",
        _rapidRebootCount, REBOOT_STABLE_MS / 60000);
    }
  }

  if (!_safeMode) {
    // Add input pins to GoodmanHP controller
    hpController.addInput("LPS", new InputPin(&ts, proj.inputDelayMs, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _lpsPin, "LPS", "LPS", onInput));
    hpController.addInput("DFT", new InputPin(&ts, proj.inputDelayMs, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _dftPin, "DFT", "DFT", onInput));
    hpController.addInput("Y", new InputPin(&ts, proj.inputDelayMs, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _yPin, "Y", "OT-NO", onInput));
    hpController.addInput("O", new InputPin(&ts, proj.inputDelayMs, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL, _oPin, "O", "OT-NC", onInput));

    // Add output pins to GoodmanHP controller
    hpController.addOutput("FAN", new OutPin(&ts, 0, _fanPin, "FAN", "FAN", onOutpin));
    hpController.addOutput("CNT", new OutPin(&ts, 3000, _CNTPin, "CNT", "CNT", onOutpin));
    hpController.addOutput("W", new OutPin(&ts, 0, _WPin, "W", "W", onOutpin));
    hpController.addOutput("RV", new OutPin(&ts, 0, _RVPin, "RV", "RV", onOutpin));
    hpController.addOutput("AUX", new OutPin(&ts, 0, _auxPin, "AUX", "AUX", onOutpin));

    // Start GoodmanHP controller
    hpController.setDallasTemperature(&sensors);

    // Add LIQUID_TEMP sensor: MAX6675 SPI > MAX31850K OneWire > MCP9600 I2C
    // MAX6675 is preferred when detected — replaces any OneWire LIQUID_TEMP
    TempSensorMap& postDiscoveryMap = hpController.getTempSensorMap();
    if (max6675Ptr != nullptr) {
      // Remove OneWire LIQUID_TEMP if it was auto-discovered (MAX6675 takes priority)
      if (postDiscoveryMap.count("LIQUID_TEMP") > 0) {
        Log.info("MAIN", "LIQUID_TEMP: replacing OneWire with MAX6675 SPI (higher priority)");
        delete postDiscoveryMap["LIQUID_TEMP"];
        postDiscoveryMap.erase("LIQUID_TEMP");
      }
      TempSensor* liquidSensor = new TempSensor("LIQUID_TEMP");
      liquidSensor->setMAX6675(max6675Ptr);
      liquidSensor->setUpdateCallback(tempSensorUpdateCallback);
      liquidSensor->setChangeCallback(tempSensorChangeCallback);
      hpController.addTempSensor("LIQUID_TEMP", liquidSensor);
      Log.info("MAIN", "LIQUID_TEMP sensor added (MAX6675 SPI)");
    } else if (postDiscoveryMap.count("LIQUID_TEMP") > 0) {
      Log.info("MAIN", "LIQUID_TEMP found on OneWire bus (MAX31850K)");
    } else if (mcp9600Ready) {
      TempSensor* liquidSensor = new TempSensor("LIQUID_TEMP");
      liquidSensor->setMCP9600(&mcp9600);
      liquidSensor->setI2CAddress(0x67);
      liquidSensor->setUpdateCallback(tempSensorUpdateCallback);
      liquidSensor->setChangeCallback(tempSensorChangeCallback);
      hpController.addTempSensor("LIQUID_TEMP", liquidSensor);
      Log.info("MAIN", "LIQUID_TEMP sensor added (MCP9600 I2C fallback)");
    } else {
      Log.warn("MAIN", "No LIQUID_TEMP sensor found (no MAX6675, MAX31850K, or MCP9600)");
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
  } else {
    Log.warn("MAIN", "SAFE MODE: HP controller, inputs, outputs, and MQTT publishing skipped");
  }

  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);
  tCpuLoad.enable();

  Log.info("MAIN", "Starting Main Loop%s", _safeMode ? " (SAFE MODE)" : "");
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
  bool rvFail = hpController.isRvFailActive();
  bool swDefrost = hpController.isSoftwareDefrostActive();
  bool runtimeChanged = (runtimeMs != proj.heatRuntimeAccumulatedMs);
  bool rvFailChanged = (rvFail != proj.rvFail);
  bool defrostChanged = (swDefrost != proj.softwareDefrost);

  if (runtimeChanged) proj.heatRuntimeAccumulatedMs = runtimeMs;
  if (rvFailChanged) proj.rvFail = rvFail;
  if (defrostChanged) proj.softwareDefrost = swDefrost;

  if (rvFailChanged || defrostChanged) {
    // rvFail and softwareDefrost are in heatpump section — need full config update
    TempSensorMap& tempSensors = hpController.getTempSensorMap();
    if (config.updateConfig(_filename, tempSensors, proj)) {
      Log.info("MAIN", "Config saved (rvFail=%d, defrost=%d, runtime=%lu ms)", rvFail, swDefrost, runtimeMs);
    }
  } else if (runtimeChanged) {
    if (config.updateRuntime(_filename, runtimeMs, swDefrost)) {
      Log.debug("MAIN", "Heat runtime saved: %lu ms", runtimeMs);
    }
  }
}

static uint8_t _cpuLoadWarmup = 5; // Skip first 5s for idle hooks to stabilize

void onCalcCpuLoad() {
  // Atomically read and reset idle microsecond accumulators
  uint32_t idle0 = _idleUsCore0; _idleUsCore0 = 0;
  uint32_t idle1 = _idleUsCore1; _idleUsCore1 = 0;

  if (_cpuLoadWarmup > 0) {
    _cpuLoadWarmup--;
    return;
  }

  // Convert idle microseconds to load % (1 second = 1,000,000 us)
  uint8_t raw0 = (idle0 >= 1000000) ? 0 : (uint8_t)(100 - idle0 / 10000);
  uint8_t raw1 = (idle1 >= 1000000) ? 0 : (uint8_t)(100 - idle1 / 10000);

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
  } else {
    _workLoopCount++;
  }
  vTaskDelay(1); // Yield to FreeRTOS so idle hooks can fire on both cores
}
