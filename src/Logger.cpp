#include "Logger.h"
#include <stdarg.h>

Logger Log;

static const char* LEVEL_NAMES[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

Logger::Logger()
    : _level(LOG_INFO)
    , _serialEnabled(true)
    , _mqttEnabled(false)
    , _sdCardEnabled(false)
    , _mqttClient(nullptr)
    , _mqttTopic("goodman/log")
    , _sd(nullptr)
    , _logFilename("/log.txt")
    , _maxFileSize(DEFAULT_MAX_FILE_SIZE)
    , _maxRotatedFiles(DEFAULT_MAX_ROTATED_FILES)
{
}

void Logger::setLevel(Level level) {
    _level = level;
}

Logger::Level Logger::getLevel() {
    return _level;
}

const char* Logger::getLevelName(Level level) {
    if (level >= 0 && level <= LOG_DEBUG) {
        return LEVEL_NAMES[level];
    }
    return "UNKN ";
}

void Logger::setMqttClient(AsyncMqttClient* client, const char* topic) {
    _mqttClient = client;
    _mqttTopic = topic;
    _mqttEnabled = (client != nullptr);
}

void Logger::setLogFile(SdFs* sd, const char* filename, uint32_t maxFileSize, uint8_t maxRotatedFiles) {
    _sd = sd;
    _logFilename = filename;
    _maxFileSize = maxFileSize;
    _maxRotatedFiles = maxRotatedFiles;
    _sdCardEnabled = (sd != nullptr);
}

void Logger::enableSerial(bool enable) {
    _serialEnabled = enable;
}

void Logger::enableMqtt(bool enable) {
    _mqttEnabled = enable && (_mqttClient != nullptr);
}

void Logger::enableSdCard(bool enable) {
    _sdCardEnabled = enable && (_sd != nullptr);
}

bool Logger::isSerialEnabled() {
    return _serialEnabled;
}

bool Logger::isMqttEnabled() {
    return _mqttEnabled;
}

bool Logger::isSdCardEnabled() {
    return _sdCardEnabled;
}

void Logger::error(const char* tag, const char* format, ...) {
    if (_level >= LOG_ERROR) {
        va_list args;
        va_start(args, format);
        log(LOG_ERROR, tag, format, args);
        va_end(args);
    }
}

void Logger::warn(const char* tag, const char* format, ...) {
    if (_level >= LOG_WARN) {
        va_list args;
        va_start(args, format);
        log(LOG_WARN, tag, format, args);
        va_end(args);
    }
}

void Logger::info(const char* tag, const char* format, ...) {
    if (_level >= LOG_INFO) {
        va_list args;
        va_start(args, format);
        log(LOG_INFO, tag, format, args);
        va_end(args);
    }
}

void Logger::debug(const char* tag, const char* format, ...) {
    if (_level >= LOG_DEBUG) {
        va_list args;
        va_start(args, format);
        log(LOG_DEBUG, tag, format, args);
        va_end(args);
    }
}

void Logger::log(Level level, const char* tag, const char* format, va_list args) {
    char msgBuffer[384];
    vsnprintf(msgBuffer, sizeof(msgBuffer), format, args);

    unsigned long ms = millis();
    snprintf(_buffer, sizeof(_buffer), "[%s] %8lums [%s] %s",
             getLevelName(level), ms, tag, msgBuffer);

    if (_serialEnabled) {
        writeToSerial(_buffer);
    }
    if (_mqttEnabled) {
        writeToMqtt(_buffer);
    }
    if (_sdCardEnabled) {
        writeToSdCard(_buffer);
    }
}

void Logger::writeToSerial(const char* msg) {
    Serial.println(msg);
}

void Logger::writeToMqtt(const char* msg) {
    if (_mqttClient == nullptr || !_mqttClient->connected()) {
        return;
    }
    _mqttClient->publish(_mqttTopic.c_str(), 0, false, msg);
}

void Logger::writeToSdCard(const char* msg) {
    if (_sd == nullptr) {
        return;
    }

    FsFile logFile;
    if (!logFile.open(_logFilename.c_str(), O_WRONLY | O_CREAT | O_APPEND)) {
        return;
    }

    if (logFile.size() > _maxFileSize) {
        logFile.close();
        rotateLogFiles();
        if (!logFile.open(_logFilename.c_str(), O_WRONLY | O_CREAT | O_APPEND)) {
            return;
        }
    }

    logFile.println(msg);
    logFile.close();
}

String Logger::getRotatedFilename(uint8_t index) {
    // Extract base name without extension
    int dotIndex = _logFilename.lastIndexOf('.');
    String baseName = (dotIndex > 0) ? _logFilename.substring(0, dotIndex) : _logFilename;
    String ext = (dotIndex > 0) ? _logFilename.substring(dotIndex) : ".txt";

    // Return format: /log.1.txt, /log.2.txt, etc.
    return baseName + "." + String(index) + ext;
}

void Logger::rotateLogFiles() {
    if (_sd == nullptr) {
        return;
    }

    Serial.println("[Logger] Starting log rotation...");

    // Delete the oldest rotated file if it exists
    String oldestFile = getRotatedFilename(_maxRotatedFiles);
    if (_sd->exists(oldestFile.c_str())) {
        _sd->remove(oldestFile.c_str());
        Serial.printf("[Logger] Deleted oldest: %s\n", oldestFile.c_str());
    }

    // Shift existing rotated files: log.9.txt -> log.10.txt, log.8.txt -> log.9.txt, etc.
    for (int i = _maxRotatedFiles - 1; i >= 1; i--) {
        String oldName = getRotatedFilename(i);
        String newName = getRotatedFilename(i + 1);

        if (_sd->exists(oldName.c_str())) {
            _sd->rename(oldName.c_str(), newName.c_str());
            Serial.printf("[Logger] Renamed %s -> %s\n", oldName.c_str(), newName.c_str());
        }
    }

    // Rename current log file to log.1.txt
    String rotatedName = getRotatedFilename(1);
    if (_sd->rename(_logFilename.c_str(), rotatedName.c_str())) {
        Serial.printf("[Logger] Rotated %s -> %s\n", _logFilename.c_str(), rotatedName.c_str());
    } else {
        Serial.printf("[Logger] Failed to rotate %s\n", _logFilename.c_str());
    }

    Serial.println("[Logger] Log rotation complete");
}

bool Logger::compressFile(const char* srcPath, const char* destPath) {
    // Compression not implemented - placeholder for future gzip support
    // For now, just rename the file
    if (_sd == nullptr) {
        return false;
    }
    return _sd->rename(srcPath, destPath);
}
