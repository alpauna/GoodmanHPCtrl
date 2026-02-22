#!/bin/bash
# Generate self-signed ECC P-256 certificate for ESP32 HTTPS server
# Usage: ./generate-cert.sh [system-name]
# Output files are written to the project root directory.
# Copy cert.pem and key.pem to SD card root after generation.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

CN="${1:-ESP32}"

openssl req -x509 \
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes \
    -keyout "$PROJECT_DIR/key.pem" \
    -out "$PROJECT_DIR/cert.pem" \
    -days 3650 \
    -subj "/CN=$CN"

echo "Generated $PROJECT_DIR/cert.pem and $PROJECT_DIR/key.pem (CN=$CN)"
echo "Copy both files to the root of the SD card."
