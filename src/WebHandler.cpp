#include "WebHandler.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "TempSensor.h"
#include "TempHistory.h"
#include "OtaUtils.h"

extern const char compile_date[];

extern uint8_t getCpuLoadCore0();
extern uint8_t getCpuLoadCore1();
extern bool _apModeActive;

WebHandler::WebHandler(uint16_t port, Scheduler* ts, GoodmanHP* hpController)
    : _server(port), _ws("/ws"), _ts(ts), _hpController(hpController),
      _config(nullptr), _shouldReboot(false), _tDelayedReboot(nullptr),
      _ntpSynced(false), _tNtpSync(nullptr) {}

void WebHandler::setFtpControl(FtpEnableCallback enableCb, FtpDisableCallback disableCb, FtpStatusCallback statusCb) {
    _ftpEnableCb = enableCb;
    _ftpDisableCb = disableCb;
    _ftpStatusCb = statusCb;
}

void WebHandler::setFtpState(bool* activePtr, unsigned long* stopTimePtr) {
    _ftpActivePtr = activePtr;
    _ftpStopTimePtr = stopTimePtr;
}

bool WebHandler::checkAuth(AsyncWebServerRequest* request) {
    if (!_config || !_config->hasAdminPassword()) return true;

    String authHeader = request->header("Authorization");
    if (!authHeader.startsWith("Basic ")) {
        request->requestAuthentication(nullptr, false);
        return false;
    }

    String b64 = authHeader.substring(6);
    size_t decodedLen = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedLen,
        (const uint8_t*)b64.c_str(), b64.length());
    uint8_t* decoded = new uint8_t[decodedLen + 1];
    mbedtls_base64_decode(decoded, decodedLen + 1, &decodedLen,
        (const uint8_t*)b64.c_str(), b64.length());
    decoded[decodedLen] = '\0';

    String credentials = String((char*)decoded);
    delete[] decoded;

    int colonIdx = credentials.indexOf(':');
    if (colonIdx < 0) {
        request->requestAuthentication(nullptr, false);
        return false;
    }

    String password = credentials.substring(colonIdx + 1);
    if (_config->verifyAdminPassword(password)) {
        return true;
    }

    request->requestAuthentication(nullptr, false);
    return false;
}

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

    Log.setWebSocket(&_ws);
    Log.enableWebSocket(true);

    setupRoutes();

    _server.begin();
    Log.info("HTTP", "HTTP server started");
}
const char * WebHandler::getWiFiIP(){
    if (!WiFi.isConnected()) return NOT_AVAILABLE;
    _wifiIPStr = WiFi.localIP().toString();
    return _wifiIPStr.length() > 0 ? _wifiIPStr.c_str() : NOT_AVAILABLE;
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

const char* WebHandler::getContentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".ico")) return "image/x-icon";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".svg")) return "image/svg+xml";
    return "text/plain";
}

void WebHandler::serveFile(AsyncWebServerRequest* request, const String& path) {
    if (!_config || !_config->isSDCardInitialized()) {
        request->send(503, "text/plain", "SD card not available");
        return;
    }
    String fullPath = "/www" + path;
    fs::File file = SD.open(fullPath.c_str(), FILE_READ);
    if (!file) {
        request->send(404, "text/plain", "Not found: " + path);
        return;
    }
    size_t fileSize = file.size();
    if (fileSize == 0) {
        file.close();
        request->send(200, getContentType(path), "");
        return;
    }
    char* buf = (char*)ps_malloc(fileSize + 1);
    if (!buf) {
        file.close();
        request->send(500, "text/plain", "Out of memory");
        return;
    }
    file.read((uint8_t*)buf, fileSize);
    buf[fileSize] = '\0';
    file.close();
    request->send(200, getContentType(path), buf);
    free(buf);
}

