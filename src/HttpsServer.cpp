// Separate compilation unit for ESP-IDF HTTPS server.
// esp_https_server.h and ESPAsyncWebServer.h define conflicting HTTP method
// enums (HTTP_PUT, HTTP_OPTIONS, HTTP_PATCH) and cannot coexist in the same TU.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TaskSchedulerDeclarations.h>
#include <SD.h>
#include "mbedtls/base64.h"
#include "HttpsServer.h"
#include "OtaUtils.h"
#include "Config.h"
#include "GoodmanHP.h"
#include "TempHistory.h"
#include "Logger.h"

extern uint8_t getCpuLoadCore0();
extern uint8_t getCpuLoadCore1();
extern bool _apModeActive;
extern const char compile_date[];

// --- HTTPS Basic Auth helper ---

static bool checkHttpsAuth(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    if (!ctx->config || !ctx->config->hasAdminPassword()) return true;

    size_t authLen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (authLen == 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GoodmanHP\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    char* authBuf = (char*)malloc(authLen + 1);
    if (!authBuf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return false;
    }
    httpd_req_get_hdr_value_str(req, "Authorization", authBuf, authLen + 1);

    // Expect "Basic <base64>"
    if (strncmp(authBuf, "Basic ", 6) != 0) {
        free(authBuf);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GoodmanHP\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    const char* b64 = authBuf + 6;
    size_t b64Len = strlen(b64);
    size_t decodedLen = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedLen, (const uint8_t*)b64, b64Len);
    uint8_t* decoded = (uint8_t*)malloc(decodedLen + 1);
    if (!decoded) { free(authBuf); return false; }
    mbedtls_base64_decode(decoded, decodedLen + 1, &decodedLen, (const uint8_t*)b64, b64Len);
    decoded[decodedLen] = '\0';
    free(authBuf);

    // Split at ':'
    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        free(decoded);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GoodmanHP\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    String password = String(colon + 1);
    free(decoded);

    if (ctx->config->verifyAdminPassword(password)) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"GoodmanHP\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

// --- SD card file serving helper ---

static esp_err_t serveFileHttps(httpd_req_t* req, const char* sdPath) {
    fs::File file = SD.open(sdPath, FILE_READ);
    if (!file) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    size_t fileSize = file.size();
    char* buf = (char*)ps_malloc(fileSize + 1);
    if (!buf) {
        file.close();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    file.read((uint8_t*)buf, fileSize);
    buf[fileSize] = '\0';
    file.close();

    const char* contentType = "text/html";
    if (strstr(sdPath, ".css")) contentType = "text/css";
    else if (strstr(sdPath, ".js")) contentType = "application/javascript";
    else if (strstr(sdPath, ".json")) contentType = "application/json";

    httpd_resp_set_type(req, contentType);
    httpd_resp_send(req, buf, fileSize);
    free(buf);
    return ESP_OK;
}

// --- ESP-IDF httpd handler callbacks ---

static esp_err_t configGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    // Gate: redirect to admin setup if no admin password set
    if (ctx->config && !ctx->config->hasAdminPassword()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/admin/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (!checkHttpsAuth(req)) return ESP_OK;

    // Check for ?format=json
    size_t qLen = httpd_req_get_url_query_len(req);
    bool wantJson = false;
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "format", val, sizeof(val)) == ESP_OK) {
                wantJson = (strcmp(val, "json") == 0);
            }
        }
        free(qBuf);
    }

    if (wantJson) {
        if (!ctx->config || !ctx->config->getProjectInfo()) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ProjectInfo* proj = ctx->config->getProjectInfo();
        JsonDocument doc;
        doc["wifiSSID"] = ctx->config->getWifiSSID();
        doc["wifiPassword"] = "******";
        doc["mqttHost"] = ctx->config->getMqttHost().toString();
        doc["mqttPort"] = ctx->config->getMqttPort();
        doc["mqttUser"] = ctx->config->getMqttUser();
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
        doc["apFallbackMinutes"] = proj->apFallbackSeconds / 60;
        doc["maxLogSize"] = proj->maxLogSize;
        doc["maxOldLogCount"] = proj->maxOldLogCount;
        doc["tempHistoryIntervalSec"] = proj->tempHistoryIntervalSec;
        doc["adminPasswordSet"] = ctx->config->hasAdminPassword();
        doc["theme"] = proj->theme.length() > 0 ? proj->theme : "dark";
        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    return serveFileHttps(req, "/www/config.html");
}

