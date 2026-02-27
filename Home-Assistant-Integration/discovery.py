#!/usr/bin/env python3
"""
MQTT Auto-Discovery for Home Assistant — Goodman Heat Pump Controller

Publishes retained discovery config messages to the HA MQTT discovery prefix
so all entities auto-appear grouped under a single device.

Usage:
    pip install paho-mqtt
    python discovery.py --broker 192.168.1.1 --prefix goodman
    python discovery.py --broker 192.168.1.1 --prefix goodman --remove   # Remove all entities

Requirements:
    - paho-mqtt >= 2.0
    - MQTT broker reachable from this machine
    - Home Assistant MQTT integration configured with default discovery prefix (homeassistant)
"""

import argparse
import json
import time
import paho.mqtt.client as mqtt

# Default configuration
DEFAULT_BROKER = "192.168.1.1"
DEFAULT_PORT = 1883
DEFAULT_PREFIX = "goodman"
DEFAULT_DISCOVERY_PREFIX = "homeassistant"
DEFAULT_DEVICE_IP = "192.168.1.138"


def device_block(prefix, device_ip):
    """Shared device block — groups all entities under one device in HA."""
    return {
        "identifiers": [f"{prefix}_hp_ctrl"],
        "name": f"{prefix.title()} Heat Pump",
        "manufacturer": "Custom",
        "model": "ESP32-S3 HVAC Controller",
        "configuration_url": f"http://{device_ip}",
    }


def build_entities(prefix):
    """Build all entity discovery configs."""
    avail = {"availability_topic": f"{prefix}/availability"}
    entities = []

    # --- Temperature sensors (from {prefix}/temps) ---
    temp_sensors = [
        ("compressor_temp", "COMPRESSOR_TEMP", "Compressor"),
        ("suction_temp", "SUCTION_TEMP", "Suction"),
        ("ambient_temp", "AMBIENT_TEMP", "Ambient"),
        ("condenser_temp", "CONDENSER_TEMP", "Condenser"),
        ("liquid_temp", "LIQUID_TEMP", "Liquid Line"),
    ]
    for obj_id, json_key, label in temp_sensors:
        entities.append(("sensor", obj_id, {
            "name": f"{label} Temperature",
            "unique_id": f"{prefix}_{obj_id}",
            "state_topic": f"{prefix}/temps",
            "value_template": f"{{{{ value_json.{json_key} | default('') }}}}",
            "unit_of_measurement": "\u00b0F",
            "device_class": "temperature",
            "state_class": "measurement",
            **avail,
        }))

    # --- Controller state (from {prefix}/state) ---
    entities.append(("sensor", "state", {
        "name": "State",
        "unique_id": f"{prefix}_state",
        "state_topic": f"{prefix}/state",
        "value_template": "{{ value_json.state }}",
        "device_class": "enum",
        "options": ["OFF", "COOL", "HEAT", "DEFROST", "ERROR", "LOW_TEMP"],
        "icon": "mdi:hvac",
        **avail,
    }))

    # --- Heat runtime (from {prefix}/state) ---
    entities.append(("sensor", "heat_runtime", {
        "name": "Heat Runtime",
        "unique_id": f"{prefix}_heat_runtime",
        "state_topic": f"{prefix}/state",
        "value_template": "{{ value_json.heatRuntimeMin }}",
        "unit_of_measurement": "min",
        "device_class": "duration",
        "state_class": "measurement",
        "icon": "mdi:timer-outline",
        **avail,
    }))

    # --- Input pins (from {prefix}/state → inputs) ---
    input_pins = [
        ("input_y", "Y", "Y Input (Compressor Call)", "mdi:thermostat"),
        ("input_o", "O", "O Input (Cool Mode)", "mdi:snowflake"),
        ("input_lps", "LPS", "LPS Input (Low Pressure)", "mdi:gauge-low"),
        ("input_dft", "DFT", "DFT Input (Defrost)", "mdi:snowflake-melt"),
    ]
    for obj_id, json_key, label, icon in input_pins:
        entities.append(("binary_sensor", obj_id, {
            "name": label,
            "unique_id": f"{prefix}_{obj_id}",
            "state_topic": f"{prefix}/state",
            "value_template": f"{{{{ value_json.inputs.{json_key} }}}}",
            "payload_on": "True",
            "payload_off": "False",
            "icon": icon,
            **avail,
        }))

    # --- Output relays (from {prefix}/state → outputs) ---
    output_pins = [
        ("output_fan", "FAN", "Fan Relay", "mdi:fan"),
        ("output_cnt", "CNT", "Contactor Relay", "mdi:electric-switch"),
        ("output_w", "W", "W Heat Relay", "mdi:radiator"),
        ("output_rv", "RV", "Reversing Valve", "mdi:valve"),
        ("output_aux", "AUX", "Aux Heat Relay", "mdi:radiator"),
    ]
    for obj_id, json_key, label, icon in output_pins:
        entities.append(("binary_sensor", obj_id, {
            "name": label,
            "unique_id": f"{prefix}_{obj_id}",
            "state_topic": f"{prefix}/state",
            "value_template": f"{{{{ value_json.outputs.{json_key} }}}}",
            "payload_on": "True",
            "payload_off": "False",
            "device_class": "running",
            "icon": icon,
            **avail,
        }))

    # --- Fault flags (from {prefix}/state) ---
    fault_flags = [
        ("lps_fault", "lpsFault", "LPS Fault"),
        ("compressor_overtemp", "compressorOverTemp", "Compressor Over Temp"),
        ("suction_low_temp", "suctionLowTemp", "Suction Low Temp"),
        ("rv_fail", "rvFail", "RV Fail"),
        ("high_suction_temp", "highSuctionTemp", "High Suction Temp"),
    ]
    for obj_id, json_key, label in fault_flags:
        entities.append(("binary_sensor", obj_id, {
            "name": label,
            "unique_id": f"{prefix}_{obj_id}",
            "state_topic": f"{prefix}/state",
            "value_template": f"{{{{ value_json.{json_key} }}}}",
            "payload_on": "True",
            "payload_off": "False",
            "device_class": "problem",
            **avail,
        }))

    # --- Protection/status flags (from {prefix}/state) ---
    status_flags = [
        ("low_temp", "lowTemp", "Low Temp Protection", "mdi:thermometer-alert"),
        ("defrost", "defrost", "Defrost Active", "mdi:snowflake-melt"),
        ("manual_override", "manualOverride", "Manual Override", "mdi:hand-back-right"),
        ("state_validating", "stateValidating", "State Validating", "mdi:timer-sand"),
    ]
    for obj_id, json_key, label, icon in status_flags:
        entities.append(("binary_sensor", obj_id, {
            "name": label,
            "unique_id": f"{prefix}_{obj_id}",
            "state_topic": f"{prefix}/state",
            "value_template": f"{{{{ value_json.{json_key} }}}}",
            "payload_on": "True",
            "payload_off": "False",
            "icon": icon,
            **avail,
        }))

    return entities


