# Burn a random 256-bit HMAC key to ESP32-S3 eFuse BLOCK_KEY0
# This key is used by the firmware to derive AES-256-GCM encryption keys
# for password storage on the SD card.
#
# WARNING: eFuse burning is PERMANENT and IRREVERSIBLE.
# Each key block can only be written ONCE per chip.
#
# Usage: .\scripts\burn-efuse-key.ps1 [-Port COM3]
# Requires: espefuse.py (included with PlatformIO/esptool)

param(
    [string]$Port = "COM3"
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $PSScriptRoot
$KeyFile = Join-Path $ProjectDir "efuse_hmac_key.bin"
$Chip = "esp32s3"

Write-Host "============================================"
Write-Host "  ESP32-S3 eFuse HMAC Key Burn Utility"
Write-Host "============================================"
Write-Host ""
Write-Host "This script will:"
Write-Host "  1. Generate a random 256-bit (32-byte) key"
Write-Host "  2. Save it to $KeyFile (backup)"
Write-Host "  3. Burn it to eFuse BLOCK_KEY0 with HMAC_UP purpose"
Write-Host ""
Write-Host "WARNING: eFuse writes are PERMANENT and IRREVERSIBLE."
Write-Host "  - BLOCK_KEY0 can only be written ONCE"
Write-Host "  - The key will be read-protected (software cannot read it)"
Write-Host "  - Only the hardware HMAC peripheral can use it"
Write-Host "  - There is NO way to undo this operation"
Write-Host ""
Write-Host "Target port: $Port"
Write-Host ""

# Check for espefuse.py
$espefuse = Get-Command espefuse.py -ErrorAction SilentlyContinue
if (-not $espefuse) {
    $pioPenv = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\espefuse.py"
    if (Test-Path $pioPenv) {
        $espefuse = $pioPenv
    } else {
        Write-Error "espefuse.py not found. Install esptool or check PlatformIO installation."
        exit 1
    }
}
Write-Host "Using: $espefuse"
Write-Host ""

# Prompt for confirmation
$confirm = Read-Host "Are you sure you want to burn an eFuse key? This CANNOT be undone. (yes/no)"
if ($confirm -ne "yes") {
    Write-Host "Aborted."; exit 0
}
Write-Host ""

# Generate random 32-byte key
if (Test-Path $KeyFile) {
    Write-Host "WARNING: $KeyFile already exists."
    $overwrite = Read-Host "Overwrite and generate a new key? (yes/no)"
    if ($overwrite -ne "yes") {
        Write-Host "Using existing $KeyFile"
    } else {
        $bytes = New-Object byte[] 32
        [Security.Cryptography.RNGCryptoServiceProvider]::Create().GetBytes($bytes)
        [IO.File]::WriteAllBytes($KeyFile, $bytes)
        Write-Host "Generated new 32-byte random key -> $KeyFile"
    }
} else {
    $bytes = New-Object byte[] 32
    [Security.Cryptography.RNGCryptoServiceProvider]::Create().GetBytes($bytes)
    [IO.File]::WriteAllBytes($KeyFile, $bytes)
    Write-Host "Generated 32-byte random key -> $KeyFile"
}

Write-Host ""
Write-Host "Key (hex):"
$keyBytes = [IO.File]::ReadAllBytes($KeyFile)
Write-Host ([BitConverter]::ToString($keyBytes).Replace("-", "").ToLower())
Write-Host ""

# Final confirmation
Write-Host "FINAL WARNING: This will permanently burn the key to eFuse BLOCK_KEY0."
$final = Read-Host "Type BURN to proceed"
if ($final -ne "BURN") {
    Write-Host "Aborted. Key file $KeyFile preserved for later use."
    exit 0
}

Write-Host ""
Write-Host "Burning eFuse key..."
& $espefuse --port $Port --chip $Chip burn_key BLOCK_KEY0 $KeyFile HMAC_UP

Write-Host ""
Write-Host "============================================"
Write-Host "  eFuse key burned successfully!"
Write-Host "============================================"
Write-Host ""
Write-Host "IMPORTANT:"
Write-Host "  - Keep $KeyFile as a backup in a secure location"
Write-Host "  - Do NOT commit it to git (already in .gitignore)"
Write-Host "  - Flash normal firmware - initEncryption() will now succeed"
Write-Host "  - Passwords will be encrypted with AES-256-GCM"