void WebHandler::setupRoutes() {
    _server.on("/ws", HTTP_GET, [this](AsyncWebServerRequest *request) {
        _ws.handleRequest(request);
    });

    _server.on("/theme.css", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/theme.css");
    });

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/index.html");
    });

    _server.on("/dashboard", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/dashboard.html");
    });

    _server.on("/log/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/log.html");
    });

    _server.on("/heap/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/heap.html");
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

    // Consolidated temp history from PSRAM (must be registered before /temps/history)
    _server.on("/temps/history/all", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!_tempHistory) {
            request->send(503, "application/json", "{\"error\":\"Temp history not available\"}");
            return;
        }
        int range = 24;
        if (request->hasParam("range")) {
            range = request->getParam("range")->value().toInt();
            if (range < 1) range = 1;
            if (range > 168) range = 168;
        }
        struct tm ti;
        if (!getLocalTime(&ti, 0)) {
            request->send(503, "application/json", "{\"error\":\"Time not synced\"}");
            return;
        }
        time_t now = mktime(&ti);
        uint32_t sinceEpoch = (uint32_t)(now - (time_t)range * 3600);

        // Allocate output buffer on PSRAM for largest possible result
        int maxPerSensor = TempHistory::MAX_SAMPLES;
        TempSample* buf = (TempSample*)ps_malloc(maxPerSensor * sizeof(TempSample));
        if (!buf) {
            request->send(500, "application/json", "{\"error\":\"Out of memory\"}");
            return;
        }

        String json = "{\"sensors\":{";
        for (int s = 0; s < TempHistory::MAX_SENSORS; s++) {
            if (s > 0) json += ",";
            json += "\"";
            json += TempHistory::sensorDirs[s];
            json += "\":[";
            int count = _tempHistory->getSamples(s, sinceEpoch, buf, maxPerSensor);

            // Decimate to max 500 points
            int step = 1;
            if (count > 500) step = (count + 499) / 500;

            bool first = true;
            for (int i = 0; i < count; i += step) {
                if (!first) json += ",";
                json += "[";
                json += String(buf[i].epoch);
                json += ",";
                json += String(buf[i].temp, 1);
                json += "]";
                first = false;
            }
            json += "]";
        }
        json += "}}";
        free(buf);
        request->send(200, "application/json", json);
    });

    // Temperature history CSV endpoint (must be registered before /temps)
    _server.on("/temps/history", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!_config || !_config->isSDCardInitialized()) {
            request->send(503, "application/json", "{\"error\":\"SD card not available\"}");
            return;
        }

        static const char* validSensors[] = {"ambient","compressor","suction","condenser","liquid"};
        if (!request->hasParam("sensor")) {
            request->send(400, "application/json", "{\"error\":\"Missing sensor param\"}");
            return;
        }
        String sensor = request->getParam("sensor")->value();
        bool valid = false;
        for (int i = 0; i < 5; i++) {
            if (sensor == validSensors[i]) { valid = true; break; }
        }
        if (!valid) {
            request->send(400, "application/json", "{\"error\":\"Invalid sensor\"}");
            return;
        }

        String dirPath = "/temps/" + sensor;

        if (request->hasParam("date")) {
            String date = request->getParam("date")->value();
            if (date.length() != 10 || date[4] != '-' || date[7] != '-') {
                request->send(400, "application/json", "{\"error\":\"Invalid date format\"}");
                return;
            }
            String filepath = dirPath + "/" + date + ".csv";
            if (!SD.exists(filepath.c_str())) {
                request->send(404, "application/json", "{\"error\":\"No data\"}");
                return;
            }
            request->send(SD, filepath.c_str(), "text/csv");
            return;
        }

        File dir = SD.open(dirPath.c_str());
        if (!dir || !dir.isDirectory()) {
            request->send(200, "application/json", "{\"files\":[]}");
            return;
        }

        String json = "{\"files\":[";
        bool first = true;
        File entry = dir.openNextFile();
        while (entry) {
            String name = entry.name();
            size_t size = entry.size();
            entry.close();
            if (name.endsWith(".csv")) {
                int slashIdx = name.lastIndexOf('/');
                String datePart = (slashIdx >= 0) ? name.substring(slashIdx + 1) : name;
                datePart = datePart.substring(0, datePart.length() - 4);
                if (!first) json += ",";
                json += "{\"date\":\"" + datePart + "\",\"size\":" + String(size) + "}";
                first = false;
            }
            entry = dir.openNextFile();
        }
        dir.close();
        json += "]}";
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
        json += ",\"cpuLoad0\":" + String(getCpuLoadCore0());
        json += ",\"cpuLoad1\":" + String(getCpuLoadCore1());
        json += "}";
        request->send(200, "application/json", json);
    });

    _server.on("/theme", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String theme = "dark";
        if (_config && _config->getProjectInfo()) {
            theme = _config->getProjectInfo()->theme;
            if (theme.length() == 0) theme = "dark";
        }
        request->send(200, "application/json", "{\"theme\":\"" + theme + "\"}");
    });

    _server.on("/state", HTTP_GET, [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["state"] = _hpController->getStateString();

        JsonObject inputs = doc["inputs"].to<JsonObject>();
        for (auto& pair : _hpController->getInputMap()) {
            if (pair.second != nullptr)
                inputs[pair.first] = pair.second->isActive();
        }

        JsonObject outputs = doc["outputs"].to<JsonObject>();
        for (auto& pair : _hpController->getOutputMap()) {
            if (pair.second != nullptr)
                outputs[pair.first] = pair.second->isPinOn();
        }

        doc["heatRuntimeMin"] = _hpController->getHeatRuntimeMs() / 60000UL;
        doc["defrost"] = _hpController->isSoftwareDefrostActive();
        doc["lpsFault"] = _hpController->isLPSFaultActive();
        doc["lowTemp"] = _hpController->isLowTempActive();
        doc["compressorOverTemp"] = _hpController->isCompressorOverTempActive();
        doc["suctionLowTemp"] = _hpController->isSuctionLowTempActive();
        doc["startupLockout"] = _hpController->isStartupLockoutActive();
        doc["startupLockoutRemainSec"] = _hpController->getStartupLockoutRemainingMs() / 1000;
        doc["shortCycleProtection"] = _hpController->isShortCycleProtectionActive();
        doc["rvFail"] = _hpController->isRvFailActive();
        doc["highSuctionTemp"] = _hpController->isHighSuctionTempActive();
        doc["defrostTransition"] = _hpController->isDefrostTransitionActive();
        doc["defrostTransitionRemainSec"] = _hpController->getDefrostTransitionRemainingMs() / 1000;
        doc["defrostCntPending"] = _hpController->isDefrostCntPendingActive();
        doc["defrostCntPendingRemainSec"] = _hpController->getDefrostCntPendingRemainingMs() / 1000;
        doc["defrostExiting"] = _hpController->isDefrostExitingActive();
        doc["manualOverride"] = _hpController->isManualOverrideActive();
        doc["manualOverrideRemainSec"] = _hpController->getManualOverrideRemainingMs() / 1000;
        doc["cpuLoad0"] = getCpuLoadCore0();
        doc["cpuLoad1"] = getCpuLoadCore1();
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["wifiSSID"] = WiFi.SSID();
        doc["wifiRSSI"] = WiFi.RSSI();
        doc["wifiIP"] = WiFi.localIP().toString();
        doc["apMode"] = _apModeActive;
        doc["buildDate"] = compile_date;
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            char buf[20];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
            doc["datetime"] = buf;
        }

        JsonObject temps = doc["temps"].to<JsonObject>();
        for (const auto& m : _hpController->getTempSensorMap()) {
            if (m.second != nullptr && m.second->isValid())
                temps[m.first] = m.second->getValue();
        }

        String json;
        serializeJson(doc, json);
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
        json += ",\"websocket\":" + String(Log.isWebSocketEnabled() ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });

    _server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        size_t count = Log.getRingBufferCount();
        size_t limit = count;
        if (request->hasParam("limit")) {
            limit = request->getParam("limit")->value().toInt();
            if (limit > count) limit = count;
        }

        const auto& buffer = Log.getRingBuffer();
        size_t head = Log.getRingBufferHead();
        size_t bufSize = buffer.size();

        // Calculate start index: we want the last 'limit' entries
        size_t startOffset = count - limit;

        String json = "{\"count\":" + String(limit) + ",\"entries\":[";
        for (size_t i = 0; i < limit; i++) {
            size_t idx = (head + bufSize - count + startOffset + i) % bufSize;
            if (i > 0) json += ",";
            json += "\"";
            // Escape JSON characters
            const String& entry = buffer[idx];
            for (size_t j = 0; j < entry.length(); j++) {
                char c = entry[j];
                switch (c) {
                    case '"':  json += "\\\""; break;
                    case '\\': json += "\\\\"; break;
                    case '\n': json += "\\n"; break;
                    case '\r': json += "\\r"; break;
                    case '\t': json += "\\t"; break;
                    default:   json += c; break;
                }
            }
            json += "\"";
        }
        json += "]}";
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
        if (request->hasParam("websocket")) {
            Log.enableWebSocket(request->getParam("websocket")->value() == "true");
        }
        Log.info("HTTP", "Log config updated");
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    _server.on("/i2c/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "[";
        bool first = true;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                if (!first) json += ",";
                char hex[7];
                snprintf(hex, sizeof(hex), "0x%02X", addr);
                json += "{\"address\":\"" + String(hex) + "\",\"decimal\":" + String(addr) + "}";
                first = false;
            }
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    // --- Pin table / manual override ---
    _server.on("/pins", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        if (request->hasParam("format") && request->getParam("format")->value() == "json") {
            JsonDocument doc;
            doc["manualOverride"] = _hpController->isManualOverrideActive();
            doc["manualOverrideRemainSec"] = _hpController->getManualOverrideRemainingMs() / 1000;
            doc["shortCycleActive"] = _hpController->isShortCycleProtectionActive();
            doc["state"] = _hpController->getStateString();
            doc["defrost"] = _hpController->isSoftwareDefrostActive();
            doc["defrostTransition"] = _hpController->isDefrostTransitionActive();
            doc["defrostCntPending"] = _hpController->isDefrostCntPendingActive();
            doc["defrostExiting"] = _hpController->isDefrostExitingActive();

            JsonArray inputs = doc["inputs"].to<JsonArray>();
            for (auto& pair : _hpController->getInputMap()) {
                if (pair.second != nullptr) {
                    JsonObject inp = inputs.add<JsonObject>();
                    inp["pin"] = pair.second->getPin();
                    inp["name"] = pair.first;
                    inp["active"] = pair.second->isActive();
                }
            }

            JsonArray outputs = doc["outputs"].to<JsonArray>();
            for (auto& pair : _hpController->getOutputMap()) {
                if (pair.second != nullptr) {
                    JsonObject out = outputs.add<JsonObject>();
                    out["pin"] = pair.second->getPin();
                    out["name"] = pair.first;
                    out["on"] = pair.second->isPinOn();
                }
            }

            JsonObject temps = doc["temps"].to<JsonObject>();
            for (const auto& m : _hpController->getTempSensorMap()) {
                if (m.second != nullptr && m.second->isValid())
                    temps[m.first] = m.second->getValue();
            }

            String json;
            serializeJson(doc, json);
            request->send(200, "application/json", json);
            return;
        }
        serveFile(request, "/pins.html");
    });

    auto* pinsPostHandler = new AsyncCallbackJsonWebHandler("/pins", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!checkAuth(request)) return;
        JsonObject data = json.as<JsonObject>();
        JsonDocument resp;

        // Toggle manual override
        if (data["manualOverride"].is<bool>()) {
            bool on = data["manualOverride"] | false;
            _hpController->setManualOverride(on);
            resp["status"] = "ok";
            resp["manualOverride"] = _hpController->isManualOverrideActive();
            resp["message"] = on ? "Manual override enabled (30 min timeout)" : "Manual override disabled, all outputs OFF";
            String json;
            serializeJson(resp, json);
            request->send(200, "application/json", json);
            return;
        }

        // Set output state
        if (data["output"].is<const char*>()) {
            String name = data["output"] | String("");
            bool state = data["state"] | false;
            String err = _hpController->setManualOutput(name, state);
            if (err.length() > 0) {
                resp["error"] = err;
            } else {
                resp["status"] = "ok";
                resp["output"] = name;
                resp["state"] = state;
            }
            String json;
            serializeJson(resp, json);
            request->send(200, "application/json", json);
            return;
        }

        // Force defrost
        if (data["forceDefrost"].is<bool>() && data["forceDefrost"]) {
            String err = _hpController->forceDefrost();
            if (err.length() > 0) {
                resp["error"] = err;
            } else {
                resp["status"] = "ok";
                resp["message"] = "Defrost initiated";
            }
            String json;
            serializeJson(resp, json);
            request->send(200, "application/json", json);
            return;
        }

        request->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    });
    _server.addHandler(pinsPostHandler);

    // /config and /update are served via HTTPS when certificates are available.
    // If HTTPS is active, these redirect HTTP->HTTPS. If no certs, they serve directly on HTTP.
    // The actual handlers are registered in registerHttpsHandlers() or as fallbacks below.
    if (!_httpsServer) {
        // No HTTPS — serve /update and /config directly on HTTP (fallback)
        _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            serveFile(request, "/update.html");
        });

        _server.on("/admin/setup", HTTP_GET, [this](AsyncWebServerRequest *request) {
            serveFile(request, "/admin.html");
        });

        auto* adminPostHandler = new AsyncCallbackJsonWebHandler("/admin/setup", [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!_config) {
                request->send(500, "application/json", "{\"error\":\"Config not available\"}");
                return;
            }
            if (_config->hasAdminPassword()) {
                request->send(400, "application/json", "{\"error\":\"Admin password already set. Change it from the config page.\"}");
                return;
            }
            JsonObject data = json.as<JsonObject>();
            String pw = data["password"] | String("");
            String confirm = data["confirm"] | String("");
            if (pw.length() < 4) {
                request->send(400, "application/json", "{\"error\":\"Password must be at least 4 characters.\"}");
                return;
            }
            if (pw != confirm) {
                request->send(400, "application/json", "{\"error\":\"Passwords do not match.\"}");
                return;
            }
            _config->setAdminPassword(pw);
            if (_ftpDisableCb) _ftpDisableCb();
            TempSensorMap& tempSensors = _hpController->getTempSensorMap();
            ProjectInfo* proj = _config->getProjectInfo();
            _config->updateConfig("/config.txt", tempSensors, *proj);
            Log.info("AUTH", "Admin password set via setup page");
            request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Admin password set.\"}");
        });
        _server.addHandler(adminPostHandler);

        _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (_config && !_config->hasAdminPassword()) {
                request->redirect("/admin/setup");
                return;
            }
            if (!checkAuth(request)) return;
            if (request->hasParam("format") && request->getParam("format")->value() == "json") {
                if (!_config || !_config->getProjectInfo()) {
                    request->send(500, "application/json", "{\"error\":\"Config not available\"}");
                    return;
                }
                ProjectInfo* proj = _config->getProjectInfo();
                JsonDocument doc;
                doc["wifiSSID"] = _config->getWifiSSID();
                doc["wifiPassword"] = "******";
                doc["mqttHost"] = _config->getMqttHost().toString();
                doc["mqttPort"] = _config->getMqttPort();
                doc["mqttUser"] = _config->getMqttUser();
                doc["mqttPassword"] = "******";
                doc["gmtOffsetHrs"] = proj->gmtOffsetSec / 3600.0f;
                doc["daylightOffsetHrs"] = proj->daylightOffsetSec / 3600.0f;
                doc["lowTempThreshold"] = proj->lowTempThreshold;
                doc["highSuctionTempThreshold"] = proj->highSuctionTempThreshold;
                doc["rvFail"] = proj->rvFail;
                doc["rvShortCycleSec"] = proj->rvShortCycleMs / 1000;
                doc["cntShortCycleSec"] = proj->cntShortCycleMs / 1000;
                doc["defrostMinRuntimeSec"] = proj->defrostMinRuntimeMs / 1000;
                doc["defrostExitTempF"] = proj->defrostExitTempF;
                doc["heatRuntimeThresholdMin"] = proj->heatRuntimeThresholdMs / 60000;
                doc["apFallbackMinutes"] = proj->apFallbackSeconds / 60;
                doc["maxLogSize"] = proj->maxLogSize;
                doc["maxOldLogCount"] = proj->maxOldLogCount;
                doc["tempHistoryIntervalSec"] = proj->tempHistoryIntervalSec;
                doc["adminPasswordSet"] = _config->hasAdminPassword();
                doc["theme"] = proj->theme.length() > 0 ? proj->theme : "dark";
                String json;
                serializeJson(doc, json);
                request->send(200, "application/json", json);
                return;
            }
            serveFile(request, "/config.html");
        });

        auto* configPostHandler = new AsyncCallbackJsonWebHandler("/config", [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!checkAuth(request)) return;
            if (!_config || !_config->getProjectInfo()) {
                request->send(500, "application/json", "{\"error\":\"Config not available\"}");
                return;
            }
            ProjectInfo* proj = _config->getProjectInfo();
            JsonObject data = json.as<JsonObject>();
            bool needsReboot = false;
            String errors = "";

            String newSSID = data["wifiSSID"] | _config->getWifiSSID();
            if (newSSID != _config->getWifiSSID()) {
                _config->setWifiSSID(newSSID);
                needsReboot = true;
            }

            String wifiPw = data["wifiPassword"] | String("******");
            if (wifiPw != "******" && wifiPw.length() > 0) {
                String curPw = data["curWifiPw"] | String("");
                if (curPw == _config->getWifiPassword() || _config->verifyAdminPassword(curPw)) {
                    _config->setWifiPassword(wifiPw);
                    needsReboot = true;
                } else {
                    errors += "WiFi password: current password incorrect. ";
                }
            }

            String mqttHost = data["mqttHost"] | _config->getMqttHost().toString();
            IPAddress newMqttHost;
            newMqttHost.fromString(mqttHost);
            if (newMqttHost != _config->getMqttHost()) {
                _config->setMqttHost(newMqttHost);
                needsReboot = true;
            }

            uint16_t mqttPort = data["mqttPort"] | _config->getMqttPort();
            if (mqttPort != _config->getMqttPort()) {
                _config->setMqttPort(mqttPort);
                needsReboot = true;
            }

            String mqttUser = data["mqttUser"] | _config->getMqttUser();
            if (mqttUser != _config->getMqttUser()) {
                _config->setMqttUser(mqttUser);
                needsReboot = true;
            }

            String mqttPw = data["mqttPassword"] | String("******");
            if (mqttPw != "******" && mqttPw.length() > 0) {
                String curPw = data["curMqttPw"] | String("");
                if (curPw == _config->getMqttPassword() || _config->verifyAdminPassword(curPw)) {
                    _config->setMqttPassword(mqttPw);
                    needsReboot = true;
                } else {
                    errors += "MQTT password: current password incorrect. ";
                }
            }

            // Admin password
            String adminPw = data["adminPassword"] | String("");
            if (adminPw.length() > 0) {
                if (!_config->hasAdminPassword()) {
                    // First-time setup — no current password required
                    _config->setAdminPassword(adminPw);
                    if (_ftpDisableCb) _ftpDisableCb();
                    Log.info("AUTH", "Admin password set for first time");
                } else {
                    String curAdminPw = data["curAdminPw"] | String("");
                    if (_config->verifyAdminPassword(curAdminPw)) {
                        _config->setAdminPassword(adminPw);
                        Log.info("AUTH", "Admin password changed");
                    } else {
                        errors += "Admin password: current password incorrect. ";
                    }
                }
            }

            float gmtHrs = data["gmtOffsetHrs"] | (proj->gmtOffsetSec / 3600.0f);
            float dstHrs = data["daylightOffsetHrs"] | (proj->daylightOffsetSec / 3600.0f);
            int32_t gmtOffset = (int32_t)(gmtHrs * 3600);
            int32_t dstOffset = (int32_t)(dstHrs * 3600);
            if (gmtOffset != proj->gmtOffsetSec || dstOffset != proj->daylightOffsetSec) {
                proj->gmtOffsetSec = gmtOffset;
                proj->daylightOffsetSec = dstOffset;
                setTimezone(gmtOffset, dstOffset);
                configTime(gmtOffset, dstOffset, "192.168.0.1", "time.nist.gov");
            }

            float threshold = data["lowTempThreshold"] | proj->lowTempThreshold;
            if (threshold != proj->lowTempThreshold) {
                proj->lowTempThreshold = threshold;
                _hpController->setLowTempThreshold(threshold);
            }

            float hsThreshold = data["highSuctionTempThreshold"] | proj->highSuctionTempThreshold;
            if (hsThreshold != proj->highSuctionTempThreshold) {
                proj->highSuctionTempThreshold = hsThreshold;
                _hpController->setHighSuctionTempThreshold(hsThreshold);
            }

            uint32_t rvSC = (data["rvShortCycleSec"] | (int)(proj->rvShortCycleMs / 1000)) * 1000UL;
            if (rvSC != proj->rvShortCycleMs) {
                proj->rvShortCycleMs = rvSC;
                _hpController->setRvShortCycleMs(rvSC);
            }

            uint32_t cntSC = (data["cntShortCycleSec"] | (int)(proj->cntShortCycleMs / 1000)) * 1000UL;
            if (cntSC != proj->cntShortCycleMs) {
                proj->cntShortCycleMs = cntSC;
                _hpController->setCntShortCycleMs(cntSC);
            }

            uint32_t dfMinSec = data["defrostMinRuntimeSec"] | (int)(proj->defrostMinRuntimeMs / 1000);
            uint32_t dfMinMs = dfMinSec * 1000UL;
            if (dfMinMs != proj->defrostMinRuntimeMs) {
                proj->defrostMinRuntimeMs = dfMinMs;
                _hpController->setDefrostMinRuntimeMs(dfMinMs);
            }

            float dfExitTemp = data["defrostExitTempF"] | proj->defrostExitTempF;
            if (dfExitTemp != proj->defrostExitTempF) {
                proj->defrostExitTempF = dfExitTemp;
                _hpController->setDefrostExitTempF(dfExitTemp);
            }

            uint32_t hrtMin = data["heatRuntimeThresholdMin"] | (int)(proj->heatRuntimeThresholdMs / 60000);
            if (hrtMin < 30) hrtMin = 30;
            if (hrtMin > 90) hrtMin = 90;
            uint32_t hrtMs = hrtMin * 60000UL;
            if (hrtMs != proj->heatRuntimeThresholdMs) {
                proj->heatRuntimeThresholdMs = hrtMs;
                _hpController->setHeatRuntimeThresholdMs(hrtMs);
            }

            // Clear RV Fail
            bool clearRvFail = data["clearRvFail"] | false;
            if (clearRvFail) {
                _hpController->clearRvFail();
                proj->rvFail = false;
            }

            uint32_t apMinutes = data["apFallbackMinutes"] | (proj->apFallbackSeconds / 60);
            proj->apFallbackSeconds = apMinutes * 60;

            uint32_t maxLogSize = data["maxLogSize"] | proj->maxLogSize;
            uint8_t maxOldLogCount = data["maxOldLogCount"] | proj->maxOldLogCount;
            proj->maxLogSize = maxLogSize;
            proj->maxOldLogCount = maxOldLogCount;

            uint32_t thInterval = data["tempHistoryIntervalSec"] | proj->tempHistoryIntervalSec;
            if (thInterval < 30) thInterval = 30;
            if (thInterval > 300) thInterval = 300;
            if (thInterval != proj->tempHistoryIntervalSec) {
                proj->tempHistoryIntervalSec = thInterval;
                if (_tempHistIntervalCb) _tempHistIntervalCb(thInterval);
            }

            String theme = data["theme"] | proj->theme;
            if (theme == "dark" || theme == "light") {
                proj->theme = theme;
            }

            TempSensorMap& tempSensors = _hpController->getTempSensorMap();
            bool saved = _config->updateConfig("/config.txt", tempSensors, *proj);

            String response;
            JsonDocument respDoc;
            if (!saved) {
                respDoc["error"] = "Failed to save config to SD card";
                if (errors.length() > 0) respDoc["error"] = errors + "Also failed to save.";
            } else if (errors.length() > 0) {
                respDoc["error"] = errors + "Other settings saved.";
            } else if (needsReboot) {
                respDoc["message"] = "Settings saved. Rebooting in 2 seconds...";
                respDoc["reboot"] = true;
            } else {
                respDoc["message"] = "Settings saved and applied.";
            }
            serializeJson(respDoc, response);
            request->send(200, "application/json", response);

            if (needsReboot && saved && errors.length() == 0) {
                Log.info("CONFIG", "Config changed, rebooting in 2s...");
                if (!_tDelayedReboot) {
                    _tDelayedReboot = new Task(2 * TASK_SECOND, TASK_ONCE, [this]() {
                        _shouldReboot = true;
                    }, _ts, false);
                }
                _tDelayedReboot->restartDelayed(2 * TASK_SECOND);
            }
        });
        _server.addHandler(configPostHandler);

        // Upload saves firmware to SD card (no reboot)
        _server.on("/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            request->send(200, "text/plain", _otaUploadOk ? "OK" : "FAIL: upload error");
            _otaUploadOk = false;
        }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                _otaUploadOk = false;
                _otaFile = SD.open("/firmware.new", FILE_WRITE);
                if (!_otaFile) {
                    Log.error("OTA", "Failed to open /firmware.new for writing");
                    return;
                }
                Log.info("OTA", "Saving firmware to SD (%u bytes)", total);
            }
            if (_otaFile) {
                if (_otaFile.write(data, len) != len) {
                    Log.error("OTA", "SD write failed at offset %u", index);
                    _otaFile.close();
                    SD.remove("/firmware.new");
                    _otaFile = File();
                }
            }
            if (index + len == total) {
                if (_otaFile) {
                    _otaFile.close();
                    Log.info("OTA", "Firmware saved to SD");
                    _otaUploadOk = true;
                }
            }
        });

        // Apply: backup current firmware, flash from SD, reboot
        _server.on("/apply", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            bool exists = firmwareBackupExists("/firmware.new");
            size_t size = exists ? firmwareBackupSize("/firmware.new") : 0;
            String json = "{\"exists\":" + String(exists ? "true" : "false") +
                          ",\"size\":" + String(size) + "}";
            request->send(200, "application/json", json);
        });

        _server.on("/apply", HTTP_POST, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            if (!firmwareBackupExists("/firmware.new")) {
                request->send(400, "text/plain", "FAIL: no firmware uploaded");
                return;
            }
            bool ok = applyFirmwareFromSD();
            request->send(200, "text/plain", ok ? "OK" : "FAIL");
            if (ok) {
                if (!_tDelayedReboot) {
                    _tDelayedReboot = new Task(2 * TASK_SECOND, TASK_ONCE, [this]() {
                        _shouldReboot = true;
                    }, _ts, false);
                }
                _tDelayedReboot->restartDelayed(2 * TASK_SECOND);
            }
        });

        _server.on("/revert", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            bool exists = firmwareBackupExists();
            size_t size = exists ? firmwareBackupSize() : 0;
            String json = "{\"exists\":" + String(exists ? "true" : "false") +
                          ",\"size\":" + String(size) + "}";
            request->send(200, "application/json", json);
        });

        _server.on("/revert", HTTP_POST, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            if (!firmwareBackupExists()) {
                request->send(400, "text/plain", "FAIL: no backup");
                return;
            }
            bool ok = revertFirmwareFromSD();
            request->send(200, "text/plain", ok ? "OK" : "FAIL");
            if (ok) {
                if (!_tDelayedReboot) {
                    _tDelayedReboot = new Task(2 * TASK_SECOND, TASK_ONCE, [this]() {
                        _shouldReboot = true;
                    }, _ts, false);
                }
                _tDelayedReboot->restartDelayed(2 * TASK_SECOND);
            }
        });

        // Reboot endpoint
        _server.on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            request->send(200, "text/plain", "OK");
            Log.info("WEB", "Reboot requested, rebooting in 2s...");
            if (!_tDelayedReboot) {
                _tDelayedReboot = new Task(2 * TASK_SECOND, TASK_ONCE, [this]() {
                    _shouldReboot = true;
                }, _ts, false);
            }
            _tDelayedReboot->restartDelayed(2 * TASK_SECOND);
        });

        // FTP control endpoints (HTTP fallback)
        _server.on("/ftp", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            String json = _ftpStatusCb ? _ftpStatusCb() : "{\"active\":false}";
            request->send(200, "application/json", json);
        });

        auto* ftpPostHandler = new AsyncCallbackJsonWebHandler("/ftp", [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!checkAuth(request)) return;
            JsonObject data = json.as<JsonObject>();
            int duration = data["duration"] | 0;
            if (duration > 0 && _ftpEnableCb) {
                _ftpEnableCb(duration);
                request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"FTP enabled\"}");
            } else if (_ftpDisableCb) {
                _ftpDisableCb();
                request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"FTP disabled\"}");
            } else {
                request->send(500, "application/json", "{\"error\":\"FTP control not available\"}");
            }
        });
        _server.addHandler(ftpPostHandler);
        // WiFi scan/test endpoints (HTTP only — WiFi test disrupts HTTPS)
        _server.on("/wifi/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            serveFile(request, "/wifi.html");
        });

        _server.on("/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            String json = "{\"status\":\"" + _wifiTestState + "\"";
            if (_wifiTestMessage.length() > 0) {
                json += ",\"message\":\"" + _wifiTestMessage + "\"";
            }
            json += "}";
            request->send(200, "application/json", json);
        });

        auto* wifiTestHandler = new AsyncCallbackJsonWebHandler("/wifi/test", [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!checkAuth(request)) return;
            if (_wifiTestState == "testing") {
                request->send(409, "application/json", "{\"error\":\"Test already in progress\"}");
                return;
            }
            JsonObject data = json.as<JsonObject>();
            String ssid = data["ssid"] | String("");
            String password = data["password"] | String("");
            String curPassword = data["curPassword"] | String("");

            if (ssid.length() == 0) {
                request->send(400, "application/json", "{\"error\":\"SSID required\"}");
                return;
            }

            // Verify current password (WiFi password or admin password if no WiFi configured)
            bool verified = false;
            if (_config->getWifiPassword().length() > 0) {
                verified = (curPassword == _config->getWifiPassword());
            } else if (_config->hasAdminPassword()) {
                verified = _config->verifyAdminPassword(curPassword);
            } else {
                verified = true;  // No passwords set
            }
            if (!verified) {
                request->send(403, "application/json", "{\"error\":\"Current password incorrect\"}");
                return;
            }

            // Store old credentials for revert
            _wifiOldSSID = _config->getWifiSSID();
            _wifiOldPassword = _config->getWifiPassword();
            _wifiTestNewSSID = ssid;
            _wifiTestNewPassword = password;
            _wifiTestState = "testing";
            _wifiTestMessage = "";
            _wifiTestCountdown = 15;

            // Start WiFi test task (polls every 1s for up to 15s)
            if (!_tWifiTest) {
                _tWifiTest = new Task(TASK_SECOND, TASK_FOREVER, [this]() {
                    if (_wifiTestCountdown == 15) {
                        // First iteration: initiate connection
                        extern bool _apModeActive;
                        if (_apModeActive) {
                            WiFi.mode(WIFI_AP_STA);
                        } else {
                            WiFi.disconnect(true);
                        }
                        WiFi.begin(_wifiTestNewSSID.c_str(), _wifiTestNewPassword.c_str());
                        Log.info("WiFi", "Testing connection to '%s'...", _wifiTestNewSSID.c_str());
                    }

                    _wifiTestCountdown--;

                    if (WiFi.status() == WL_CONNECTED) {
                        // Success — save new credentials
                        String newIP = WiFi.localIP().toString();
                        _config->setWifiSSID(_wifiTestNewSSID);
                        _config->setWifiPassword(_wifiTestNewPassword);
                        TempSensorMap& tempSensors = _hpController->getTempSensorMap();
                        ProjectInfo* proj = _config->getProjectInfo();
                        _config->updateConfig("/config.txt", tempSensors, *proj);
                        _wifiTestState = "success";
                        _wifiTestMessage = newIP;
                        Log.info("WiFi", "Test OK — connected to '%s' at %s. Rebooting...",
                                 _wifiTestNewSSID.c_str(), newIP.c_str());
                        _tWifiTest->disable();
                        // Schedule reboot
                        if (!_tDelayedReboot) {
                            _tDelayedReboot = new Task(3 * TASK_SECOND, TASK_ONCE, [this]() {
                                _shouldReboot = true;
                            }, _ts, false);
                        }
                        _tDelayedReboot->restartDelayed(3 * TASK_SECOND);
                        return;
                    }

                    if (_wifiTestCountdown == 0) {
                        // Timeout — revert
                        Log.warn("WiFi", "Test FAILED — could not connect to '%s'", _wifiTestNewSSID.c_str());
                        WiFi.disconnect(true);
                        extern bool _apModeActive;
                        if (_apModeActive) {
                            WiFi.mode(WIFI_AP);
                        } else {
                            WiFi.begin(_wifiOldSSID.c_str(), _wifiOldPassword.c_str());
                        }
                        _wifiTestState = "failed";
                        _wifiTestMessage = "Could not connect to " + _wifiTestNewSSID;
                        _tWifiTest->disable();
                    }
                }, _ts, false);
            }
            _tWifiTest->restartDelayed(TASK_SECOND);
            request->send(200, "application/json", "{\"status\":\"testing\"}");
        });
        _server.addHandler(wifiTestHandler);

    } else {
        // HTTPS is active — redirect HTTP pages to HTTPS
        _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/");
        });
        _server.on("/dashboard", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/dashboard");
        });
        _server.on("/log/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/log/view");
        });
        _server.on("/heap/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/heap/view");
        });
        _server.on("/admin/setup", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/admin/setup");
        });
        _server.on("/admin/setup", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/admin/setup");
        });
        _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
            String url = "https://" + String(getWiFiIP()) + "/config";
            if (request->hasParam("format")) {
                url += "?format=" + request->getParam("format")->value();
            }
            request->redirect(url);
        });
        _server.on("/config", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/config");
        });
        _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/update");
        });
        _server.on("/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/update");
        });
        _server.on("/apply", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/apply");
        });
        _server.on("/apply", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/apply");
        });
        _server.on("/revert", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/revert");
        });
        _server.on("/revert", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/revert");
        });
        _server.on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/reboot");
        });
        _server.on("/ftp", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/ftp");
        });
        _server.on("/ftp", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/ftp");
        });

        // WiFi scan/test — serve on HTTP too (WiFi test disrupts connections)
        _server.on("/wifi/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            serveFile(request, "/wifi.html");
        });
        _server.on("/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            String json = "{\"status\":\"" + _wifiTestState + "\"";
            if (_wifiTestMessage.length() > 0) {
                json += ",\"message\":\"" + _wifiTestMessage + "\"";
            }
            json += "}";
            request->send(200, "application/json", json);
        });
        auto* wifiTestHandlerHttps = new AsyncCallbackJsonWebHandler("/wifi/test", [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!checkAuth(request)) return;
            if (_wifiTestState == "testing") {
                request->send(409, "application/json", "{\"error\":\"Test already in progress\"}");
                return;
            }
            JsonObject data = json.as<JsonObject>();
            String ssid = data["ssid"] | String("");
            String password = data["password"] | String("");
            String curPassword = data["curPassword"] | String("");

            if (ssid.length() == 0) {
                request->send(400, "application/json", "{\"error\":\"SSID required\"}");
                return;
            }

            bool verified = false;
            if (_config->getWifiPassword().length() > 0) {
                verified = (curPassword == _config->getWifiPassword());
            } else if (_config->hasAdminPassword()) {
                verified = _config->verifyAdminPassword(curPassword);
            } else {
                verified = true;
            }
            if (!verified) {
                request->send(403, "application/json", "{\"error\":\"Current password incorrect\"}");
                return;
            }

            _wifiOldSSID = _config->getWifiSSID();
            _wifiOldPassword = _config->getWifiPassword();
            _wifiTestNewSSID = ssid;
            _wifiTestNewPassword = password;
            _wifiTestState = "testing";
            _wifiTestMessage = "";
            _wifiTestCountdown = 15;

            if (!_tWifiTest) {
                _tWifiTest = new Task(TASK_SECOND, TASK_FOREVER, [this]() {
                    if (_wifiTestCountdown == 15) {
                        extern bool _apModeActive;
                        if (_apModeActive) {
                            WiFi.mode(WIFI_AP_STA);
                        } else {
                            WiFi.disconnect(true);
                        }
                        WiFi.begin(_wifiTestNewSSID.c_str(), _wifiTestNewPassword.c_str());
                        Log.info("WiFi", "Testing connection to '%s'...", _wifiTestNewSSID.c_str());
                    }
                    _wifiTestCountdown--;
                    if (WiFi.status() == WL_CONNECTED) {
                        String newIP = WiFi.localIP().toString();
                        _config->setWifiSSID(_wifiTestNewSSID);
                        _config->setWifiPassword(_wifiTestNewPassword);
                        TempSensorMap& tempSensors = _hpController->getTempSensorMap();
                        ProjectInfo* proj = _config->getProjectInfo();
                        _config->updateConfig("/config.txt", tempSensors, *proj);
                        _wifiTestState = "success";
                        _wifiTestMessage = newIP;
                        Log.info("WiFi", "Test OK — connected to '%s' at %s. Rebooting...",
                                 _wifiTestNewSSID.c_str(), newIP.c_str());
                        _tWifiTest->disable();
                        if (!_tDelayedReboot) {
                            _tDelayedReboot = new Task(3 * TASK_SECOND, TASK_ONCE, [this]() {
                                _shouldReboot = true;
                            }, _ts, false);
                        }
                        _tDelayedReboot->restartDelayed(3 * TASK_SECOND);
                        return;
                    }
                    if (_wifiTestCountdown == 0) {
                        Log.warn("WiFi", "Test FAILED — could not connect to '%s'", _wifiTestNewSSID.c_str());
                        WiFi.disconnect(true);
                        extern bool _apModeActive;
                        if (_apModeActive) {
                            WiFi.mode(WIFI_AP);
                        } else {
                            WiFi.begin(_wifiOldSSID.c_str(), _wifiOldPassword.c_str());
                        }
                        _wifiTestState = "failed";
                        _wifiTestMessage = "Could not connect to " + _wifiTestNewSSID;
                        _tWifiTest->disable();
                    }
                }, _ts, false);
            }
            _tWifiTest->restartDelayed(TASK_SECOND);
            request->send(200, "application/json", "{\"status\":\"testing\"}");
        });
        _server.addHandler(wifiTestHandlerHttps);
    }
}