def publish_discovery(client, prefix, device_ip, discovery_prefix, remove=False):
    """Publish (or remove) all discovery configs."""
    device = device_block(prefix, device_ip)
    entities = build_entities(prefix)
    count = 0

    for component, obj_id, config in entities:
        topic = f"{discovery_prefix}/{component}/{prefix}/{obj_id}/config"

        if remove:
            # Empty retained message removes the entity
            client.publish(topic, "", retain=True)
        else:
            config["device"] = device
            payload = json.dumps(config)
            client.publish(topic, payload, retain=True)

        count += 1

    return count


def main():
    parser = argparse.ArgumentParser(
        description="Publish MQTT auto-discovery messages for Goodman HP Controller"
    )
    parser.add_argument("--broker", default=DEFAULT_BROKER, help=f"MQTT broker address (default: {DEFAULT_BROKER})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"MQTT broker port (default: {DEFAULT_PORT})")
    parser.add_argument("--user", default=None, help="MQTT username")
    parser.add_argument("--password", default=None, help="MQTT password")
    parser.add_argument("--prefix", default=DEFAULT_PREFIX, help=f"Device MQTT topic prefix (default: {DEFAULT_PREFIX})")
    parser.add_argument("--discovery-prefix", default=DEFAULT_DISCOVERY_PREFIX, help=f"HA discovery prefix (default: {DEFAULT_DISCOVERY_PREFIX})")
    parser.add_argument("--device-ip", default=DEFAULT_DEVICE_IP, help=f"Device IP for configuration_url (default: {DEFAULT_DEVICE_IP})")
    parser.add_argument("--remove", action="store_true", help="Remove all discovery entities instead of creating them")
    args = parser.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.user:
        client.username_pw_set(args.user, args.password)

    print(f"Connecting to MQTT broker at {args.broker}:{args.port}...")
    client.connect(args.broker, args.port, 60)
    client.loop_start()
    time.sleep(1)  # Wait for connection

    action = "Removing" if args.remove else "Publishing"
    print(f"{action} discovery messages (prefix={args.prefix}, discovery={args.discovery_prefix})...")

    count = publish_discovery(client, args.prefix, args.device_ip, args.discovery_prefix, args.remove)

    time.sleep(1)  # Wait for publishes to flush
    client.loop_stop()
    client.disconnect()

    action = "Removed" if args.remove else "Published"
    print(f"{action} {count} entity configs. Entities should {'disappear from' if args.remove else 'appear in'} Home Assistant shortly.")


if __name__ == "__main__":
    main()
