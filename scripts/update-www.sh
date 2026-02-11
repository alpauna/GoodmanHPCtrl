#!/bin/bash
# Upload HTML files from data/www/ to SD card /www/ via FTP
# Prompts for device IP and optional admin password.

set -e

FTP_USER="admin"
FTP_PASS="admin"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WWW_DIR="$SCRIPT_DIR/../data/www"

if [ ! -d "$WWW_DIR" ]; then
    echo "Error: data/www/ directory not found at $WWW_DIR"
    exit 1
fi

read -rp "Device IP: " DEVICE_IP
if [ -z "$DEVICE_IP" ]; then
    echo "Error: IP address required"
    exit 1
fi

read -rsp "Admin password (blank if none set): " ADMIN_PW
echo

# Always use HTTPS with -k for self-signed certs
BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"

# Enable FTP for 10 minutes
if [ -n "$ADMIN_PW" ]; then
    echo "Authenticating and enabling FTP for 10 minutes..."
    RESP=$(curl $CURL_OPTS -u "admin:$ADMIN_PW" \
        -X POST "$BASE_URL/ftp" \
        -H "Content-Type: application/json" \
        -d '{"duration":10}')
    if echo "$RESP" | grep -q '"error"'; then
        echo "Error: $RESP"
        exit 1
    fi
    echo "FTP enabled: $RESP"
else
    echo "Enabling FTP for 10 minutes (no auth)..."
    RESP=$(curl $CURL_OPTS \
        -X POST "$BASE_URL/ftp" \
        -H "Content-Type: application/json" \
        -d '{"duration":10}')
    echo "FTP response: $RESP"
fi

# Wait for FTP to start
sleep 2

# Count files to upload
FILE_COUNT=0
for f in "$WWW_DIR"/*; do
    [ -f "$f" ] || continue
    FILE_COUNT=$((FILE_COUNT + 1))
done

if [ "$FILE_COUNT" -eq 0 ]; then
    echo "No files found in $WWW_DIR"
    exit 1
fi

echo "Uploading $FILE_COUNT file(s) to $DEVICE_IP:/www/ ..."

# Upload each file via FTP
for f in "$WWW_DIR"/*; do
    [ -f "$f" ] || continue
    FNAME="$(basename "$f")"
    echo "  $FNAME"
    curl -s -T "$f" --ftp-create-dirs \
        "ftp://$FTP_USER:$FTP_PASS@$DEVICE_IP/www/$FNAME"
done

echo "Done. $FILE_COUNT file(s) uploaded."
echo "FTP will auto-disable in ~10 minutes."
