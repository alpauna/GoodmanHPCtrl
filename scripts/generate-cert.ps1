# Generate self-signed ECC P-256 certificate for ESP32 HTTPS server
# Usage: .\scripts\generate-cert.ps1 [-Name "System Name"]
# Requires: openssl (included with Git for Windows)
# Output files are written to the project root directory.
# Copy cert.pem and key.pem to SD card root after generation.

param(
    [string]$Name = "ESP32"
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $PSScriptRoot

openssl req -x509 `
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 `
    -nodes `
    -keyout "$ProjectDir\key.pem" `
    -out "$ProjectDir\cert.pem" `
    -days 3650 `
    -subj "/CN=$Name"

if ($LASTEXITCODE -ne 0) {
    Write-Error "openssl command failed. Is openssl installed? Git for Windows includes openssl."
    exit 1
}

Write-Host "Generated $ProjectDir\cert.pem and $ProjectDir\key.pem (CN=$Name)"
Write-Host "Copy both files to the root of the SD card."
