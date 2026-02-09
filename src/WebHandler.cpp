#include "WebHandler.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "TempSensor.h"

WebHandler::WebHandler(uint16_t port, Scheduler* ts, GoodmanHP* hpController)
    : _server(port), _ws("/ws"), _ts(ts), _hpController(hpController),
      _shouldReboot(false), _ntpSynced(false), _tNtpSync(nullptr),
      _tGpioTest(nullptr), _gpioTestRunning(false), _gpioTestStep(0) {}

void WebHandler::begin() {
    // NTP sync task - enabled on WiFi connect, then repeats every 2 hours
    _tNtpSync = new Task(2 * TASK_HOUR, TASK_FOREVER, [this]() {
        this->syncNtpTime();
    }, _ts, false);

    _server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404);
    });
    _server.onFileUpload([](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    });
    _server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    });

    _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
        this->onWsEvent(server, client, type, arg, data, len);
    });
    _server.addHandler(&_ws);

    setupRoutes();

    _server.begin();
    Log.info("HTTP", "HTTP server started");
}
const char * WebHandler::getWiFiIP(){
    String ipStr = WiFi.localIP().toString();
    return WiFi.isConnected() && ipStr.length() > 0  ? WiFi.localIP().toString().c_str() : NOT_AVAILABLE;
}

void WebHandler::startNtpSync() {
    if (_tNtpSync) {
        _tNtpSync->enable();
    }
}

void WebHandler::syncNtpTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Log.warn("NTP", "WiFi not connected, skipping NTP sync");
        return;
    }

    Log.info("NTP", "Syncing time from NTP servers...");
    configTime(_gmtOffsetSec, _daylightOffsetSec, NTP_SERVER1, NTP_SERVER2);

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

void WebHandler::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                           AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.println("WebSocket client connected");
        String json = "{\"status\":\"connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
        client->text(json);
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.println("WebSocket client disconnected");
    } else if (type == WS_EVT_ERROR) {
        Serial.println("WebSocket error");
    }
}

void WebHandler::setTimezone(int32_t gmtOffset, int32_t daylightOffset) {
    _gmtOffsetSec = gmtOffset;
    _daylightOffsetSec = daylightOffset;
}

