#include "MQTTHandler.h"

MQTTHandler::MQTTHandler(Scheduler* ts)
    : _ts(ts), _tReconnect(nullptr) {}

void MQTTHandler::begin(const IPAddress& host, uint16_t port,
                         const String& user, const String& password) {
    _client.onConnect([this](bool sessionPresent) {
        this->onConnect(sessionPresent);
    });
    _client.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
        this->onDisconnect(reason);
    });
    _client.onSubscribe([this](uint16_t packetId, uint8_t qos) {
        this->onSubscribe(packetId, qos);
    });
    _client.onUnsubscribe([this](uint16_t packetId) {
        this->onUnsubscribe(packetId);
    });
    _client.onMessage([this](char* topic, char* payload,
                             AsyncMqttClientMessageProperties properties,
                             size_t len, size_t index, size_t total) {
        this->onMessage(topic, payload, properties, len, index, total);
    });
    _client.onPublish([this](uint16_t packetId) {
        this->onPublish(packetId);
    });
    _client.setServer(host, port);
    _client.setCredentials(user.c_str(), password.c_str());

    _tReconnect = new Task(10 * TASK_SECOND, TASK_FOREVER, [this]() {
        if (_client.connected()) {
            _tReconnect->disable();
            return;
        }
        Log.info("MQTT", "Connecting to MQTT...");
        _client.connect();
    }, _ts, false);
}

void MQTTHandler::startReconnect() {
    if (_tReconnect) {
        _tReconnect->enableDelayed();
    }
}

void MQTTHandler::stopReconnect() {
    if (_tReconnect) {
        _tReconnect->disable();
    }
}

void MQTTHandler::disconnect() {
    _client.disconnect();
}

void MQTTHandler::onConnect(bool sessionPresent) {
    Log.info("MQTT", "Connected to MQTT (session present: %s)", sessionPresent ? "yes" : "no");
    Log.info("MQTT", "IP: %s", WiFi.localIP().toString().c_str());
    if (_tReconnect) {
        _tReconnect->disable();
    }
}

void MQTTHandler::onDisconnect(AsyncMqttClientDisconnectReason reason) {
    Log.warn("MQTT", "Disconnected from MQTT (reason: %d)", (int)reason);

    if (reason == AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT) {
        Log.error("MQTT", "Bad server fingerprint");
    }

    if (WiFi.isConnected()) {
        startReconnect();
    }
}

void MQTTHandler::onSubscribe(uint16_t packetId, uint8_t qos) {
    Serial.println("Subscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
    Serial.print("  qos: ");
    Serial.println(qos);
}

void MQTTHandler::onUnsubscribe(uint16_t packetId) {
    Serial.println("Unsubscribe acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
}

void MQTTHandler::onMessage(char* topic, char* payload,
                             AsyncMqttClientMessageProperties properties,
                             size_t len, size_t index, size_t total) {
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

void MQTTHandler::onPublish(uint16_t packetId) {
    Serial.println("Publish acknowledged.");
    Serial.print("  packetId: ");
    Serial.println(packetId);
}
