#!/bin/bash
# Burn a random 256-bit HMAC key to ESP32-S3 eFuse BLOCK_KEY0
# This key is used by the firmware to derive AES-256-GCM encryption keys
# for password storage on the SD card.
#
# WARNING: eFuse burning is PERMANENT and IRREVERSIBLE.
# Each key block can only be written ONCE per chip.

set -e

KEY_FILE="efuse_hmac_key.bin"
PORT="${1:-/dev/ttyUSB0}"
CHIP="esp32s3"

echo "============================================"
echo "  ESP32-S3 eFuse HMAC Key Burn Utility"
echo "============================================"
echo ""
echo "This script will:"
echo "  1. Generate a random 256-bit (32-byte) key"
echo "  2. Save it to ${KEY_FILE} (backup)"
echo "  3. Burn it to eFuse BLOCK_KEY0 with HMAC_UP purpose"
echo ""
echo "WARNING: eFuse writes are PERMANENT and IRREVERSIBLE."
echo "  - BLOCK_KEY0 can only be written ONCE"
echo "  - The key will be read-protected (software cannot read it)"
echo "  - Only the hardware HMAC peripheral can use it"
echo "  - There is NO way to undo this operation"
echo ""
echo "Target port: ${PORT}"
echo ""

# Check for espefuse.py
if ! command -v espefuse.py &> /dev/null; then
    ESPEFUSE="$HOME/.platformio/penv/bin/espefuse.py"
    if [ ! -f "$ESPEFUSE" ]; then
        echo "ERROR: espefuse.py not found. Install esptool or check PlatformIO installation."
        exit 1
    fi
else
    ESPEFUSE="espefuse.py"
fi

echo "Using: $ESPEFUSE"
echo ""

# Prompt for confirmation
read -p "Are you sure you want to burn an eFuse key? This CANNOT be undone. (yes/no): " CONFIRM
if [ "$CONFIRM" != "yes" ]; then
    echo "Aborted."
    exit 0
fi

echo ""

# Generate random 32-byte key
if [ -f "$KEY_FILE" ]; then
    echo "WARNING: ${KEY_FILE} already exists."
    read -p "Overwrite and generate a new key? (yes/no): " OVERWRITE
    if [ "$OVERWRITE" != "yes" ]; then
        echo "Using existing ${KEY_FILE}"
    else
        dd if=/dev/urandom of="$KEY_FILE" bs=32 count=1 2>/dev/null
        echo "Generated new 32-byte random key -> ${KEY_FILE}"
    fi
else
    dd if=/dev/urandom of="$KEY_FILE" bs=32 count=1 2>/dev/null
    echo "Generated 32-byte random key -> ${KEY_FILE}"
fi

echo ""
echo "Key (hex):"
xxd -p -c 32 "$KEY_FILE"
echo ""

# Final confirmation
echo "FINAL WARNING: This will permanently burn the key to eFuse BLOCK_KEY0."
read -p "Type BURN to proceed: " FINAL
if [ "$FINAL" != "BURN" ]; then
    echo "Aborted. Key file ${KEY_FILE} preserved for later use."
    exit 0
fi

echo ""
echo "Burning eFuse key..."
$ESPEFUSE --port "$PORT" --chip "$CHIP" \
    burn_key BLOCK_KEY0 "$KEY_FILE" HMAC_UP

echo ""
echo "============================================"
echo "  eFuse key burned successfully!"
echo "============================================"
echo ""
echo "IMPORTANT:"
echo "  - Keep ${KEY_FILE} as a backup in a secure location"
echo "  - Do NOT commit it to git (already in .gitignore)"
echo "  - Flash normal firmware — initEncryption() will now succeed"
echo "  - Passwords will be encrypted with AES-256-GCM"
