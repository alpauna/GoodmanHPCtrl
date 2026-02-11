#!/bin/bash
# Generate self-signed ECC P-256 certificate for ESP32 HTTPS server
# Output files are written to the project root directory.
# Copy cert.pem and key.pem to SD card root after generation.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

openssl req -x509 \
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes \
    -keyout "$PROJECT_DIR/key.pem" \
    -out "$PROJECT_DIR/cert.pem" \
    -days 3650 \
    -subj '/CN=ESP32'

echo "Generated $PROJECT_DIR/cert.pem and $PROJECT_DIR/key.pem"
echo "Copy both files to the root of the SD card."
