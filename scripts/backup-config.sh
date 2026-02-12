#!/bin/bash
# Download config.txt from device SD card for local backup
# Usage: ./scripts/backup-config.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKUP_DIR="$SCRIPT_DIR/../backups"
FTP_USER="admin"
FTP_PASS="admin"

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
HEAP=$(curl $CURL_OPTS --connect-timeout 5 "$BASE_URL/heap" 2>/dev/null) || true
if [ -z "$HEAP" ]; then
    HEAP=$(curl $CURL_OPTS --connect-timeout 5 "http://$DEVICE_IP/heap" 2>/dev/null) || true
fi
if [ -z "$HEAP" ]; then
    echo "Error: Could not reach device at $DEVICE_IP"
    exit 1
fi
echo "Device online."

# Enable FTP for 10 minutes
echo "Enabling FTP for 10 minutes..."
if [ -n "$ADMIN_PW" ]; then
    RESP=$(curl $CURL_OPTS $AUTH_OPTS \
        -X POST "$BASE_URL/ftp" \
        -H "Content-Type: application/json" \
        -d '{"duration":10}')
else
    RESP=$(curl $CURL_OPTS \
        -X POST "$BASE_URL/ftp" \
        -H "Content-Type: application/json" \
        -d '{"duration":10}')
fi
if echo "$RESP" | grep -q '"error"'; then
    echo "Error: $RESP"
    exit 1
fi
echo "FTP enabled."

sleep 2

# Create backup directory with timestamp
TIMESTAMP=$(date '+%Y%m%d-%H%M%S')
DEST_DIR="$BACKUP_DIR/$TIMESTAMP"
mkdir -p "$DEST_DIR"

# Download config.txt
echo "Downloading config.txt..."
curl -s -o "$DEST_DIR/config.txt" \
    "ftp://$FTP_USER:$FTP_PASS@$DEVICE_IP/config.txt"

if [ ! -s "$DEST_DIR/config.txt" ]; then
    echo "Error: Downloaded file is empty or missing"
    rm -rf "$DEST_DIR"
    exit 1
fi

SIZE=$(stat -c%s "$DEST_DIR/config.txt" 2>/dev/null || stat -f%z "$DEST_DIR/config.txt" 2>/dev/null)
echo "Saved: $DEST_DIR/config.txt ($SIZE bytes)"

# Also keep a "latest" copy
cp "$DEST_DIR/config.txt" "$BACKUP_DIR/config-latest.txt"
echo "Latest copy: $BACKUP_DIR/config-latest.txt"

echo
echo "Backup complete."