static esp_err_t configPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 4096) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Receive failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    DeserializationError err = deserializeJson(data, body);
    free(body);
    if (err) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!ctx->config || !ctx->config->getProjectInfo()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ProjectInfo* proj = ctx->config->getProjectInfo();
    bool needsReboot = false;
    String errors = "";

    // WiFi SSID
    String newSSID = data["wifiSSID"] | ctx->config->getWifiSSID();
    if (newSSID != ctx->config->getWifiSSID()) {
        ctx->config->setWifiSSID(newSSID);
        needsReboot = true;
    }

    // WiFi password
    String wifiPw = data["wifiPassword"] | String("******");
    if (wifiPw != "******" && wifiPw.length() > 0) {
        String curPw = data["curWifiPw"] | String("");
        if (curPw == ctx->config->getWifiPassword() || ctx->config->verifyAdminPassword(curPw)) {
            ctx->config->setWifiPassword(wifiPw);
            needsReboot = true;
        } else {
            errors += "WiFi password: current password incorrect. ";
        }
    }

    // MQTT host
    String mqttHost = data["mqttHost"] | ctx->config->getMqttHost().toString();
    IPAddress newMqttHost;
    newMqttHost.fromString(mqttHost);
    if (newMqttHost != ctx->config->getMqttHost()) {
        ctx->config->setMqttHost(newMqttHost);
        needsReboot = true;
    }

    uint16_t mqttPort = data["mqttPort"] | ctx->config->getMqttPort();
    if (mqttPort != ctx->config->getMqttPort()) {
        ctx->config->setMqttPort(mqttPort);
        needsReboot = true;
    }

    String mqttUser = data["mqttUser"] | ctx->config->getMqttUser();
    if (mqttUser != ctx->config->getMqttUser()) {
        ctx->config->setMqttUser(mqttUser);
        needsReboot = true;
    }

    // MQTT password
    String mqttPw = data["mqttPassword"] | String("******");
    if (mqttPw != "******" && mqttPw.length() > 0) {
        String curPw = data["curMqttPw"] | String("");
        if (curPw == ctx->config->getMqttPassword() || ctx->config->verifyAdminPassword(curPw)) {
            ctx->config->setMqttPassword(mqttPw);
            needsReboot = true;
        } else {
            errors += "MQTT password: current password incorrect. ";
        }
    }

    // Admin password
    String adminPw = data["adminPassword"] | String("");
    if (adminPw.length() > 0) {
        if (!ctx->config->hasAdminPassword()) {
            ctx->config->setAdminPassword(adminPw);
            if (ctx->ftpDisableCb) ctx->ftpDisableCb();
            Log.info("AUTH", "Admin password set for first time (HTTPS)");
        } else {
            String curAdminPw = data["curAdminPw"] | String("");
            if (ctx->config->verifyAdminPassword(curAdminPw)) {
                ctx->config->setAdminPassword(adminPw);
                Log.info("AUTH", "Admin password changed (HTTPS)");
            } else {
                errors += "Admin password: current password incorrect. ";
            }
        }
    }

    // Timezone (live)
    float gmtHrs = data["gmtOffsetHrs"] | (proj->gmtOffsetSec / 3600.0f);
    float dstHrs = data["daylightOffsetHrs"] | (proj->daylightOffsetSec / 3600.0f);
    int32_t gmtOffset = (int32_t)(gmtHrs * 3600);
    int32_t dstOffset = (int32_t)(dstHrs * 3600);
    if (gmtOffset != proj->gmtOffsetSec || dstOffset != proj->daylightOffsetSec) {
        proj->gmtOffsetSec = gmtOffset;
        proj->daylightOffsetSec = dstOffset;
        *(ctx->gmtOffsetSec) = gmtOffset;
        *(ctx->daylightOffsetSec) = dstOffset;
        configTime(gmtOffset, dstOffset, "192.168.0.1", "time.nist.gov");
    }

    // Low temp threshold (live)
    float threshold = data["lowTempThreshold"] | proj->lowTempThreshold;
    if (threshold != proj->lowTempThreshold) {
        proj->lowTempThreshold = threshold;
        ctx->hpController->setLowTempThreshold(threshold);
    }

    // High suction temp threshold (live)
    float hsThreshold = data["highSuctionTempThreshold"] | proj->highSuctionTempThreshold;
    if (hsThreshold != proj->highSuctionTempThreshold) {
        proj->highSuctionTempThreshold = hsThreshold;
        ctx->hpController->setHighSuctionTempThreshold(hsThreshold);
    }

    // Short cycle times (live)
    uint32_t rvSC = (data["rvShortCycleSec"] | (int)(proj->rvShortCycleMs / 1000)) * 1000UL;
    if (rvSC != proj->rvShortCycleMs) {
        proj->rvShortCycleMs = rvSC;
        ctx->hpController->setRvShortCycleMs(rvSC);
    }

    uint32_t cntSC = (data["cntShortCycleSec"] | (int)(proj->cntShortCycleMs / 1000)) * 1000UL;
    if (cntSC != proj->cntShortCycleMs) {
        proj->cntShortCycleMs = cntSC;
        ctx->hpController->setCntShortCycleMs(cntSC);
    }

    uint32_t dfMinSec = data["defrostMinRuntimeSec"] | (int)(proj->defrostMinRuntimeMs / 1000);
    uint32_t dfMinMs = dfMinSec * 1000UL;
    if (dfMinMs != proj->defrostMinRuntimeMs) {
        proj->defrostMinRuntimeMs = dfMinMs;
        ctx->hpController->setDefrostMinRuntimeMs(dfMinMs);
    }

    float dfExitTemp = data["defrostExitTempF"] | proj->defrostExitTempF;
    if (dfExitTemp != proj->defrostExitTempF) {
        proj->defrostExitTempF = dfExitTemp;
        ctx->hpController->setDefrostExitTempF(dfExitTemp);
    }

    // Clear RV Fail
    bool clearRvFail = data["clearRvFail"] | false;
    if (clearRvFail) {
        ctx->hpController->clearRvFail();
        proj->rvFail = false;
    }

    // AP fallback timeout (live)
    uint32_t apMinutes = data["apFallbackMinutes"] | (proj->apFallbackSeconds / 60);
    proj->apFallbackSeconds = apMinutes * 60;

    // Logging (live)
    uint32_t maxLogSize = data["maxLogSize"] | proj->maxLogSize;
    uint8_t maxOldLogCount = data["maxOldLogCount"] | proj->maxOldLogCount;
    proj->maxLogSize = maxLogSize;
    proj->maxOldLogCount = maxOldLogCount;

    // Temp history interval (live)
    uint32_t thInterval = data["tempHistoryIntervalSec"] | proj->tempHistoryIntervalSec;
    if (thInterval < 30) thInterval = 30;
    if (thInterval > 300) thInterval = 300;
    if (thInterval != proj->tempHistoryIntervalSec) {
        proj->tempHistoryIntervalSec = thInterval;
        if (ctx->tempHistIntervalCb) ctx->tempHistIntervalCb(thInterval);
    }

    // UI theme
    String theme = data["theme"] | proj->theme;
    if (theme == "dark" || theme == "light") {
        proj->theme = theme;
    }

    // Save to SD card
    TempSensorMap& tempSensors = ctx->hpController->getTempSensorMap();
    bool saved = ctx->config->updateConfig("/config.txt", tempSensors, *proj);

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
    String response;
    serializeJson(respDoc, response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());

    if (needsReboot && saved && errors.length() == 0) {
        Log.info("CONFIG", "Config changed via HTTPS, rebooting in 2s...");
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    }
    return ESP_OK;
}

