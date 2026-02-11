#!/bin/bash
# Configure WiFi and MQTT credentials on the device
# Usage:
#   ./scripts/configure.sh <device-ip> [admin-password]   — update via HTTPS API
#   ./scripts/configure.sh --local                         — write config.txt for SD card

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Prompt helpers ---
prompt() {
    local var="$1" label="$2" default="$3"
    local input
    if [ -n "$default" ]; then
        read -rp "$label [$default]: " input
        eval "$var=\"${input:-$default}\""
    else
        read -rp "$label: " input
        eval "$var=\"$input\""
    fi
}

prompt_secret() {
    local var="$1" label="$2"
    local input
    read -rsp "$label: " input
    echo
    eval "$var=\"$input\""
}

# --- Local config.txt generation ---
write_local_config() {
    echo "=== Generate config.txt for SD card ==="
    echo "Passwords will be stored in plaintext and encrypted on first device boot."
    echo

    prompt WIFI_SSID "WiFi SSID" ""
    prompt_secret WIFI_PW "WiFi Password"
    prompt MQTT_HOST "MQTT Host" "192.168.0.46"
    prompt MQTT_PORT "MQTT Port" "1883"
    prompt MQTT_USER "MQTT User" "debian"
    prompt_secret MQTT_PW "MQTT Password"

    OUT="$SCRIPT_DIR/../data/config.txt"

    cat > "$OUT" << JSONEOF
{
  "project": "Goodman Heatpump Control",
  "created": "$(date '+%b %d %Y %H:%M:%S')",
  "description": "Control Goodman heatpump including defrost mode.",
  "wifi": {
    "ssid": "$WIFI_SSID",
    "password": "$WIFI_PW"
  },
  "mqtt": {
    "user": "$MQTT_USER",
    "password": "$MQTT_PW",
    "host": "$MQTT_HOST",
    "port": $MQTT_PORT
  },
  "logging": {
    "maxLogSize": 52428800,
    "maxOldLogCount": 10
  },
  "runtime": {
    "heatAccumulatedMs": 0
  },
  "timezone": {
    "gmtOffset": -21600,
    "daylightOffset": 3600
  },
  "lowTemp": {
    "threshold": 20.0
  },
  "admin": {
    "password": ""
  },
  "sensors": {
    "temp": {}
  }
}
JSONEOF

    echo
    echo "Written to: $OUT"
    echo "Copy this file to the root of the SD card as /config.txt"
    exit 0
}

# --- Network config via HTTPS API ---
if [ "$1" = "--local" ]; then
    write_local_config
fi

DEVICE_IP="${1:?Usage: $0 <device-ip> [admin-password]  OR  $0 --local}"
ADMIN_PW="${2:-}"
BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"
AUTH_OPTS=""
if [ -n "$ADMIN_PW" ]; then
    AUTH_OPTS="-u admin:$ADMIN_PW"
fi

# Fetch current config
echo "Fetching current config from $DEVICE_IP..."
CURRENT=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/config?format=json" 2>/dev/null)
if [ -z "$CURRENT" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi

# Parse current values for defaults
CUR_SSID=$(echo "$CURRENT" | grep -o '"wifiSSID":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_HOST=$(echo "$CURRENT" | grep -o '"mqttHost":"[^"]*"' | cut -d'"' -f4)
CUR_MQTT_PORT=$(echo "$CURRENT" | grep -o '"mqttPort":[0-9]*' | cut -d: -f2)
CUR_MQTT_USER=$(echo "$CURRENT" | grep -o '"mqttUser":"[^"]*"' | cut -d'"' -f4)

echo
echo "=== Configure Device ==="
echo "Leave blank to keep current value. Passwords always required for changes."
echo

prompt WIFI_SSID "WiFi SSID" "$CUR_SSID"

WIFI_PW=""
read -rsp "WiFi Password (blank=no change): " WIFI_PW
echo
CUR_WIFI_PW=""
if [ -n "$WIFI_PW" ]; then
    read -rsp "Current WiFi Password (required to change): " CUR_WIFI_PW
    echo
fi

prompt MQTT_HOST "MQTT Host" "$CUR_MQTT_HOST"
prompt MQTT_PORT "MQTT Port" "$CUR_MQTT_PORT"
prompt MQTT_USER "MQTT User" "$CUR_MQTT_USER"

MQTT_PW=""
read -rsp "MQTT Password (blank=no change): " MQTT_PW
echo
CUR_MQTT_PW=""
if [ -n "$MQTT_PW" ]; then
    read -rsp "Current MQTT Password (required to change): " CUR_MQTT_PW
    echo
fi

# Build JSON payload
JSON="{"
JSON+="\"wifiSSID\":\"$WIFI_SSID\""
if [ -n "$WIFI_PW" ]; then
    JSON+=",\"wifiPassword\":\"$WIFI_PW\""
    JSON+=",\"curWifiPw\":\"$CUR_WIFI_PW\""
fi
JSON+=",\"mqttHost\":\"$MQTT_HOST\""
JSON+=",\"mqttPort\":$MQTT_PORT"
JSON+=",\"mqttUser\":\"$MQTT_USER\""
if [ -n "$MQTT_PW" ]; then
    JSON+=",\"mqttPassword\":\"$MQTT_PW\""
    JSON+=",\"curMqttPw\":\"$CUR_MQTT_PW\""
fi
JSON+="}"

echo
echo "Saving configuration..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/config" \
    -H "Content-Type: application/json" \
    -d "$JSON")

echo "Response: $RESP"
