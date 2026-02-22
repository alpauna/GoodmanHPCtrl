# Upload web pages from data\www\ to ESP32 LittleFS via HTTPS /www/upload endpoint.
# Can also upload via USB serial as a fallback (-USB flag).
#
# Usage:
#   .\scripts\update-www.ps1                      - upload all files over HTTPS
#   .\scripts\update-www.ps1 -File dashboard.html - upload a single file over HTTPS
#   .\scripts\update-www.ps1 -USB [-Port COM3]    - flash entire LittleFS via USB serial
# Requires: curl (built into Windows 10+)

param(
    [string]$File,
    [switch]$USB,
    [string]$Port
)

$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $PSScriptRoot
$WwwDir = Join-Path $ProjectDir "data\www"
$Env = "freenove_esp32_s3_wroom"

if (!(Test-Path $WwwDir)) {
    Write-Error "data\www\ directory not found"; exit 1
}

# --- USB serial mode ---

if ($USB) {
    $portArg = if ($Port) { "--upload-port $Port" } else { "" }
    Write-Host "Building and uploading LittleFS image via USB..."

    # Find PlatformIO
    $pio = Get-Command pio -ErrorAction SilentlyContinue
    if (-not $pio) {
        $pioPenv = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
        if (Test-Path $pioPenv) { $pio = $pioPenv }
        else { Write-Error "pio not found. Install PlatformIO or add it to PATH."; exit 1 }
    }

    if ($Port) {
        & $pio run -t uploadfs -e $Env --upload-port $Port
    } else {
        & $pio run -t uploadfs -e $Env
    }
    Write-Host "Done. Web pages flashed to LittleFS."
    exit 0
}

# --- HTTPS upload mode ---

function Read-Secret {
    param([string]$Label)
    $secure = Read-Host $Label -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"; exit 1
}

$AdminPW = Read-Secret "Admin password (blank if none set)"

$BaseURL = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# Verify device is reachable and auth works
Write-Host "Checking device at $DeviceIP..."
$httpCode = & curl @CurlBase -o $null -w "%{http_code}" "$BaseURL/www/list" 2>$null
if ($httpCode -eq "401") {
    Write-Error "Authentication failed."; exit 1
}
if ($httpCode -ne "200") {
    Write-Error "Could not reach device (HTTP $httpCode)"; exit 1
}

# Determine files to upload
if ($File) {
    $filePath = Join-Path $WwwDir $File
    if (!(Test-Path $filePath)) {
        Write-Error "$filePath not found"; exit 1
    }
    $files = @(Get-Item $filePath)
} else {
    $files = @(Get-ChildItem $WwwDir -File)
}

$total = 0
$failed = 0

foreach ($f in $files) {
    $sizeKB = [math]::Floor($f.Length / 1024)
    $display = $f.Name.PadRight(25)
    Write-Host -NoNewline "  $display ${sizeKB} KB ... "

    $resp = & curl @CurlBase -X POST "$BaseURL/www/upload" `
        -H "X-Filename: $($f.Name)" `
        -H "Content-Type: application/octet-stream" `
        --data-binary "@$($f.FullName)" 2>$null

    if ($resp -match '"status"\s*:\s*"ok"') {
        Write-Host "OK"
        $total++
    } else {
        Write-Host "FAILED"
        Write-Host "    $resp"
        $failed++
    }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "Done. $total file(s) uploaded successfully."
} else {
    Write-Host "Done. $total uploaded, $failed failed."
    exit 1
}