static esp_err_t updateGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    return serveFileHttps(req, "/www/update.html");
}

static esp_err_t updatePostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;

    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send(req, "FAIL: no data", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    File fw = SD.open("/firmware.new", FILE_WRITE);
    if (!fw) {
        httpd_resp_send(req, "FAIL: SD open error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    Log.info("OTA", "Saving firmware to SD (%d bytes)", remaining);

    char buf[1024];
    while (remaining > 0) {
        int toRead = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int ret = httpd_req_recv(req, buf, toRead);
        if (ret <= 0) {
            fw.close();
            SD.remove("/firmware.new");
            httpd_resp_send(req, "FAIL: receive error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (fw.write((uint8_t*)buf, ret) != (size_t)ret) {
            fw.close();
            SD.remove("/firmware.new");
            httpd_resp_send(req, "FAIL: SD write error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        remaining -= ret;
    }

    fw.close();
    Log.info("OTA", "Firmware saved to SD");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t applyGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    bool exists = firmwareBackupExists("/firmware.new");
    size_t size = exists ? firmwareBackupSize("/firmware.new") : 0;
    String json = "{\"exists\":" + String(exists ? "true" : "false") +
                  ",\"size\":" + String(size) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t applyPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!firmwareBackupExists("/firmware.new")) {
        httpd_resp_send(req, "FAIL: no firmware uploaded", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (applyFirmwareFromSD()) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    } else {
        httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// --- FTP control handlers ---

static esp_err_t ftpGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    bool active = ctx->ftpActive ? *(ctx->ftpActive) : false;
    unsigned long stopTime = ctx->ftpStopTime ? *(ctx->ftpStopTime) : 0;
    int remainingMin = 0;
    if (active && stopTime > 0) {
        unsigned long now = millis();
        if (stopTime > now) {
            remainingMin = (int)((stopTime - now) / 60000) + 1;
        }
    }

    String json = "{\"active\":" + String(active ? "true" : "false") +
                  ",\"remainingMinutes\":" + String(remainingMin) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t ftpPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 256) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    if (deserializeJson(data, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    int duration = data["duration"] | 0;
    httpd_resp_set_type(req, "application/json");
    if (duration > 0 && ctx->ftpEnableCb) {
        ctx->ftpEnableCb(duration);
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"FTP enabled\"}", HTTPD_RESP_USE_STRLEN);
    } else if (ctx->ftpDisableCb) {
        ctx->ftpDisableCb();
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"FTP disabled\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"error\":\"FTP control not available\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// --- Temps handler ---

static esp_err_t tempsGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    String json = "[";
    bool firstTime = true;
    for (const auto& m : ctx->hpController->getTempSensorMap()) {
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Consolidated temp history from PSRAM ---

static esp_err_t tempsHistoryAllGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    if (!ctx->tempHistory) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Temp history not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int range = 24;
    size_t qLen = httpd_req_get_url_query_len(req);
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "range", val, sizeof(val)) == ESP_OK) {
                range = atoi(val);
                if (range < 1) range = 1;
                if (range > 168) range = 168;
            }
        }
        free(qBuf);
    }

    struct tm ti;
    if (!getLocalTime(&ti, 0)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Time not synced\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    time_t now = mktime(&ti);
    uint32_t sinceEpoch = (uint32_t)(now - (time_t)range * 3600);

    int maxPerSensor = TempHistory::MAX_SAMPLES;
    TempSample* buf = (TempSample*)ps_malloc(maxPerSensor * sizeof(TempSample));
    if (!buf) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    // Send response in chunks to avoid huge single allocation
    httpd_resp_send_chunk(req, "{\"sensors\":{", 12);

    for (int s = 0; s < TempHistory::MAX_SENSORS; s++) {
        // Sensor key prefix
        String prefix = (s > 0 ? ",\"" : "\"");
        prefix += TempHistory::sensorDirs[s];
        prefix += "\":[";
        httpd_resp_send_chunk(req, prefix.c_str(), prefix.length());

        int count = ctx->tempHistory->getSamples(s, sinceEpoch, buf, maxPerSensor);

        // Decimate to max 500 points
        int step = 1;
        if (count > 500) step = (count + 499) / 500;

        bool first = true;
        // Build in chunks of ~50 points to avoid huge strings
        String chunk;
        chunk.reserve(2048);
        int pointsInChunk = 0;

        for (int i = 0; i < count; i += step) {
            if (!first) chunk += ",";
            chunk += "[";
            chunk += String(buf[i].epoch);
            chunk += ",";
            chunk += String(buf[i].temp, 1);
            chunk += "]";
            first = false;
            pointsInChunk++;

            if (pointsInChunk >= 50) {
                httpd_resp_send_chunk(req, chunk.c_str(), chunk.length());
                chunk = "";
                pointsInChunk = 0;
            }
        }
        if (chunk.length() > 0) {
            httpd_resp_send_chunk(req, chunk.c_str(), chunk.length());
        }

        httpd_resp_send_chunk(req, "]", 1);
    }

    httpd_resp_send_chunk(req, "}}", 2);
    httpd_resp_send_chunk(req, NULL, 0);  // End chunked response

    free(buf);
    return ESP_OK;
}

// --- Temps history handler ---

static esp_err_t tempsHistoryGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!ctx->config || !ctx->config->isSDCardInitialized()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"SD card not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Parse query string
    size_t qLen = httpd_req_get_url_query_len(req);
    char sensorVal[16] = {};
    char dateVal[16] = {};
    bool hasSensor = false, hasDate = false;

    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            if (httpd_query_key_value(qBuf, "sensor", sensorVal, sizeof(sensorVal)) == ESP_OK)
                hasSensor = true;
            if (httpd_query_key_value(qBuf, "date", dateVal, sizeof(dateVal)) == ESP_OK)
                hasDate = true;
        }
        free(qBuf);
    }

    if (!hasSensor) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing sensor param\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Validate sensor name
    static const char* validSensors[] = {"ambient","compressor","suction","condenser","liquid"};
    bool valid = false;
    for (int i = 0; i < 5; i++) {
        if (strcmp(sensorVal, validSensors[i]) == 0) { valid = true; break; }
    }
    if (!valid) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid sensor\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char dirPath[32];
    snprintf(dirPath, sizeof(dirPath), "/temps/%s", sensorVal);

    if (hasDate) {
        if (strlen(dateVal) != 10 || dateVal[4] != '-' || dateVal[7] != '-') {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Invalid date format\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }

        char filepath[48];
        snprintf(filepath, sizeof(filepath), "%s/%s.csv", dirPath, dateVal);
        if (!SD.exists(filepath)) {
            httpd_resp_send_404(req);
            return ESP_OK;
        }

        // Stream CSV in chunks
        File f = SD.open(filepath, FILE_READ);
        if (!f) {
            httpd_resp_send_404(req);
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/csv");
        char buf[1024];
        while (f.available()) {
            int bytesRead = f.read((uint8_t*)buf, sizeof(buf));
            if (bytesRead > 0)
                httpd_resp_send_chunk(req, buf, bytesRead);
        }
        f.close();
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    // List available files
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"files\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
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

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Heap handler ---

static esp_err_t heapGetHandler(httpd_req_t* req) {
    static constexpr float MB = 1.0f / (1024.0f * 1024.0f);
    String json = "{";
    json += "\"free heap\":" + String(ESP.getFreeHeap());
    json += ",\"free psram MB\":" + String(ESP.getFreePsram() * MB);
    json += ",\"used psram MB\":" + String((ESP.getPsramSize() - ESP.getFreePsram()) * MB);
    json += ",\"cpuLoad0\":" + String(getCpuLoadCore0());
    json += ",\"cpuLoad1\":" + String(getCpuLoadCore1());
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- State handler ---

static esp_err_t stateGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    JsonDocument doc;
    doc["state"] = ctx->hpController->getStateString();

    JsonObject inputs = doc["inputs"].to<JsonObject>();
    for (auto& pair : ctx->hpController->getInputMap()) {
        if (pair.second != nullptr)
            inputs[pair.first] = pair.second->isActive();
    }

    JsonObject outputs = doc["outputs"].to<JsonObject>();
    for (auto& pair : ctx->hpController->getOutputMap()) {
        if (pair.second != nullptr)
            outputs[pair.first] = pair.second->isPinOn();
    }

    doc["heatRuntimeMin"] = ctx->hpController->getHeatRuntimeMs() / 60000UL;
    doc["defrost"] = ctx->hpController->isSoftwareDefrostActive();
    doc["lpsFault"] = ctx->hpController->isLPSFaultActive();
    doc["lowTemp"] = ctx->hpController->isLowTempActive();
    doc["compressorOverTemp"] = ctx->hpController->isCompressorOverTempActive();
    doc["suctionLowTemp"] = ctx->hpController->isSuctionLowTempActive();
    doc["startupLockout"] = ctx->hpController->isStartupLockoutActive();
    doc["startupLockoutRemainSec"] = ctx->hpController->getStartupLockoutRemainingMs() / 1000;
    doc["shortCycleProtection"] = ctx->hpController->isShortCycleProtectionActive();
    doc["rvFail"] = ctx->hpController->isRvFailActive();
    doc["highSuctionTemp"] = ctx->hpController->isHighSuctionTempActive();
    doc["defrostTransition"] = ctx->hpController->isDefrostTransitionActive();
    doc["defrostTransitionRemainSec"] = ctx->hpController->getDefrostTransitionRemainingMs() / 1000;
    doc["defrostCntPending"] = ctx->hpController->isDefrostCntPendingActive();
    doc["defrostCntPendingRemainSec"] = ctx->hpController->getDefrostCntPendingRemainingMs() / 1000;
    doc["manualOverride"] = ctx->hpController->isManualOverrideActive();
    doc["manualOverrideRemainSec"] = ctx->hpController->getManualOverrideRemainingMs() / 1000;
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
    for (const auto& m : ctx->hpController->getTempSensorMap()) {
        if (m.second != nullptr && m.second->isValid())
            temps[m.first] = m.second->getValue();
    }

    String json;
    serializeJson(doc, json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Pins handler ---

static esp_err_t pinsGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    // Check for ?format=json
    size_t qLen = httpd_req_get_url_query_len(req);
    bool wantJson = false;
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "format", val, sizeof(val)) == ESP_OK) {
                wantJson = (strcmp(val, "json") == 0);
            }
        }
        free(qBuf);
    }

    if (wantJson) {
        JsonDocument doc;
        doc["manualOverride"] = ctx->hpController->isManualOverrideActive();
        doc["manualOverrideRemainSec"] = ctx->hpController->getManualOverrideRemainingMs() / 1000;
        doc["shortCycleActive"] = ctx->hpController->isShortCycleProtectionActive();
        doc["state"] = ctx->hpController->getStateString();
        doc["defrost"] = ctx->hpController->isSoftwareDefrostActive();
        doc["defrostTransition"] = ctx->hpController->isDefrostTransitionActive();
        doc["defrostCntPending"] = ctx->hpController->isDefrostCntPendingActive();

        JsonArray inputs = doc["inputs"].to<JsonArray>();
        for (auto& pair : ctx->hpController->getInputMap()) {
            if (pair.second != nullptr) {
                JsonObject inp = inputs.add<JsonObject>();
                inp["pin"] = pair.second->getPin();
                inp["name"] = pair.first;
                inp["active"] = pair.second->isActive();
            }
        }

        JsonArray outputs = doc["outputs"].to<JsonArray>();
        for (auto& pair : ctx->hpController->getOutputMap()) {
            if (pair.second != nullptr) {
                JsonObject out = outputs.add<JsonObject>();
                out["pin"] = pair.second->getPin();
                out["name"] = pair.first;
                out["on"] = pair.second->isPinOn();
            }
        }

        JsonObject temps = doc["temps"].to<JsonObject>();
        for (const auto& m : ctx->hpController->getTempSensorMap()) {
            if (m.second != nullptr && m.second->isValid())
                temps[m.first] = m.second->getValue();
        }

        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    return serveFileHttps(req, "/www/pins.html");
}

static esp_err_t pinsPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    if (deserializeJson(data, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");

    // Toggle manual override
    if (data["manualOverride"].is<bool>()) {
        bool on = data["manualOverride"] | false;
        ctx->hpController->setManualOverride(on);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["manualOverride"] = ctx->hpController->isManualOverrideActive();
        resp["message"] = on ? "Manual override enabled (30 min timeout)" : "Manual override disabled, all outputs OFF";
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Set output state
    if (data["output"].is<const char*>()) {
        String name = data["output"] | String("");
        bool state = data["state"] | false;
        String err = ctx->hpController->setManualOutput(name, state);
        JsonDocument resp;
        if (err.length() > 0) {
            resp["error"] = err;
        } else {
            resp["status"] = "ok";
            resp["output"] = name;
            resp["state"] = state;
        }
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Force defrost
    if (data["forceDefrost"].is<bool>() && data["forceDefrost"]) {
        String err = ctx->hpController->forceDefrost();
        JsonDocument resp;
        if (err.length() > 0) {
            resp["error"] = err;
        } else {
            resp["status"] = "ok";
            resp["message"] = "Defrost initiated";
        }
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"error\":\"Invalid request\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Dashboard handler ---

static esp_err_t dashboardGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/dashboard.html");
}

static esp_err_t logViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/log.html");
}

static esp_err_t heapViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/heap.html");
}

static esp_err_t wifiViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/wifi.html");
}

static esp_err_t wifiStatusGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    String json = "{\"status\":\"" + *ctx->wifiTestState + "\"";
    if (ctx->wifiTestMessage->length() > 0) {
        json += ",\"message\":\"" + *ctx->wifiTestMessage + "\"";
    }
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

static esp_err_t wifiTestPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (*ctx->wifiTestState == "testing") {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Test already in progress\"}");
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid body\"}");
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Out of memory\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }
    free(body);

    String ssid = doc["ssid"] | String("");
    String password = doc["password"] | String("");
    String curPassword = doc["curPassword"] | String("");

    if (ssid.length() == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"SSID required\"}");
        return ESP_OK;
    }

    bool verified = false;
    if (ctx->config->getWifiPassword().length() > 0) {
        verified = (curPassword == ctx->config->getWifiPassword());
    } else if (ctx->config->hasAdminPassword()) {
        verified = ctx->config->verifyAdminPassword(curPassword);
    } else {
        verified = true;
    }
    if (!verified) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Current password incorrect\"}");
        return ESP_OK;
    }

    // Store old credentials and start test
    *ctx->wifiOldSSID = ctx->config->getWifiSSID();
    *ctx->wifiOldPassword = ctx->config->getWifiPassword();
    *ctx->wifiTestNewSSID = ssid;
    *ctx->wifiTestNewPassword = password;
    *ctx->wifiTestState = "testing";
    *ctx->wifiTestMessage = "";
    *ctx->wifiTestCountdown = 15;

    if (!*ctx->wifiTestTask) {
        *ctx->wifiTestTask = new Task(TASK_SECOND, TASK_FOREVER, [ctx]() {
            if (*ctx->wifiTestCountdown == 15) {
                extern bool _apModeActive;
                if (_apModeActive) {
                    WiFi.mode(WIFI_AP_STA);
                } else {
                    WiFi.disconnect(true);
                }
                WiFi.begin(ctx->wifiTestNewSSID->c_str(), ctx->wifiTestNewPassword->c_str());
                Log.info("WiFi", "Testing connection to '%s'...", ctx->wifiTestNewSSID->c_str());
            }
            (*ctx->wifiTestCountdown)--;
            if (WiFi.status() == WL_CONNECTED) {
                String newIP = WiFi.localIP().toString();
                ctx->config->setWifiSSID(*ctx->wifiTestNewSSID);
                ctx->config->setWifiPassword(*ctx->wifiTestNewPassword);
                TempSensorMap& tempSensors = ctx->hpController->getTempSensorMap();
                ProjectInfo* proj = ctx->config->getProjectInfo();
                ctx->config->updateConfig("/config.txt", tempSensors, *proj);
                *ctx->wifiTestState = "success";
                *ctx->wifiTestMessage = newIP;
                Log.info("WiFi", "Test OK — connected to '%s' at %s. Rebooting...",
                         ctx->wifiTestNewSSID->c_str(), newIP.c_str());
                (*ctx->wifiTestTask)->disable();
                if (!*ctx->delayedReboot) {
                    *ctx->delayedReboot = new Task(3 * TASK_SECOND, TASK_ONCE, [ctx]() {
                        *ctx->shouldReboot = true;
                    }, ctx->scheduler, false);
                }
                (*ctx->delayedReboot)->restartDelayed(3 * TASK_SECOND);
                return;
            }
            if (*ctx->wifiTestCountdown == 0) {
                Log.warn("WiFi", "Test FAILED — could not connect to '%s'", ctx->wifiTestNewSSID->c_str());
                WiFi.disconnect(true);
                extern bool _apModeActive;
                if (_apModeActive) {
                    WiFi.mode(WIFI_AP);
                } else {
                    WiFi.begin(ctx->wifiOldSSID->c_str(), ctx->wifiOldPassword->c_str());
                }
                *ctx->wifiTestState = "failed";
                *ctx->wifiTestMessage = "Could not connect to " + *ctx->wifiTestNewSSID;
                (*ctx->wifiTestTask)->disable();
            }
        }, ctx->scheduler, false);
    }
    (*ctx->wifiTestTask)->restartDelayed(TASK_SECOND);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"testing\"}");
    return ESP_OK;
}

