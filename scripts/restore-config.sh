#!/bin/bash
# Restore config.txt to device SD card from a local backup
# Usage:
#   ./scripts/restore-config.sh                     — pick from available backups
#   ./scripts/restore-config.sh backups/config-latest.txt  — restore a specific file

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKUP_DIR="$SCRIPT_DIR/../backups"
FTP_USER="admin"
FTP_PASS=""

# --- Determine which file to restore ---

CONFIG_FILE=""
if [ -n "$1" ]; then
    # Explicit file path
    if [ ! -f "$1" ]; then
        echo "Error: File not found: $1"
        exit 1
    fi
    CONFIG_FILE="$1"
else
    # List available backups
    if [ ! -d "$BACKUP_DIR" ]; then
        echo "Error: No backups directory found. Run backup-config.sh first."
        exit 1
    fi

    echo "Available backups:"
    echo
    IDX=0
    declare -a BACKUP_FILES
    # Latest copy
    if [ -f "$BACKUP_DIR/config-latest.txt" ]; then
        SIZE=$(stat -c%s "$BACKUP_DIR/config-latest.txt" 2>/dev/null || stat -f%z "$BACKUP_DIR/config-latest.txt" 2>/dev/null)
        IDX=$((IDX + 1))
        BACKUP_FILES[$IDX]="$BACKUP_DIR/config-latest.txt"
        printf "  [%d] config-latest.txt (%d bytes)\n" "$IDX" "$SIZE"
    fi
    # Timestamped backups (newest first)
    for DIR in $(ls -rd "$BACKUP_DIR"/[0-9]* 2>/dev/null); do
        if [ -f "$DIR/config.txt" ]; then
            SIZE=$(stat -c%s "$DIR/config.txt" 2>/dev/null || stat -f%z "$DIR/config.txt" 2>/dev/null)
            TIMESTAMP=$(basename "$DIR")
            IDX=$((IDX + 1))
            BACKUP_FILES[$IDX]="$DIR/config.txt"
            printf "  [%d] %s/config.txt (%d bytes)\n" "$IDX" "$TIMESTAMP" "$SIZE"
        fi
    done

    if [ $IDX -eq 0 ]; then
        echo "  (none found)"
        echo
        echo "Run backup-config.sh first, or specify a file path."
        exit 1
    fi

    echo
    read -rp "Select backup to restore [1-$IDX]: " CHOICE
    if [ -z "$CHOICE" ] || [ "$CHOICE" -lt 1 ] 2>/dev/null || [ "$CHOICE" -gt "$IDX" ] 2>/dev/null; then
        echo "Invalid selection."
        exit 1
    fi
    CONFIG_FILE="${BACKUP_FILES[$CHOICE]}"
fi

SIZE=$(stat -c%s "$CONFIG_FILE" 2>/dev/null || stat -f%z "$CONFIG_FILE" 2>/dev/null)
echo
echo "Restoring: $CONFIG_FILE ($SIZE bytes)"

# Validate JSON
if ! python3 -m json.tool "$CONFIG_FILE" >/dev/null 2>&1; then
    echo "Warning: File does not appear to be valid JSON."
    read -rp "Continue anyway? [y/N] " CONT
    if [ "$CONT" != "y" ] && [ "$CONT" != "Y" ]; then
        echo "Cancelled."
        exit 0
    fi
fi

# --- Connect to device ---

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

# Get FTP password from status endpoint
FTP_STATUS=$(curl $CURL_OPTS $AUTH_OPTS "$BASE_URL/ftp" 2>/dev/null)
FTP_PASS=$(echo "$FTP_STATUS" | grep -o '"password":"[^"]*"' | cut -d'"' -f4)
if [ -z "$FTP_PASS" ]; then
    FTP_PASS="admin"
fi
echo "FTP password: $FTP_PASS"

sleep 2

# Upload config.txt
echo "Uploading config.txt to device..."
curl -s -T "$CONFIG_FILE" \
    "ftp://$FTP_USER:$FTP_PASS@$DEVICE_IP/config.txt"

# Verify upload by downloading and comparing size
echo "Verifying upload..."
TMPFILE=$(mktemp)
curl -s -o "$TMPFILE" \
    "ftp://$FTP_USER:$FTP_PASS@$DEVICE_IP/config.txt" 2>/dev/null || true

if [ -f "$TMPFILE" ]; then
    REMOTE_SIZE=$(stat -c%s "$TMPFILE" 2>/dev/null || stat -f%z "$TMPFILE" 2>/dev/null)
    rm -f "$TMPFILE"
    if [ "$REMOTE_SIZE" = "$SIZE" ]; then
        echo "Verified: $REMOTE_SIZE bytes on device matches local file."
    else
        echo "Warning: Size mismatch — local $SIZE bytes, device $REMOTE_SIZE bytes"
    fi
else
    echo "Warning: Could not verify upload"
    rm -f "$TMPFILE"
fi

# Disable FTP
curl $CURL_OPTS $AUTH_OPTS \
    -X POST "$BASE_URL/ftp" \
    -H "Content-Type: application/json" \
    -d '{"duration":0}' >/dev/null 2>&1 || true

echo
echo "Config restored. Reboot the device to load the new configuration."
read -rp "Reboot now? [y/N] " REBOOT
if [ "$REBOOT" = "y" ] || [ "$REBOOT" = "Y" ]; then
    curl $CURL_OPTS $AUTH_OPTS -X POST "$BASE_URL/reboot" >/dev/null 2>&1 || true
    echo "Reboot initiated."
fi
