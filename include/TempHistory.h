#ifndef TEMPHISTORY_H
#define TEMPHISTORY_H

#include <cstdint>
#include <cstddef>

struct TempSample {
    uint32_t epoch;
    float temp;
};

class TempHistory {
public:
    static const int MAX_SENSORS = 5;
    static const int MAX_SAMPLES = 2016;  // 7 days * 24h * 60min / 5min

    void begin();
    void addSample(int sensorIdx, uint32_t epoch, float temp);
    int getSamples(int sensorIdx, uint32_t sinceEpoch, TempSample* out, int maxOut);
    void backfillFromSD();

    static const char* sensorDirs[MAX_SENSORS];
    static const char* sensorKeys[MAX_SENSORS];

private:
    TempSample* _buffers[MAX_SENSORS] = {};
    int _head[MAX_SENSORS] = {};
    int _count[MAX_SENSORS] = {};
};

#endif
