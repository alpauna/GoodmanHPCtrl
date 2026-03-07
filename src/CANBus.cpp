#include "CANBus.h"
#include "Logger.h"

CANBus::CANBus(Scheduler* ts, gpio_num_t txPin, gpio_num_t rxPin)
    : _ts(ts), _txPin(txPin), _rxPin(rxPin) {}

bool CANBus::begin(twai_timing_config_t timing) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(_txPin, _rxPin, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 16;
    g_config.tx_queue_len = 8;

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &timing, &f_config);
    if (err != ESP_OK) {
        Log.error("CAN", "Driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = twai_start();
    if (err != ESP_OK) {
        Log.error("CAN", "Start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return false;
    }

    _running = true;

    // Poll for received messages every 10ms on main loop
    _tPoll = new Task(10, TASK_FOREVER, [this]() { poll(); }, _ts, true);

    // Send heartbeat every 5 seconds
    _tHeartbeat = new Task(5000, TASK_FOREVER, [this]() {
        sendHeartbeat(CANNodeId::HP_CTRL);
    }, _ts, true);

    Log.info("CAN", "Bus started (TX=GPIO%d RX=GPIO%d)", _txPin, _rxPin);
    return true;
}

void CANBus::stop() {
    if (!_running) return;

    if (_tPoll) { _tPoll->disable(); delete _tPoll; _tPoll = nullptr; }
    if (_tHeartbeat) { _tHeartbeat->disable(); delete _tHeartbeat; _tHeartbeat = nullptr; }

    twai_stop();
    twai_driver_uninstall();
    _running = false;
    Log.info("CAN", "Bus stopped");
}

bool CANBus::send(uint32_t id, const uint8_t* data, uint8_t len) {
    if (!_running || len > 8) return false;

    twai_message_t msg = {};
    msg.identifier = id;
    msg.data_length_code = len;
    memcpy(msg.data, data, len);

    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(10));
    if (err == ESP_OK) {
        _txCount++;
        return true;
    }

    _errCount++;
    if (err != ESP_ERR_TIMEOUT) {
        Log.warn("CAN", "TX failed (0x%03X): %s", id, esp_err_to_name(err));
    }
    return false;
}

bool CANBus::sendHPState(uint8_t state, uint8_t outputs, uint8_t faults,
                         uint8_t protections, uint16_t heatRuntimeMin,
                         uint8_t inputs, uint8_t bandAndSource) {
    // Byte layout (0x200, 8 bytes):
    //  [0]   state enum (0=OFF,1=COOL,2=HEAT,3=DEFROST,4=ERROR,5=LOW_TEMP)
    //  [1]   outputs: bit0=FAN, bit1=CNT, bit2=W, bit3=RV, bit4=AUX
    //  [2]   faults: bit0=LPS, bit1=lowTemp, bit2=compOT, bit3=suctLT, bit4=rvFail, bit5=highSuct
    //  [3]   protections: bit0=defrost, bit1=dfTrans, bit2=dfCntPend, bit3=dfExit,
    //                     bit4=startup, bit5=SC, bit6=manual, bit7=stateVal
    //  [4-5] heat runtime minutes (uint16, big-endian)
    //  [6]   inputs: bit0=LPS, bit1=DFT, bit2=Y, bit3=O
    //  [7]   bits[1:0]=defrost band (0=Cold,1=Mid,2=Warm),
    //        bits[3:2]=ambient source (0=sensor,1=weather,2=internal)
    uint8_t data[8];
    data[0] = state;
    data[1] = outputs;
    data[2] = faults;
    data[3] = protections;
    data[4] = (heatRuntimeMin >> 8) & 0xFF;
    data[5] = heatRuntimeMin & 0xFF;
    data[6] = inputs;
    data[7] = bandAndSource;
    return send(CAN_ID_HP_STATE, data, 8);
}

bool CANBus::sendHPTemps(int16_t ambient, int16_t condenser,
                         int16_t suction, int16_t liquid) {
    // Byte layout (0x201, 8 bytes):
    //  [0-1] AMBIENT_TEMP x10 (int16, big-endian), -9999=invalid
    //  [2-3] CONDENSER_TEMP x10
    //  [4-5] SUCTION_TEMP x10
    //  [6-7] LIQUID_TEMP x10
    uint8_t data[8];
    data[0] = (ambient >> 8) & 0xFF;
    data[1] = ambient & 0xFF;
    data[2] = (condenser >> 8) & 0xFF;
    data[3] = condenser & 0xFF;
    data[4] = (suction >> 8) & 0xFF;
    data[5] = suction & 0xFF;
    data[6] = (liquid >> 8) & 0xFF;
    data[7] = liquid & 0xFF;
    return send(CAN_ID_HP_SENSORS, data, 8);
}

bool CANBus::sendHeartbeat(CANNodeId node) {
    // Byte layout:
    //  [0]   node ID
    //  [1-4] uptime seconds (uint32, big-endian)
    uint32_t uptime = millis() / 1000;
    uint8_t data[5];
    data[0] = static_cast<uint8_t>(node);
    data[1] = (uptime >> 24) & 0xFF;
    data[2] = (uptime >> 16) & 0xFF;
    data[3] = (uptime >> 8) & 0xFF;
    data[4] = uptime & 0xFF;
    return send(CAN_ID_HEARTBEAT, data, 5);
}

void CANBus::poll() {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        _rxCount++;
        if (_rxCallback) {
            _rxCallback(msg.identifier, msg.data, msg.data_length_code);
        }
    }

    // Check for bus-off and recover
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        if (status.state == TWAI_STATE_BUS_OFF) {
            recoverBus();
        }
    }
}

void CANBus::recoverBus() {
    Log.warn("CAN", "Bus-off detected, initiating recovery");
    _errCount++;
    esp_err_t err = twai_initiate_recovery();
    if (err != ESP_OK) {
        Log.error("CAN", "Recovery failed: %s", esp_err_to_name(err));
    }
}

twai_status_info_t CANBus::getStatus() {
    twai_status_info_t status = {};
    twai_get_status_info(&status);
    return status;
}
