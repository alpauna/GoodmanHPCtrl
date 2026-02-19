#!/bin/bash
# Build and upload LittleFS filesystem image containing web pages (data/www/)
# to ESP32 flash. Replaces the old FTP-based SD card upload.
#
# Usage: ./update-www.sh [upload_port]
#   upload_port  Serial port (default: auto-detect)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
PIO="$HOME/.platformio/penv/bin/pio"
ENV="freenove_esp32_s3_wroom"

if [ ! -d "$PROJECT_DIR/data/www" ]; then
    echo "Error: data/www/ directory not found"
    exit 1
fi

PORT_ARG=""
if [ -n "$1" ]; then
    PORT_ARG="--upload-port $1"
fi

echo "Building and uploading LittleFS image..."
$PIO run -t uploadfs -e "$ENV" $PORT_ARG

echo "Done. Web pages flashed to LittleFS."
