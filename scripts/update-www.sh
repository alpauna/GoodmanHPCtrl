#!/bin/bash
# Upload web pages from data/www/ to ESP32 LittleFS via HTTP /www/upload endpoint.
# Can also upload via USB serial as a fallback (--usb flag).
#
# Usage:
#   ./scripts/update-www.sh                    — upload all files over HTTP
#   ./scripts/update-www.sh dashboard.html     — upload a single file over HTTP
#   ./scripts/update-www.sh --usb [port]       — flash entire LittleFS via USB serial

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
WWW_DIR="$PROJECT_DIR/data/www"
PIO="$HOME/.platformio/penv/bin/pio"
ENV="freenove_esp32_s3_wroom"

if [ ! -d "$WWW_DIR" ]; then
    echo "Error: data/www/ directory not found"
    exit 1
fi

# --- USB serial mode ---
if [ "$1" = "--usb" ]; then
    PORT_ARG=""
    if [ -n "$2" ]; then
        PORT_ARG="--upload-port $2"
    fi
    echo "Building and uploading LittleFS image via USB..."
    $PIO run -t uploadfs -e "$ENV" $PORT_ARG
    echo "Done. Web pages flashed to LittleFS."
    exit 0
fi

# --- HTTP upload mode ---

read -rp "Device IP: " DEVICE_IP
if [ -z "$DEVICE_IP" ]; then
    echo "Error: IP address required"
    exit 1
fi

read -rsp "Admin password (blank if none set): " ADMIN_PW
echo

BASE_URL="http://$DEVICE_IP"
CURL_OPTS="-s"
AUTH_OPTS=""
if [ -n "$ADMIN_PW" ]; then
    AUTH_OPTS="-u admin:$ADMIN_PW"
fi

# Verify device is reachable and auth works
echo "Checking device at $DEVICE_IP..."
HTTP_CODE=$(curl $CURL_OPTS $AUTH_OPTS -o /dev/null -w "%{http_code}" "$BASE_URL/www/list" 2>/dev/null) || true
if [ "$HTTP_CODE" = "401" ]; then
    echo "Error: Authentication failed."
    exit 1
fi
if [ "$HTTP_CODE" != "200" ]; then
    echo "Error: Could not reach device (HTTP $HTTP_CODE)"
    exit 1
fi

# Determine files to upload
if [ -n "$1" ]; then
    # Single file mode
    FILES="$1"
    if [ ! -f "$WWW_DIR/$1" ]; then
        echo "Error: $WWW_DIR/$1 not found"
        exit 1
    fi
else
    # All files
    FILES=$(ls "$WWW_DIR")
fi

# Upload each file
TOTAL=0
FAILED=0
for FILE in $FILES; do
    FILEPATH="$WWW_DIR/$FILE"
    if [ ! -f "$FILEPATH" ]; then
        continue
    fi

    SIZE=$(stat -c%s "$FILEPATH" 2>/dev/null || stat -f%z "$FILEPATH" 2>/dev/null)
    SIZE_KB=$((SIZE / 1024))
    printf "  %-25s %4d KB ... " "$FILE" "$SIZE_KB"

    RESP=$(curl $CURL_OPTS $AUTH_OPTS \
        -X POST "$BASE_URL/www/upload" \
        -H "X-Filename: $FILE" \
        -H "Content-Type: application/octet-stream" \
        --data-binary "@$FILEPATH" 2>/dev/null) || true

    if echo "$RESP" | grep -q '"status":"ok"'; then
        echo "OK"
        TOTAL=$((TOTAL + 1))
    else
        echo "FAILED"
        echo "    $RESP"
        FAILED=$((FAILED + 1))
    fi
done

echo
if [ $FAILED -eq 0 ]; then
    echo "Done. $TOTAL file(s) uploaded successfully."
else
    echo "Done. $TOTAL uploaded, $FAILED failed."
    exit 1
fi
