#!/bin/bash
# Upload HTML files from data/www/ to SD card /www/ via FTP
# Usage: ./scripts/update-www.sh <device-ip> [admin-password]
#
# If admin password is set on the device, it will authenticate and
# enable FTP for 10 minutes before uploading files.

set -e

DEVICE_IP="${1:?Usage: $0 <device-ip> [admin-password]}"
ADMIN_PW="${2:-}"
FTP_USER="admin"
FTP_PASS="admin"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WWW_DIR="$SCRIPT_DIR/../data/www"

if [ ! -d "$WWW_DIR" ]; then
    echo "Error: data/www/ directory not found at $WWW_DIR"
    exit 1
fi

# Try HTTPS first, fall back to HTTP
BASE_URL="https://$DEVICE_IP"
CURL_OPTS="-sk"  # silent + allow self-signed certs
if ! curl $CURL_OPTS --connect-timeout 3 "$BASE_URL/ftp" -o /dev/null 2>/dev/null; then
    BASE_URL="http://$DEVICE_IP"
    CURL_OPTS="-s"
fi

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
    echo "No admin password provided, assuming FTP is already active..."
fi

# Wait briefly for FTP to start
sleep 1

# Build FTP upload commands
FTP_CMDS="cd /www"$'\n'
FILE_COUNT=0
for f in "$WWW_DIR"/*; do
    [ -f "$f" ] || continue
    FNAME="$(basename "$f")"
    FTP_CMDS+="put $f $FNAME"$'\n'
    FILE_COUNT=$((FILE_COUNT + 1))
done
FTP_CMDS+="bye"$'\n'

if [ "$FILE_COUNT" -eq 0 ]; then
    echo "No files found in $WWW_DIR"
    exit 1
fi

echo "Uploading $FILE_COUNT file(s) to $DEVICE_IP:/www/ ..."

# Upload via FTP
curl -s -T "{$(cd "$WWW_DIR" && ls -1 | tr '\n' ',' | sed 's/,$//')}" \
    --ftp-create-dirs \
    "ftp://$FTP_USER:$FTP_PASS@$DEVICE_IP/www/" \
    2>&1

echo "Done. $FILE_COUNT file(s) uploaded."
