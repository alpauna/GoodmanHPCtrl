#!/bin/bash
# OTA firmware update or revert via HTTPS
# Usage:
#   ./scripts/ota-update.sh           — upload firmware.bin from PlatformIO build dir
#   ./scripts/ota-update.sh --revert  — revert to previous firmware backup on SD card

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../.pio/build/freenove_esp32_s3_wroom"
FIRMWARE="$BUILD_DIR/firmware.bin"

read -rp "Device IP: " DEVICE_IP
if [ -z "$DEVICE_IP" ]; then
    echo "Error: IP address required"
    exit 1
fi

read -rsp "Admin password (blank if none set): " ADMIN_PW
echo

BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"
AUTH_OPTS=""
if [ -n "$ADMIN_PW" ]; then
    AUTH_OPTS="-u admin:$ADMIN_PW"
fi

# Verify device is reachable
echo "Checking device at $DEVICE_IP..."
HEAP=$(curl $CURL_OPTS --connect-timeout 5 "http://$DEVICE_IP/heap" 2>/dev/null) || true
if [ -z "$HEAP" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi
echo "Device online."

# --- Revert mode ---
if [ "$1" = "--revert" ]; then
    echo
    echo "Checking for firmware backup on device..."
    BACKUP=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/revert" 2>/dev/null)
    if [ -z "$BACKUP" ]; then
        echo "Error: Could not check backup status (auth failed?)"
        exit 1
    fi

    EXISTS=$(echo "$BACKUP" | grep -o '"exists":true' || true)
    if [ -z "$EXISTS" ]; then
        echo "No firmware backup available on device."
        exit 0
    fi

    SIZE=$(echo "$BACKUP" | grep -o '"size":[0-9]*' | cut -d: -f2)
    SIZE_KB=$((SIZE / 1024))
    echo "Backup available: ${SIZE_KB} KB"
    read -rp "Revert to previous firmware? [y/N] " CONFIRM
    if [ "$CONFIRM" != "y" ] && [ "$CONFIRM" != "Y" ]; then
        echo "Cancelled."
        exit 0
    fi

    echo "Reverting firmware..."
    RESP=$(curl $CURL_OPTS $AUTH_OPTS -X POST "$BASE_URL/revert" 2>/dev/null)
    if [ "$RESP" = "OK" ]; then
        echo "Revert successful. Device is rebooting..."
    else
        echo "Revert failed: $RESP"
        exit 1
    fi
    exit 0
fi

# --- Upload mode ---
if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware.bin not found at $FIRMWARE"
    echo "Run 'pio run -e freenove_esp32_s3_wroom' first."
    exit 1
fi

SIZE=$(stat -c%s "$FIRMWARE" 2>/dev/null || stat -f%z "$FIRMWARE" 2>/dev/null)
SIZE_KB=$((SIZE / 1024))
echo
echo "Firmware: $FIRMWARE (${SIZE_KB} KB)"
read -rp "Upload and flash this firmware? [y/N] " CONFIRM
if [ "$CONFIRM" != "y" ] && [ "$CONFIRM" != "Y" ]; then
    echo "Cancelled."
    exit 0
fi

echo "Uploading firmware (previous version will be backed up on device)..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/update" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$FIRMWARE" 2>/dev/null)

if [ "$RESP" = "OK" ]; then
    echo "Upload successful. Device is rebooting..."
else
    echo "Upload failed: $RESP"
    exit 1
fi