void WebHandler::setupRoutes() {
    _server.on("/ws", HTTP_GET, [this](AsyncWebServerRequest *request) {
        _ws.handleRequest(request);
    });

    _server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html",
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
            "</body></html>");
    });

    _server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "[";
        int n = WiFi.scanComplete();
        if (n == -2) {
            WiFi.scanNetworks(true);
        } else if (n) {
            for (int i = 0; i < n; ++i) {
                if (i) json += ",";
                json += "{";
                json += "\"rssi\":" + String(WiFi.RSSI(i));
                json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
                json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
                json += ",\"channel\":" + String(WiFi.channel(i));
                json += ",\"secure\":" + String(WiFi.encryptionType(i));
                json += "}";
            }
            WiFi.scanDelete();
            if (WiFi.scanComplete() == -2) {
                WiFi.scanNetworks(true);
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    _server.on("/temps", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String json = "[";
        bool firstTime = true;
        for (const auto& m : _hpController->getTempSensorMap()) {
            if (m.first.length() > 0 && m.second != nullptr) {
                if (!firstTime) json += ",";
                json += "{";
                json += "\"description\":\"" + m.second->getDescription() + "\"";
                json += ",\"devid\":\"" + TempSensor::addressToString(m.second->getDeviceAddress()) + "\"";
                json += ",\"value\":" + String(m.second->getValue());
                json += ",\"previous\":" + String(m.second->getPrevious());
                json += ",\"valid\":\"" + String(m.second->isValid() ? "true" : "false") + "\"";
                json += "}";
            }
            firstTime = false;
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    _server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"free heap\":" + String(ESP.getFreeHeap());
        json += ",\"free psram MB\":" + String(ESP.getFreePsram() * MB_MULTIPLIER);
        json += ",\"used psram MB\":" + String((ESP.getPsramSize() - ESP.getFreePsram()) * MB_MULTIPLIER);
        json += "}";
        request->send(200, "application/json", json);
    });

    _server.on("/log/level", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"level\":" + String(Log.getLevel());
        json += ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    _server.on("/log/level", HTTP_POST, [](AsyncWebServerRequest *request) {
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

    _server.on("/log/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"level\":" + String(Log.getLevel());
        json += ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) + "\"";
        json += ",\"serial\":" + String(Log.isSerialEnabled() ? "true" : "false");
        json += ",\"mqtt\":" + String(Log.isMqttEnabled() ? "true" : "false");
        json += ",\"sdcard\":" + String(Log.isSdCardEnabled() ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });

    _server.on("/log/config", HTTP_POST, [](AsyncWebServerRequest *request) {
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

    _server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html",
            "<html><head><title>ESP32 OTA Update</title></head>"
            "<body><h1>ESP32 OTA Update</h1>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='update'>"
            "<input type='submit' value='Update'>"
            "</form>"
            "</body></html>");
    });

    _server.on("/test/w-to-o/result", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (_gpioTestRunning) {
            request->send(200, "application/json", "{\"status\":\"running\"}");
            return;
        }
        if (_gpioTestResult.length() == 0) {
            request->send(200, "application/json", "{\"status\":\"no test run\"}");
            return;
        }
        request->send(200, "application/json", _gpioTestResult);
    });

    _server.on("/test/w-to-o", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (_gpioTestRunning) {
            request->send(200, "application/json", "{\"status\":\"running\"}");
            return;
        }
        _gpioTestRunning = true;
        _gpioTestStep = 0;
        _gpioTestResult = "";

        OutPin* wPin = _hpController->getOutput("W");
        uint8_t oGpio = _hpController->getInput("O")->getPin();

        // 8 steps: initial, W on, read, W off, read, update(), read, finalize
        _tGpioTest = new Task(500 * TASK_MILLISECOND, 8, [this, wPin, oGpio]() {
            int oState;
            switch (_gpioTestStep) {
                case 0: {
                    // Record initial state
                    oState = digitalRead(oGpio);
                    const char* mode = _hpController->getStateString();
                    Log.info("GPIO-TEST", "Step 0 - Initial: mode=%s, W.isOn=%d, O=%d",
                             mode, wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult = "{\"mode\":\"" + String(mode) + "\"";
                    _gpioTestResult += ",\"initial\":{\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    break;
                }
                case 1: {
                    // Turn W ON to verify O follows (proves wiring works)
                    Log.info("GPIO-TEST", "Step 1 - Turning W ON");
                    wPin->turnOn();
                    break;
                }
                case 2: {
                    oState = digitalRead(oGpio);
                    _gpioTestOOn = oState;
                    Log.info("GPIO-TEST", "Step 2 - After W ON: W.isOn=%d, O=%d",
                             wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"w_on\":{\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    break;
                }
                case 3: {
                    // Turn W OFF
                    Log.info("GPIO-TEST", "Step 3 - Turning W OFF");
                    wPin->turnOff();
                    break;
                }
                case 4: {
                    oState = digitalRead(oGpio);
                    _gpioTestOOff = oState;
                    Log.info("GPIO-TEST", "Step 4 - After W OFF: W.isOn=%d, O=%d",
                             wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"w_off\":{\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    break;
                }
                case 5: {
                    // Run state machine update to let it enforce W state
                    Log.info("GPIO-TEST", "Step 5 - Running update() (state machine)");
                    _hpController->update();
                    break;
                }
                case 6: {
                    // Read O after state machine ran — W should be off unless DEFROST
                    oState = digitalRead(oGpio);
                    const char* mode = _hpController->getStateString();
                    bool wSw = wPin->isOn();
                    Log.info("GPIO-TEST", "Step 6 - After update(): mode=%s, W.isOn=%d, O=%d",
                             mode, wSw ? 1 : 0, oState);
                    _gpioTestResult += ",\"after_update\":{\"mode\":\"" + String(mode) +
                                       "\",\"w_sw\":" + String(wSw ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    break;
                }
                case 7: {
                    // Compute pass/fail
                    // O must follow W: high when on, low when off
                    bool wiringOk = (_gpioTestOOn == 1 && _gpioTestOOff == 0);
                    // After update(), O must be low (W off) unless mode is DEFROST
                    bool afterOk;
                    if (_hpController->getState() == GoodmanHP::State::DEFROST) {
                        afterOk = true;  // W on is expected in DEFROST
                    } else {
                        afterOk = (digitalRead(oGpio) == 0);  // W must be off
                    }
                    bool pass = wiringOk && afterOk;
                    _gpioTestResult += ",\"wiring_ok\":" + String(wiringOk ? "true" : "false");
                    _gpioTestResult += ",\"w_off_after_update\":" + String(afterOk ? "true" : "false");
                    _gpioTestResult += ",\"pass\":" + String(pass ? "true" : "false") + "}";
                    Log.info("GPIO-TEST", "Test complete: wiring=%s, w_off_after_update=%s, pass=%s",
                             wiringOk ? "OK" : "FAIL", afterOk ? "OK" : "FAIL",
                             pass ? "PASS" : "FAIL");
                    _gpioTestRunning = false;
                    break;
                }
            }
            _gpioTestStep++;
        }, _ts, false);

        _tGpioTest->enable();
        request->send(200, "application/json", "{\"status\":\"started\"}");
    });

    _server.on("/test/w-in-heat", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (_gpioTestRunning) {
            request->send(200, "application/json", "{\"status\":\"running\"}");
            return;
        }
        _gpioTestRunning = true;
        _gpioTestStep = 0;
        _gpioTestResult = "";

        OutPin* wPin = _hpController->getOutput("W");
        uint8_t oGpio = _hpController->getInput("O")->getPin();

        // Test W relay in each state via testSetState() which sets state + controls W
        // Pause update task to prevent race conditions
        _tGpioTest = new Task(500 * TASK_MILLISECOND, 10, [this, wPin, oGpio]() {
            int oState;
            switch (_gpioTestStep) {
                case 0: {
                    // Pause the update task so it doesn't override our state changes
                    _hpController->pauseUpdate();
                    oState = digitalRead(oGpio);
                    Log.info("GPIO-TEST", "ALL-STATE test - Initial: mode=%s, W.isOn=%d, O=%d",
                             _hpController->getStateString(), wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult = "{\"initial\":{\"mode\":\"" + String(_hpController->getStateString()) +
                                       "\",\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    break;
                }
                case 1: {
                    Log.info("GPIO-TEST", "ALL-STATE test - Setting HEAT");
                    _hpController->testSetState(GoodmanHP::State::HEAT);
                    break;
                }
                case 2: {
                    oState = digitalRead(oGpio);
                    Log.info("GPIO-TEST", "ALL-STATE test - HEAT: mode=%s, W.isOn=%d, O=%d",
                             _hpController->getStateString(), wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"heat\":{\"mode\":\"" + String(_hpController->getStateString()) +
                                       "\",\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    _gpioTestOOn = oState;
                    break;
                }
                case 3: {
                    Log.info("GPIO-TEST", "ALL-STATE test - Setting COOL");
                    _hpController->testSetState(GoodmanHP::State::COOL);
                    break;
                }
                case 4: {
                    oState = digitalRead(oGpio);
                    Log.info("GPIO-TEST", "ALL-STATE test - COOL: mode=%s, W.isOn=%d, O=%d",
                             _hpController->getStateString(), wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"cool\":{\"mode\":\"" + String(_hpController->getStateString()) +
                                       "\",\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    _gpioTestOOff = oState;
                    break;
                }
                case 5: {
                    Log.info("GPIO-TEST", "ALL-STATE test - Setting DEFROST");
                    _hpController->testSetState(GoodmanHP::State::DEFROST);
                    break;
                }
                case 6: {
                    oState = digitalRead(oGpio);
                    Log.info("GPIO-TEST", "ALL-STATE test - DEFROST: mode=%s, W.isOn=%d, O=%d",
                             _hpController->getStateString(), wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"defrost\":{\"mode\":\"" + String(_hpController->getStateString()) +
                                       "\",\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    _gpioTestWOn = oState;
                    break;
                }
                case 7: {
                    Log.info("GPIO-TEST", "ALL-STATE test - Restoring OFF");
                    _hpController->testSetState(GoodmanHP::State::OFF);
                    break;
                }
                case 8: {
                    oState = digitalRead(oGpio);
                    Log.info("GPIO-TEST", "ALL-STATE test - OFF: mode=%s, W.isOn=%d, O=%d",
                             _hpController->getStateString(), wPin->isOn() ? 1 : 0, oState);
                    _gpioTestResult += ",\"off\":{\"mode\":\"" + String(_hpController->getStateString()) +
                                       "\",\"w_sw\":" + String(wPin->isOn() ? 1 : 0) +
                                       ",\"o_pin\":" + String(oState) + "}";
                    _gpioTestWOff = oState;
                    break;
                }
                case 9: {
                    // Resume update task
                    _hpController->resumeUpdate();
                    // Finalize: W must be OFF in HEAT, COOL, OFF and ON only in DEFROST
                    bool heatOk = (_gpioTestOOn == 0);
                    bool coolOk = (_gpioTestOOff == 0);
                    bool defrostOk = (_gpioTestWOn == 1);
                    bool offOk = (_gpioTestWOff == 0);
                    bool pass = heatOk && coolOk && defrostOk && offOk;
                    _gpioTestResult += ",\"w_off_in_heat\":" + String(heatOk ? "true" : "false");
                    _gpioTestResult += ",\"w_off_in_cool\":" + String(coolOk ? "true" : "false");
                    _gpioTestResult += ",\"w_on_in_defrost\":" + String(defrostOk ? "true" : "false");
                    _gpioTestResult += ",\"w_off_in_off\":" + String(offOk ? "true" : "false");
                    _gpioTestResult += ",\"pass\":" + String(pass ? "true" : "false") + "}";
                    Log.info("GPIO-TEST", "ALL-STATE test: heat=%s cool=%s defrost=%s off=%s pass=%s",
                             heatOk ? "OK" : "FAIL", coolOk ? "OK" : "FAIL",
                             defrostOk ? "OK" : "FAIL", offOk ? "OK" : "FAIL",
                             pass ? "PASS" : "FAIL");
                    _gpioTestRunning = false;
                    break;
                }
            }
            _gpioTestStep++;
        }, _ts, false);

        _tGpioTest->enable();
        request->send(200, "application/json", "{\"status\":\"started\"}");
    });

    _server.on("/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
        _shouldReboot = !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", _shouldReboot ? "OK" : "FAIL");
        response->addHeader("Connection", "close");
        request->send(response);
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            Log.info("OTA", "Update Start: %s", filename.c_str());
            if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
                Log.error("OTA", "Update.begin failed");
                Update.printError(Serial);
            }
        }

        if (!Update.hasError()) {
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
        }

        if (final) {
            if (Update.end(true)) {
                Log.info("OTA", "OTA Update Successful");
            } else {
                Log.error("OTA", "OTA Update Failed");
                Update.printError(Serial);
            }
        }
    });
}