// ---- HTTPS server (delegates to HttpsServer.cpp to avoid header conflicts) ----

bool WebHandler::beginSecure(const uint8_t* cert, size_t certLen, const uint8_t* key, size_t keyLen) {
    _httpsCtx.config = _config;
    _httpsCtx.hpController = _hpController;
    _httpsCtx.scheduler = _ts;
    _httpsCtx.shouldReboot = &_shouldReboot;
    _httpsCtx.delayedReboot = &_tDelayedReboot;
    _httpsCtx.gmtOffsetSec = &_gmtOffsetSec;
    _httpsCtx.daylightOffsetSec = &_daylightOffsetSec;
    _httpsCtx.ftpEnableCb = _ftpEnableCb;
    _httpsCtx.ftpDisableCb = _ftpDisableCb;
    _httpsCtx.ftpActive = _ftpActivePtr;
    _httpsCtx.ftpStopTime = _ftpStopTimePtr;
    _httpsCtx.wifiTestState = &_wifiTestState;
    _httpsCtx.wifiTestMessage = &_wifiTestMessage;
    _httpsCtx.wifiTestNewSSID = &_wifiTestNewSSID;
    _httpsCtx.wifiTestNewPassword = &_wifiTestNewPassword;
    _httpsCtx.wifiOldSSID = &_wifiOldSSID;
    _httpsCtx.wifiOldPassword = &_wifiOldPassword;
    _httpsCtx.wifiTestCountdown = &_wifiTestCountdown;
    _httpsCtx.wifiTestTask = &_tWifiTest;
    _httpsCtx.tempHistory = _tempHistory;
    _httpsCtx.tempHistIntervalCb = _tempHistIntervalCb;

    _httpsServer = httpsStart(cert, certLen, key, keyLen, &_httpsCtx);
    return _httpsServer != nullptr;
}