static esp_err_t scanGetHandler(httpd_req_t* req) {
    String json = "[";
    int n = WiFi.scanComplete();
    if (n == -2) {
        WiFi.scanNetworks(true);
    } else if (n) {
        for (int i = 0; i < n; ++i) {
            if (i) json += ",";
            json += "{\"rssi\":" + String(WiFi.RSSI(i));
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// --- Theme CSS handler ---

static esp_err_t themeCssGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/theme.css");
}

// --- Root/index handler ---

static esp_err_t rootGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/index.html");
}

// --- Log handler (proxy to ring buffer) ---

static esp_err_t logGetHandler(httpd_req_t* req) {
    size_t count = Log.getRingBufferCount();
    size_t limit = count;

    size_t qLen = httpd_req_get_url_query_len(req);
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "limit", val, sizeof(val)) == ESP_OK) {
                size_t l = atoi(val);
                if (l < limit) limit = l;
            }
        }
        free(qBuf);
    }

    const auto& buffer = Log.getRingBuffer();
    size_t head = Log.getRingBufferHead();
    size_t bufSize = buffer.size();
    size_t startOffset = count - limit;

    String json = "{\"count\":" + String(limit) + ",\"entries\":[";
    for (size_t i = 0; i < limit; i++) {
        size_t idx = (head + bufSize - count + startOffset + i) % bufSize;
        if (i > 0) json += ",";
        json += "\"";
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

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Revert handlers ---

static esp_err_t revertGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    bool exists = firmwareBackupExists();
    size_t size = exists ? firmwareBackupSize() : 0;
    String json = "{\"exists\":" + String(exists ? "true" : "false") +
                  ",\"size\":" + String(size) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t revertPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!firmwareBackupExists()) {
        httpd_resp_send(req, "FAIL: no backup", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (revertFirmwareFromSD()) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    } else {
        httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t rebootPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    Log.info("HTTPS", "Reboot requested, rebooting in 2s...");
    if (!*(ctx->delayedReboot)) {
        *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
            *(ctx->shouldReboot) = true;
        }, ctx->scheduler, false);
    }
    (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    return ESP_OK;
}

