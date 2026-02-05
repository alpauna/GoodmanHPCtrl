#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include "SdFat.h"

class Logger {
public:
    enum Level { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

    static const uint32_t DEFAULT_MAX_FILE_SIZE = 50 * 1024 * 1024;  // 50MB
    static const uint8_t DEFAULT_MAX_ROTATED_FILES = 10;

    Logger();

    void setLevel(Level level);
    Level getLevel();
    const char* getLevelName(Level level);

    void error(const char* tag, const char* format, ...);
    void warn(const char* tag, const char* format, ...);
    void info(const char* tag, const char* format, ...);
    void debug(const char* tag, const char* format, ...);

    void setMqttClient(AsyncMqttClient* client, const char* topic);
    void setLogFile(SdFs* sd, const char* filename,
                    uint32_t maxFileSize = DEFAULT_MAX_FILE_SIZE,
                    uint8_t maxRotatedFiles = DEFAULT_MAX_ROTATED_FILES);

    void enableSerial(bool enable);
    void enableMqtt(bool enable);
    void enableSdCard(bool enable);

    bool isSerialEnabled();
    bool isMqttEnabled();
    bool isSdCardEnabled();

private:
    void log(Level level, const char* tag, const char* format, va_list args);
    void writeToSerial(const char* msg);
    void writeToMqtt(const char* msg);
    void writeToSdCard(const char* msg);
    void rotateLogFiles();
    bool compressFile(const char* srcPath, const char* destPath);
    String getRotatedFilename(uint8_t index);

    Level _level;
    bool _serialEnabled;
    bool _mqttEnabled;
    bool _sdCardEnabled;

    AsyncMqttClient* _mqttClient;
    String _mqttTopic;

    SdFs* _sd;
    String _logFilename;
    uint32_t _maxFileSize;
    uint8_t _maxRotatedFiles;

    char _buffer[512];
};

extern Logger Log;

#endif
