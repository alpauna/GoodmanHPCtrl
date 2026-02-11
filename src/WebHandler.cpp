#include "WebHandler.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "TempSensor.h"
#include "OtaUtils.h"

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
        request->requestAuthentication();
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
        request->requestAuthentication();
        return false;
    }

    String password = credentials.substring(colonIdx + 1);
    if (_config->verifyAdminPassword(password)) {
        return true;
    }

    request->requestAuthentication();
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

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/index.html");
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

    // /config and /update are served via HTTPS when certificates are available.
    // If HTTPS is active, these redirect HTTP->HTTPS. If no certs, they serve directly on HTTP.
    // The actual handlers are registered in registerHttpsHandlers() or as fallbacks below.
    if (!_httpsServer) {
        // No HTTPS — serve /update and /config directly on HTTP (fallback)
        _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            serveFile(request, "/update.html");
        });

        _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
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
                doc["maxLogSize"] = proj->maxLogSize;
                doc["maxOldLogCount"] = proj->maxOldLogCount;
                doc["adminPasswordSet"] = _config->hasAdminPassword();
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
                if (curPw == _config->getWifiPassword()) {
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
                if (curPw == _config->getMqttPassword()) {
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

            uint32_t maxLogSize = data["maxLogSize"] | proj->maxLogSize;
            uint8_t maxOldLogCount = data["maxOldLogCount"] | proj->maxOldLogCount;
            proj->maxLogSize = maxLogSize;
            proj->maxOldLogCount = maxOldLogCount;

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

        _server.on("/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
            if (!checkAuth(request)) return;
            _shouldReboot = !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", _shouldReboot ? "OK" : "FAIL");
            response->addHeader("Connection", "close");
            request->send(response);
        }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                backupFirmwareToSD();
                Log.info("OTA", "Update Start: %u bytes", total);
                if (!Update.begin(total)) {
                    Log.error("OTA", "Update.begin failed");
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (index + len == total) {
                if (Update.end(true)) {
                    Log.info("OTA", "OTA Update Successful");
                } else {
                    Log.error("OTA", "OTA Update Failed");
                    Update.printError(Serial);
                }
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
            if (ok) _shouldReboot = true;
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
    } else {
        // HTTPS is active — redirect HTTP /config and /update to HTTPS
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
        _server.on("/revert", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/revert");
        });
        _server.on("/revert", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/revert");
        });
        _server.on("/ftp", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/ftp");
        });
        _server.on("/ftp", HTTP_POST, [this](AsyncWebServerRequest *request) {
            request->redirect("https://" + String(getWiFiIP()) + "/ftp");
        });
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

    _httpsServer = httpsStart(cert, certLen, key, keyLen, &_httpsCtx);
    return _httpsServer != nullptr;
}
