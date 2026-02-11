// Separate compilation unit for ESP-IDF HTTPS server.
// esp_https_server.h and ESPAsyncWebServer.h define conflicting HTTP method
// enums (HTTP_PUT, HTTP_OPTIONS, HTTP_PATCH) and cannot coexist in the same TU.

#include <Arduino.h>
#include <esp_https_server.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TaskSchedulerDeclarations.h>
#include "SdFat.h"
#include "HttpsServer.h"
#include "Config.h"
#include "GoodmanHP.h"
#include "Logger.h"

// --- SD card file serving helper ---

static esp_err_t serveFileHttps(httpd_req_t* req, const char* sdPath) {
    FsFile file;
    if (!file.open(sdPath, O_RDONLY)) {
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
        doc["maxLogSize"] = proj->maxLogSize;
        doc["maxOldLogCount"] = proj->maxOldLogCount;
        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    return serveFileHttps(req, "/www/config.html");
}

static esp_err_t configPostHandler(httpd_req_t* req) {
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
        if (curPw == ctx->config->getWifiPassword()) {
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
        if (curPw == ctx->config->getMqttPassword()) {
            ctx->config->setMqttPassword(mqttPw);
            needsReboot = true;
        } else {
            errors += "MQTT password: current password incorrect. ";
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

    // Logging (live)
    uint32_t maxLogSize = data["maxLogSize"] | proj->maxLogSize;
    uint8_t maxOldLogCount = data["maxOldLogCount"] | proj->maxOldLogCount;
    proj->maxLogSize = maxLogSize;
    proj->maxOldLogCount = maxOldLogCount;

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
    return serveFileHttps(req, "/www/update.html");
}

static esp_err_t updatePostHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send(req, "FAIL: no data", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
        Log.error("OTA", "Update.begin failed");
        Update.printError(Serial);
        httpd_resp_send(req, "FAIL: begin error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    Log.info("OTA", "HTTPS OTA Update Start (%d bytes)", remaining);

    char buf[1024];
    while (remaining > 0) {
        int toRead = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int ret = httpd_req_recv(req, buf, toRead);
        if (ret <= 0) {
            Update.abort();
            httpd_resp_send(req, "FAIL: receive error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (Update.write((uint8_t*)buf, ret) != (size_t)ret) {
            Update.printError(Serial);
            Update.abort();
            httpd_resp_send(req, "FAIL: write error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        remaining -= ret;
    }

    if (Update.end(true)) {
        Log.info("OTA", "HTTPS OTA Update Successful");
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        *(ctx->shouldReboot) = true;
    } else {
        Log.error("OTA", "HTTPS OTA Update Failed");
        Update.printError(Serial);
        httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
    }
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

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_ssl_start(&server, &cfg);
    if (err != ESP_OK) {
        Log.error("HTTPS", "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return nullptr;
    }

    // Register URI handlers
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

    Log.info("HTTPS", "HTTPS server started on port 443");
    return (HttpsServerHandle)server;
}
