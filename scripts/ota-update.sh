#!/bin/bash
# OTA firmware update or revert via HTTPS
# Usage:
#   ./scripts/ota-update.sh           — upload firmware.bin from PlatformIO build dir
#   ./scripts/ota-update.sh --revert  — revert to previous firmware backup on SD card

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../.pio/build/freenove_esp32_s3_wroom"
FIRMWARE="$BUILD_DIR/firmware.bin"
REBOOT_TIMEOUT=60

# --- Helper functions ---

wait_for_reboot() {
    echo "Waiting for device to come back online..."
    local elapsed=0
    while [ $elapsed -lt $REBOOT_TIMEOUT ]; do
        sleep 2
        elapsed=$((elapsed + 2))
        if curl $CURL_OPTS --connect-timeout 2 --max-time 3 "$BASE_URL/heap" >/dev/null 2>&1; then
            echo "Device back online after ${elapsed}s."
            return 0
        fi
        printf "\r  Waiting... %ds" "$elapsed"
    done
    echo
    echo "Warning: Device not responding after ${REBOOT_TIMEOUT}s."
    return 1
}

verify_firmware_size() {
    local desc="$1"
    local expected="$2"
    local endpoint="$3"

    RESP=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/$endpoint" 2>/dev/null) || true
    if [ -z "$RESP" ]; then
        echo "Warning: Could not verify $desc"
        return 0
    fi
    local actual
    actual=$(echo "$RESP" | grep -o '"size":[0-9]*' | cut -d: -f2) || true
    if [ -n "$actual" ] && [ -n "$expected" ] && [ "$actual" != "$expected" ]; then
        echo "Warning: $desc size mismatch (expected $expected, got $actual)"
    fi
}

# --- Collect credentials ---

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

# --- Verify device is reachable ---

echo "Checking device at $DEVICE_IP..."
HEAP=$(curl $CURL_OPTS --connect-timeout 5 "$BASE_URL/heap" 2>/dev/null) || true
if [ -z "$HEAP" ]; then
    # Fall back to HTTP (no certs on device)
    HEAP=$(curl $CURL_OPTS --connect-timeout 5 "http://$DEVICE_IP/heap" 2>/dev/null) || true
fi
if [ -z "$HEAP" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi
echo "Device online. $HEAP"

# --- Verify auth works before proceeding ---

AUTH_CHECK=$(curl $CURL_OPTS $AUTH_OPTS -o /dev/null -w "%{http_code}" "$BASE_URL/revert" 2>/dev/null) || true
if [ "$AUTH_CHECK" = "401" ]; then
    echo "Error: Authentication failed. Check admin password."
    exit 1
fi

# --- Revert mode ---
if [ "$1" = "--revert" ]; then
    echo
    echo "Checking for firmware backup on device..."
    BACKUP=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/revert" 2>/dev/null)
    if [ -z "$BACKUP" ]; then
        echo "Error: Could not check backup status"
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
    RESP=$(curl $CURL_OPTS $AUTH_OPTS -X POST "$BASE_URL/revert" 2>/dev/null) || true
    if [ "$RESP" = "OK" ]; then
        echo "Revert accepted. Device is rebooting..."
    else
        echo "Revert failed: $RESP"
        exit 1
    fi

    wait_for_reboot
    exit 0
fi

# --- Upload mode ---
if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware.bin not found at $FIRMWARE"
    echo "Run 'pio run -e freenove_esp32_s3_wroom' first."
    exit 1
fi

FW_SIZE=$(stat -c%s "$FIRMWARE" 2>/dev/null || stat -f%z "$FIRMWARE" 2>/dev/null)
FW_SIZE_KB=$((FW_SIZE / 1024))
echo
echo "Firmware: $FIRMWARE (${FW_SIZE_KB} KB)"
read -rp "Upload and flash this firmware? [y/N] " CONFIRM
if [ "$CONFIRM" != "y" ] && [ "$CONFIRM" != "Y" ]; then
    echo "Cancelled."
    exit 0
fi

# Step 1: Upload to SD card
echo
echo "Step 1/3: Uploading firmware to SD card..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/update" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$FIRMWARE" 2>/dev/null)

if [ "$RESP" != "OK" ]; then
    echo "Upload failed: $RESP"
    exit 1
fi
echo "Firmware uploaded to SD card."

# Verify the uploaded file size matches
echo
echo "Step 2/3: Verifying upload..."
APPLY_STATUS=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/apply" 2>/dev/null) || true
if [ -z "$APPLY_STATUS" ]; then
    echo "Warning: Could not verify upload"
else
    UPLOADED_EXISTS=$(echo "$APPLY_STATUS" | grep -o '"exists":true' || true)
    if [ -z "$UPLOADED_EXISTS" ]; then
        echo "Error: Firmware not found on SD card after upload"
        exit 1
    fi
    UPLOADED_SIZE=$(echo "$APPLY_STATUS" | grep -o '"size":[0-9]*' | cut -d: -f2)
    if [ "$UPLOADED_SIZE" != "$FW_SIZE" ]; then
        echo "Error: Size mismatch — local ${FW_SIZE} bytes, SD card ${UPLOADED_SIZE} bytes"
        exit 1
    fi
    echo "Verified: ${FW_SIZE_KB} KB on SD card matches local firmware."
fi

# Step 3: Apply (backup current + flash new + reboot)
echo
echo "Step 3/3: Applying firmware (backing up current version)..."
RESP=$(curl $CURL_OPTS $AUTH_OPTS -X POST "$BASE_URL/apply" 2>/dev/null) || true

if [ "$RESP" != "OK" ]; then
    echo "Apply failed: $RESP"
    exit 1
fi
echo "Apply accepted. Device is rebooting..."

wait_for_reboot

# Post-reboot verification
echo "Verifying new firmware..."
NEW_HEAP=$(curl $CURL_OPTS --connect-timeout 5 "$BASE_URL/heap" 2>/dev/null) || true
if [ -n "$NEW_HEAP" ]; then
    echo "Device healthy. $NEW_HEAP"
else
    echo "Warning: Could not verify device health after reboot."
fi

# Confirm firmware.new was cleaned up and backup exists
APPLY_AFTER=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/apply" 2>/dev/null) || true
REVERT_AFTER=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/revert" 2>/dev/null) || true

PENDING=$(echo "$APPLY_AFTER" | grep -o '"exists":true' || true)
BACKUP=$(echo "$REVERT_AFTER" | grep -o '"exists":true' || true)

if [ -n "$PENDING" ]; then
    echo "Warning: /firmware.new still on SD card (apply may not have completed)"
fi
if [ -n "$BACKUP" ]; then
    BACKUP_KB=$(echo "$REVERT_AFTER" | grep -o '"size":[0-9]*' | cut -d: -f2)
    BACKUP_KB=$((BACKUP_KB / 1024))
    echo "Previous firmware backed up (${BACKUP_KB} KB) — use --revert to roll back."
fi

echo
echo "OTA update complete."
