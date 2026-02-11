// Separate compilation unit for ESP-IDF HTTPS server.
// esp_https_server.h and ESPAsyncWebServer.h define conflicting HTTP method
// enums (HTTP_PUT, HTTP_OPTIONS, HTTP_PATCH) and cannot coexist in the same TU.

#include <Arduino.h>
#include <esp_https_server.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TaskSchedulerDeclarations.h>
#include "HttpsServer.h"
#include "Config.h"
#include "GoodmanHP.h"
#include "Logger.h"

// --- Static HTML pages ---

static const char CONFIG_HTML[] PROGMEM =
    "<html><head><title>Configuration</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#f5f5f5;}"
    ".container{max-width:600px;margin:0 auto;}"
    "h1{color:#333;}"
    "fieldset{background:#fff;border:1px solid #ddd;border-radius:6px;padding:15px;margin-bottom:15px;}"
    "legend{font-weight:bold;color:#4CAF50;padding:0 8px;}"
    "label{display:block;margin:8px 0 3px;color:#555;font-size:14px;}"
    "input[type=text],input[type=password],input[type=number]{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}"
    ".tag{font-size:11px;padding:2px 6px;border-radius:3px;margin-left:6px;color:#fff;}"
    ".tag-reboot{background:#e67e22;}"
    ".tag-live{background:#27ae60;}"
    "button{background:#4CAF50;color:#fff;padding:10px 24px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:10px;}"
    "button:hover{background:#45a049;}"
    "#status{margin-top:15px;padding:10px;border-radius:4px;display:none;}"
    ".ok{background:#d4edda;color:#155724;display:block!important;}"
    ".err{background:#f8d7da;color:#721c24;display:block!important;}"
    ".info{background:#d1ecf1;color:#0c5460;display:block!important;}"
    "</style></head><body>"
    "<div class='container'>"
    "<h1>Configuration (Secure)</h1>"
    "<form id='cf' onsubmit='return save(event)'>"
    "<fieldset><legend>WiFi <span class='tag tag-reboot'>reboot</span></legend>"
    "<label>SSID</label><input type='text' id='wifiSSID'>"
    "<label>Password</label><input type='password' id='wifiPassword'>"
    "<label>Current Password (required to change)</label><input type='password' id='curWifiPw'>"
    "</fieldset>"
    "<fieldset><legend>MQTT <span class='tag tag-reboot'>reboot</span></legend>"
    "<label>Host</label><input type='text' id='mqttHost'>"
    "<label>Port</label><input type='number' id='mqttPort'>"
    "<label>User</label><input type='text' id='mqttUser'>"
    "<label>Password</label><input type='password' id='mqttPassword'>"
    "<label>Current Password (required to change)</label><input type='password' id='curMqttPw'>"
    "</fieldset>"
    "<fieldset><legend>Timezone <span class='tag tag-live'>live</span></legend>"
    "<label>GMT Offset (hours)</label><input type='number' step='0.5' id='gmtOffsetHrs'>"
    "<label>Daylight Offset (hours)</label><input type='number' step='0.5' id='daylightOffsetHrs'>"
    "</fieldset>"
    "<fieldset><legend>Low Temp Protection <span class='tag tag-live'>live</span></legend>"
    "<label>Threshold (&deg;F)</label><input type='number' step='0.1' id='lowTempThreshold'>"
    "</fieldset>"
    "<fieldset><legend>Logging <span class='tag tag-live'>live</span></legend>"
    "<label>Max Log Size (bytes)</label><input type='number' id='maxLogSize'>"
    "<label>Max Old Log Count</label><input type='number' id='maxOldLogCount'>"
    "</fieldset>"
    "<button type='submit'>Save</button>"
    "</form>"
    "<div id='status'></div>"
    "</div>"
    "<script>"
    "var fields=['wifiSSID','wifiPassword','mqttHost','mqttPort','mqttUser','mqttPassword','gmtOffsetHrs','daylightOffsetHrs','lowTempThreshold','maxLogSize','maxOldLogCount'];"
    "fetch('/config?format=json').then(r=>r.json()).then(d=>{"
    "fields.forEach(f=>{var e=document.getElementById(f);if(e)e.value=d[f]!=null?d[f]:'';});"
    "});"
    "function save(e){"
    "e.preventDefault();"
    "var d={};fields.forEach(f=>{var e=document.getElementById(f);d[f]=e?e.value:'';});"
    "d.curWifiPw=document.getElementById('curWifiPw').value;"
    "d.curMqttPw=document.getElementById('curMqttPw').value;"
    "var s=document.getElementById('status');"
    "fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(r=>r.json()).then(r=>{"
    "s.className=r.reboot?'info':r.error?'err':'ok';"
    "s.textContent=r.message||r.error||'Saved';"
    "s.style.display='block';"
    "if(r.reboot){setTimeout(()=>{s.textContent='Rebooting... page will reload in 5s';setTimeout(()=>location.reload(),5000);},1000);}"
    "document.getElementById('curWifiPw').value='';"
    "document.getElementById('curMqttPw').value='';"
    "}).catch(err=>{s.className='err';s.textContent='Error: '+err;s.style.display='block';});"
    "return false;}"
    "</script></body></html>";

static const char UPDATE_HTML[] PROGMEM =
    "<html><head><title>ESP32 OTA Update</title></head>"
    "<body><h1>ESP32 OTA Update (Secure)</h1>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update'>"
    "<input type='submit' value='Update'>"
    "</form>"
    "</body></html>";

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

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, UPDATE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
