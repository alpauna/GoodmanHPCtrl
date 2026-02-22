#pragma once

#include <Arduino.h>

bool backupFirmwareToSD(const char* path = "/firmware.bak",
                        const char* buildDate = nullptr);
bool revertFirmwareFromSD(const char* path = "/firmware.bak");
bool applyFirmwareFromSD(const char* path = "/firmware.new",
                         const char* buildDate = nullptr);
bool firmwareBackupExists(const char* path = "/firmware.bak");
size_t firmwareBackupSize(const char* path = "/firmware.bak");
String getBackupBuildDate(const char* path = "/firmware.bak");
