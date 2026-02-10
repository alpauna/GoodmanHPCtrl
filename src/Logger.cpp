#include "Logger.h"
#include <stdarg.h>
#include <time.h>
#include <WiFi.h>

Logger Log;

static const char* LEVEL_NAMES[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

Logger::Logger()
    : _level(LOG_INFO)
    , _serialEnabled(true)
    , _mqttEnabled(false)
    , _sdCardEnabled(false)
    , _wsEnabled(false)
    , _mqttClient(nullptr)
    , _mqttTopic("goodman/log")
    , _ws(nullptr)
    , _sd(nullptr)
    , _logFilename("/log.txt")
    , _maxFileSize(DEFAULT_MAX_FILE_SIZE)
    , _maxRotatedFiles(DEFAULT_MAX_ROTATED_FILES)
    , _compressionAvailable(true)
    , _ringBufferMax(DEFAULT_RING_BUFFER_SIZE)
    , _ringBufferHead(0)
    , _ringBufferCount(0)
{
    _ringBuffer.resize(_ringBufferMax);
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

void Logger::setWebSocket(AsyncWebSocket* ws) {
    _ws = ws;
    _wsEnabled = (ws != nullptr);
}

void Logger::enableWebSocket(bool enable) {
    _wsEnabled = enable && (_ws != nullptr);
}

bool Logger::isWebSocketEnabled() {
    return _wsEnabled;
}

void Logger::setRingBufferSize(size_t maxEntries) {
    _ringBufferMax = maxEntries;
    _ringBuffer.resize(_ringBufferMax);
    _ringBufferHead = 0;
    _ringBufferCount = 0;
}

const std::vector<String>& Logger::getRingBuffer() const {
    return _ringBuffer;
}

size_t Logger::getRingBufferHead() const {
    return _ringBufferHead;
}

size_t Logger::getRingBufferCount() const {
    return _ringBufferCount;
}

void Logger::addToRingBuffer(const char* msg) {
    _ringBuffer[_ringBufferHead] = String(msg);
    _ringBufferHead = (_ringBufferHead + 1) % _ringBufferMax;
    if (_ringBufferCount < _ringBufferMax) {
        _ringBufferCount++;
    }
}

void Logger::writeToWebSocket(const char* msg) {
    if (_ws == nullptr || _ws->count() == 0) {
        return;
    }
    String json = "{\"type\":\"log\",\"message\":\"";
    // Escape special JSON characters in the message
    for (const char* p = msg; *p; p++) {
        switch (*p) {
            case '"':  json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default:   json += *p; break;
        }
    }
    json += "\"}";
    _ws->textAll(json);
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

    // Get current time
    struct tm timeinfo;
    char timeStr[20] = "----/--/-- --:--:--";
    if(WiFi.isConnected()){
        if (getLocalTime(&timeinfo)) {
            strftime(timeStr, sizeof(timeStr), "%Y/%m/%d %H:%M:%S", &timeinfo);
        }
    }
    snprintf(_buffer, sizeof(_buffer), "[%s] [%s] [%s] %s",
             timeStr, getLevelName(level), tag, msgBuffer);

    addToRingBuffer(_buffer);

    if (_serialEnabled) {
        writeToSerial(_buffer);
    }
    if (_mqttEnabled) {
        writeToMqtt(_buffer);
    }
    if (_sdCardEnabled) {
        writeToSdCard(_buffer);
    }
    if (_wsEnabled) {
        writeToWebSocket(_buffer);
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
    int dotIndex = _logFilename.lastIndexOf('.');
    String baseName = (dotIndex > 0) ? _logFilename.substring(0, dotIndex) : _logFilename;

    // Rotated files use .tar.gz extension: /log.1.tar.gz, /log.2.tar.gz, etc.
    return baseName + "." + String(index) + ".tar.gz";
}

bool Logger::initArduinoSD() {
    if (!SD.begin(SS)) {
        Serial.println("[Logger] Failed to initialize Arduino SD for compression");
        _compressionAvailable = false;
        return false;
    }
    return true;
}

void Logger::deinitArduinoSD() {
    SD.end();
}

bool Logger::reinitSdFat() {
    if (!_sd->begin(SS, SD_SCK_MHZ(SD_SPI_SPEED))) {
        Serial.println("[Logger] CRITICAL: Failed to re-initialize SdFat after compression");
        _sdCardEnabled = false;
        return false;
    }
    return true;
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

    // Shift existing rotated files: log.9.tar.gz -> log.10.tar.gz, etc.
    for (int i = _maxRotatedFiles - 1; i >= 1; i--) {
        String oldName = getRotatedFilename(i);
        String newName = getRotatedFilename(i + 1);

        if (_sd->exists(oldName.c_str())) {
            _sd->rename(oldName.c_str(), newName.c_str());
            Serial.printf("[Logger] Renamed %s -> %s\n", oldName.c_str(), newName.c_str());
        }
    }

    // Compress current log file to log.1.tar.gz
    String compressedName = getRotatedFilename(1);

    if (_compressionAvailable && compressFile(_logFilename.c_str(), compressedName.c_str())) {
        Serial.printf("[Logger] Compressed %s -> %s\n",
                      _logFilename.c_str(), compressedName.c_str());
    } else {
        // Compression failed or unavailable — fall back to plain rename
        Serial.println("[Logger] Compression failed, falling back to rename");
        int dotIdx = _logFilename.lastIndexOf('.');
        String baseName = (dotIdx > 0) ? _logFilename.substring(0, dotIdx) : _logFilename;
        String fallbackName = baseName + ".1.txt";

        if (_sd->rename(_logFilename.c_str(), fallbackName.c_str())) {
            Serial.printf("[Logger] Fallback renamed %s -> %s\n",
                          _logFilename.c_str(), fallbackName.c_str());
        } else {
            Serial.printf("[Logger] CRITICAL: Failed to rotate %s\n", _logFilename.c_str());
        }
    }

    Serial.println("[Logger] Log rotation complete");
}

bool Logger::compressFile(const char* srcPath, const char* destPath) {
    if (_sd == nullptr || !_compressionAvailable) {
        return false;
    }

    Serial.printf("[Logger] Compressing %s -> %s\n", srcPath, destPath);

    // Release SdFat's hold on the SD card
    _sd->end();

    // Initialize Arduino SD library for ESP32-targz compatibility
    if (!initArduinoSD()) {
        reinitSdFat();
        return false;
    }

    // Verify source file exists and get its size via Arduino SD
    fs::File srcFile = SD.open(srcPath, FILE_READ);
    if (!srcFile) {
        Serial.printf("[Logger] Cannot open %s via Arduino SD\n", srcPath);
        deinitArduinoSD();
        reinitSdFat();
        return false;
    }
    srcFile.close();

    // Create tar.gz containing the single log file
    // Put source in a temp directory so TarGzPacker compresses just this file
    String tmpDir = "/_log_rotate";
    String tmpPath = tmpDir + "/" + String(srcPath);

    SD.mkdir(tmpDir.c_str());
    SD.rename(srcPath, tmpPath.c_str());

    fs::File outFile = SD.open(destPath, FILE_WRITE);
    if (!outFile) {
        Serial.printf("[Logger] Cannot create output %s\n", destPath);
        // Move file back
        SD.rename(tmpPath.c_str(), srcPath);
        SD.rmdir(tmpDir.c_str());
        deinitArduinoSD();
        reinitSdFat();
        return false;
    }

    size_t result = TarGzPacker::compress(&SD, tmpDir.c_str(), &outFile);
    outFile.close();

    bool success = (result > 0);
    if (success) {
        Serial.printf("[Logger] Compression successful (%d bytes)\n", result);
        // Remove the original uncompressed file
        SD.remove(tmpPath.c_str());
    } else {
        Serial.printf("[Logger] Compression FAILED (result: %d)\n", result);
        // Move original file back and delete partial output
        SD.rename(tmpPath.c_str(), srcPath);
        if (SD.exists(destPath)) {
            SD.remove(destPath);
        }
    }

    // Clean up temp directory
    SD.rmdir(tmpDir.c_str());

    // Release Arduino SD and restore SdFat
    deinitArduinoSD();

    if (!reinitSdFat()) {
        Serial.println("[Logger] CRITICAL: SdFat re-initialization failed");
        return false;
    }

    return success;
}
