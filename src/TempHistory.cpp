#include "TempHistory.h"
#include <Arduino.h>
#include <SD.h>
#include "Logger.h"

const char* TempHistory::sensorDirs[MAX_SENSORS] = {
    "ambient", "compressor", "suction", "condenser", "liquid"
};

const char* TempHistory::sensorKeys[MAX_SENSORS] = {
    "AMBIENT_TEMP", "COMPRESSOR_TEMP", "SUCTION_TEMP", "CONDENSER_TEMP", "LIQUID_TEMP"
};

void TempHistory::begin() {
    for (int i = 0; i < MAX_SENSORS; i++) {
        _buffers[i] = (TempSample*)ps_malloc(MAX_SAMPLES * sizeof(TempSample));
        if (!_buffers[i]) {
            Log.error("THIST", "Failed to allocate PSRAM for sensor %d", i);
        }
        _head[i] = 0;
        _count[i] = 0;
    }
    Log.info("THIST", "Allocated %d bytes PSRAM for temp history",
             MAX_SENSORS * MAX_SAMPLES * (int)sizeof(TempSample));
}

void TempHistory::addSample(int sensorIdx, uint32_t epoch, float temp) {
    if (sensorIdx < 0 || sensorIdx >= MAX_SENSORS || !_buffers[sensorIdx]) return;
    _buffers[sensorIdx][_head[sensorIdx]].epoch = epoch;
    _buffers[sensorIdx][_head[sensorIdx]].temp = temp;
    _head[sensorIdx] = (_head[sensorIdx] + 1) % MAX_SAMPLES;
    if (_count[sensorIdx] < MAX_SAMPLES) _count[sensorIdx]++;
}

int TempHistory::getSamples(int sensorIdx, uint32_t sinceEpoch, TempSample* out, int maxOut) {
    if (sensorIdx < 0 || sensorIdx >= MAX_SENSORS || !_buffers[sensorIdx] || !out) return 0;

    int count = _count[sensorIdx];
    int head = _head[sensorIdx];
    // Oldest entry is at (head - count + MAX_SAMPLES) % MAX_SAMPLES
    int start = (head - count + MAX_SAMPLES) % MAX_SAMPLES;

    int written = 0;
    for (int i = 0; i < count && written < maxOut; i++) {
        int idx = (start + i) % MAX_SAMPLES;
        if (_buffers[sensorIdx][idx].epoch >= sinceEpoch) {
            out[written++] = _buffers[sensorIdx][idx];
        }
    }
    return written;
}

void TempHistory::backfillFromSD() {
    if (!SD.exists("/temps")) {
        Log.info("THIST", "No /temps directory, skipping backfill");
        return;
    }

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) {
        Log.warn("THIST", "No NTP time, skipping backfill");
        return;
    }
    time_t now = mktime(&timeinfo);
    time_t cutoff = now - (7 * 86400);  // 7 days back

    for (int s = 0; s < MAX_SENSORS; s++) {
        if (!_buffers[s]) continue;

        char dirPath[32];
        snprintf(dirPath, sizeof(dirPath), "/temps/%s", sensorDirs[s]);

        File dir = SD.open(dirPath);
        if (!dir || !dir.isDirectory()) continue;

        // Collect CSV filenames
        String files[8];
        int fileCount = 0;

        File entry = dir.openNextFile();
        while (entry && fileCount < 8) {
            String name = entry.name();
            entry.close();
            if (name.endsWith(".csv")) {
                int slashIdx = name.lastIndexOf('/');
                String datePart = (slashIdx >= 0) ? name.substring(slashIdx + 1) : name;
                datePart = datePart.substring(0, datePart.length() - 4);

                // Check if within 7-day window
                struct tm fileTm = {};
                if (sscanf(datePart.c_str(), "%d-%d-%d",
                           &fileTm.tm_year, &fileTm.tm_mon, &fileTm.tm_mday) == 3) {
                    fileTm.tm_year -= 1900;
                    fileTm.tm_mon -= 1;
                    time_t fileTime = mktime(&fileTm);
                    if (fileTime >= cutoff) {
                        files[fileCount++] = datePart;
                    }
                }
            }
            entry = dir.openNextFile();
        }
        dir.close();

        // Sort dates ascending (simple bubble sort, max 8 entries)
        for (int i = 0; i < fileCount - 1; i++) {
            for (int j = i + 1; j < fileCount; j++) {
                if (files[i] > files[j]) {
                    String tmp = files[i];
                    files[i] = files[j];
                    files[j] = tmp;
                }
            }
        }

        // Read each CSV file and add samples
        int totalRows = 0;
        for (int fi = 0; fi < fileCount; fi++) {
            char filepath[48];
            snprintf(filepath, sizeof(filepath), "/temps/%s/%s.csv", sensorDirs[s], files[fi].c_str());

            File f = SD.open(filepath, FILE_READ);
            if (!f) continue;

            char line[48];
            while (f.available() && totalRows < MAX_SAMPLES) {
                int len = 0;
                while (f.available() && len < (int)sizeof(line) - 1) {
                    char c = f.read();
                    if (c == '\n' || c == '\r') break;
                    line[len++] = c;
                }
                line[len] = '\0';
                if (len == 0) continue;

                long epoch = 0;
                float temp = 0;
                if (sscanf(line, "%ld,%f", &epoch, &temp) == 2) {
                    if ((time_t)epoch >= cutoff) {
                        addSample(s, (uint32_t)epoch, temp);
                        totalRows++;
                    }
                }
            }
            f.close();
        }

        if (totalRows > 0) {
            Log.info("THIST", "Backfilled %s: %d samples", sensorDirs[s], totalRows);
        }
    }
}