// --- Admin setup handlers ---

static esp_err_t adminSetupGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/admin.html");
}

static esp_err_t adminSetupPostHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (ctx->config->hasAdminPassword()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Admin password already set. Change it from the config page.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    String pw = doc["password"] | String("");
    String confirm = doc["confirm"] | String("");

    if (pw.length() < 4) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Password must be at least 4 characters.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (pw != confirm) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Passwords do not match.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ctx->config->setAdminPassword(pw);
    if (ctx->ftpDisableCb) ctx->ftpDisableCb();
    TempSensorMap& tempSensors = ctx->hpController->getTempSensorMap();
    ProjectInfo* proj = ctx->config->getProjectInfo();
    ctx->config->updateConfig("/config.txt", tempSensors, *proj);
    Log.info("AUTH", "Admin password set via setup page (HTTPS)");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Admin password set.\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Public API ---

HttpsServerHandle httpsStart(const uint8_t* cert, size_t certLen,
                             const uint8_t* key, size_t keyLen,
                             HttpsContext* ctx) {
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.cacert_pem = cert;
    cfg.cacert_len = certLen + 1;  // PEM null terminator
    cfg.prvtkey_pem = key;
    cfg.prvtkey_len = keyLen + 1;
    cfg.port_secure = 443;
    cfg.httpd.max_uri_handlers = 30;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_ssl_start(&server, &cfg);
    if (err != ESP_OK) {
        Log.error("HTTPS", "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return nullptr;
    }

    // Register URI handlers
    httpd_uri_t themeCssGet = {
        .uri = "/theme.css",
        .method = HTTP_GET,
        .handler = themeCssGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &themeCssGet);

    httpd_uri_t rootGet = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = rootGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &rootGet);

    httpd_uri_t adminGet = {
        .uri = "/admin/setup",
        .method = HTTP_GET,
        .handler = adminSetupGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &adminGet);

    httpd_uri_t adminPost = {
        .uri = "/admin/setup",
        .method = HTTP_POST,
        .handler = adminSetupPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &adminPost);

    httpd_uri_t pinsGet = {
        .uri = "/pins",
        .method = HTTP_GET,
        .handler = pinsGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &pinsGet);

    httpd_uri_t pinsPost = {
        .uri = "/pins",
        .method = HTTP_POST,
        .handler = pinsPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &pinsPost);

    httpd_uri_t dashGet = {
        .uri = "/dashboard",
        .method = HTTP_GET,
        .handler = dashboardGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &dashGet);

    httpd_uri_t logViewGet = {
        .uri = "/log/view",
        .method = HTTP_GET,
        .handler = logViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logViewGet);

    httpd_uri_t heapViewGet = {
        .uri = "/heap/view",
        .method = HTTP_GET,
        .handler = heapViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &heapViewGet);

    httpd_uri_t wifiViewGet = {
        .uri = "/wifi/view",
        .method = HTTP_GET,
        .handler = wifiViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiViewGet);

    httpd_uri_t scanGet = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scanGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &scanGet);

    httpd_uri_t wifiStatusGet = {
        .uri = "/wifi/status",
        .method = HTTP_GET,
        .handler = wifiStatusGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiStatusGet);

    httpd_uri_t wifiTestPost = {
        .uri = "/wifi/test",
        .method = HTTP_POST,
        .handler = wifiTestPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiTestPost);

    httpd_uri_t stateGet = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = stateGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &stateGet);

    httpd_uri_t cfgGet = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = configGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &cfgGet);

    httpd_uri_t cfgPost = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = configPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &cfgPost);

    httpd_uri_t updGet = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = updateGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &updGet);

    httpd_uri_t updPost = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = updatePostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &updPost);

    httpd_uri_t appGet = {
        .uri = "/apply",
        .method = HTTP_GET,
        .handler = applyGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &appGet);

    httpd_uri_t appPost = {
        .uri = "/apply",
        .method = HTTP_POST,
        .handler = applyPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &appPost);

    httpd_uri_t revGet = {
        .uri = "/revert",
        .method = HTTP_GET,
        .handler = revertGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &revGet);

    httpd_uri_t revPost = {
        .uri = "/revert",
        .method = HTTP_POST,
        .handler = revertPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &revPost);

    httpd_uri_t rebootPost = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = rebootPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &rebootPost);

    httpd_uri_t ftpGet = {
        .uri = "/ftp",
        .method = HTTP_GET,
        .handler = ftpGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &ftpGet);

    httpd_uri_t ftpPost = {
        .uri = "/ftp",
        .method = HTTP_POST,
        .handler = ftpPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &ftpPost);

    httpd_uri_t logGet = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = logGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logGet);

    httpd_uri_t tempsGet = {
        .uri = "/temps",
        .method = HTTP_GET,
        .handler = tempsGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &tempsGet);

    httpd_uri_t tempsHistoryAllGet = {
        .uri = "/temps/history/all",
        .method = HTTP_GET,
        .handler = tempsHistoryAllGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &tempsHistoryAllGet);

    httpd_uri_t tempsHistoryGet = {
        .uri = "/temps/history",
        .method = HTTP_GET,
        .handler = tempsHistoryGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &tempsHistoryGet);

    httpd_uri_t heapGet = {
        .uri = "/heap",
        .method = HTTP_GET,
        .handler = heapGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &heapGet);

    Log.info("HTTPS", "HTTPS server started on port 443");
    return (HttpsServerHandle)server;
}
