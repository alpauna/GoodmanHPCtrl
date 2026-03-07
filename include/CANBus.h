#ifndef CANBUS_H
#define CANBUS_H

#include <Arduino.h>
#include <driver/twai.h>
#include <TaskSchedulerDeclarations.h>
#include <functional>

// CAN message IDs — AThermostat (0x100-0x10F)
static constexpr uint32_t CAN_ID_THERMO_STATE    = 0x100;  // mode, setpoints, flags
static constexpr uint32_t CAN_ID_THERMO_SENSORS  = 0x101;  // indoor temp, pressure
static constexpr uint32_t CAN_ID_LEAK_STATUS     = 0x102;  // A2L PPM, alarm, fault
static constexpr uint32_t CAN_ID_FURNACE_STATUS  = 0x103;  // flame, exhaust temp, fault

// CAN message IDs — Display (0x110-0x11F)
static constexpr uint32_t CAN_ID_DISPLAY_SETPOINT = 0x110; // user setpoint change
static constexpr uint32_t CAN_ID_DISPLAY_MODE     = 0x111; // user mode change

// CAN message IDs — GoodmanHPCtrl (0x200-0x21F)
static constexpr uint32_t CAN_ID_HP_STATE         = 0x200; // state, outputs, faults, protections
static constexpr uint32_t CAN_ID_HP_SENSORS       = 0x201; // outdoor temps (ambient, condenser, suction, liquid)

// Heartbeat (any node)
static constexpr uint32_t CAN_ID_HEARTBEAT        = 0x3FF;

// Node identifiers (used in heartbeat data)
enum class CANNodeId : uint8_t {
    THERMOSTAT  = 0x01,
    THERMO_DISP = 0x02,
    HP_CTRL     = 0x03
};

class CANBus {
public:
    CANBus(Scheduler* ts, gpio_num_t txPin = GPIO_NUM_13, gpio_num_t rxPin = GPIO_NUM_14);

    bool begin(twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS());
    void stop();
    bool isRunning() const { return _running; }

    // Send a CAN frame (up to 8 bytes)
    bool send(uint32_t id, const uint8_t* data, uint8_t len);

    // Send HP state (0x200): state, outputs, faults, protections, runtime, inputs, band+source
    bool sendHPState(uint8_t state, uint8_t outputs, uint8_t faults,
                     uint8_t protections, uint16_t heatRuntimeMin,
                     uint8_t inputs, uint8_t bandAndSource);

    // Send HP temps (0x201): 4 temps as int16 x10 (big-endian), -9999=invalid
    bool sendHPTemps(int16_t ambient, int16_t condenser,
                     int16_t suction, int16_t liquid);

    bool sendHeartbeat(CANNodeId node);

    // Receive callback — called from poll task on main loop
    typedef std::function<void(uint32_t id, const uint8_t* data, uint8_t len)> RxCallback;
    void onReceive(RxCallback cb) { _rxCallback = cb; }

    // Stats
    uint32_t getTxCount() const { return _txCount; }
    uint32_t getRxCount() const { return _rxCount; }
    uint32_t getErrorCount() const { return _errCount; }
    twai_status_info_t getStatus();

private:
    Scheduler* _ts;
    gpio_num_t _txPin;
    gpio_num_t _rxPin;
    bool _running = false;

    Task* _tPoll = nullptr;
    Task* _tHeartbeat = nullptr;

    RxCallback _rxCallback = nullptr;

    uint32_t _txCount = 0;
    uint32_t _rxCount = 0;
    uint32_t _errCount = 0;

    void poll();
    void recoverBus();
};

#endif
