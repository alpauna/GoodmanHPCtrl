#!/bin/bash
# Generate self-signed ECC P-256 certificate for ESP32 HTTPS server
# Copy cert.pem and key.pem to SD card root after generation

set -e

openssl req -x509 \
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes \
    -keyout key.pem \
    -out cert.pem \
    -days 3650 \
    -subj '/CN=ESP32'

echo "Generated cert.pem and key.pem"
echo "Copy both files to the root of the SD card."
