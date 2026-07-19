# OTA firmware update or revert via HTTP
# Usage:
#   .\scripts\ota-update.ps1           - upload firmware.bin from PlatformIO build dir
#   .\scripts\ota-update.ps1 -Revert   - revert to previous firmware backup on SD card
# Requires: curl (built into Windows 10+)

param(
    [switch]$Revert
)

$ErrorActionPreference = "Stop"

$BuildDir = Join-Path $PSScriptRoot "..\.pio\build\freenove_esp32_s3_wroom"
$Firmware = Join-Path $BuildDir "firmware.bin"
$RebootTimeout = 60

# --- Helpers ---

function Wait-ForReboot {
    Write-Host "Waiting for device to come back online..."
    $elapsed = 0
    while ($elapsed -lt $RebootTimeout) {
        Start-Sleep -Seconds 2
        $elapsed += 2
        $result = & curl @script:CurlBase --connect-timeout 2 --max-time 3 "$script:BaseURL/heap" 2>$null
        if ($LASTEXITCODE -eq 0 -and $result) {
            Write-Host "Device back online after ${elapsed}s."
            return $true
        }
        Write-Host "  Waiting... ${elapsed}s"
    }
    Write-Host ""
    Write-Host "Warning: Device not responding after ${RebootTimeout}s."
    return $false
}

function Read-Secret {
    param([string]$Label)
    $secure = Read-Host $Label -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# --- Collect credentials ---

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"; exit 1
}

$AdminPW = Read-Secret "Admin password (blank if none set)"

$BaseURL = "http://$DeviceIP"
$CurlBase = @("-s")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# --- Verify device is reachable ---

Write-Host "Checking device at $DeviceIP..."
$heap = & curl @CurlBase --connect-timeout 5 "$BaseURL/heap" 2>$null
if ([string]::IsNullOrEmpty($heap)) {
    Write-Error "Could not reach device at $DeviceIP"; exit 1
}
Write-Host "Device online. $heap"

# --- Verify auth works ---

$authCode = & curl @CurlBase -o $null -w "%{http_code}" "$BaseURL/revert" 2>$null
if ($authCode -eq "401") {
    Write-Error "Authentication failed. Check admin password."; exit 1
}

# --- Revert mode ---

if ($Revert) {
    Write-Host ""
    Write-Host "Checking for firmware backup on device..."
    $backupJson = & curl @CurlBase "$BaseURL/revert" 2>$null
    if ([string]::IsNullOrEmpty($backupJson)) {
        Write-Error "Could not check backup status"; exit 1
    }

    $backup = $backupJson | ConvertFrom-Json
    if (-not $backup.exists) {
        Write-Host "No firmware backup available on device."
        exit 0
    }

    $sizeKB = [math]::Floor($backup.size / 1024)
    Write-Host "Backup available: ${sizeKB} KB"
    $confirm = Read-Host "Revert to previous firmware? [y/N]"
    if ($confirm -ne "y" -and $confirm -ne "Y") {
        Write-Host "Cancelled."; exit 0
    }

    Write-Host "Reverting firmware..."
    $resp = & curl @CurlBase -X POST "$BaseURL/revert" 2>$null
    if ($resp -eq "OK") {
        Write-Host "Revert accepted. Device is rebooting..."
    } else {
        Write-Error "Revert failed: $resp"; exit 1
    }

    Wait-ForReboot
    exit 0
}

# --- Upload mode ---

if (!(Test-Path $Firmware)) {
    Write-Error "firmware.bin not found at $Firmware`nRun 'pio run -e freenove_esp32_s3_wroom' first."
    exit 1
}

$fwSize = (Get-Item $Firmware).Length
$fwSizeKB = [math]::Floor($fwSize / 1024)
Write-Host ""
Write-Host "Firmware: $Firmware (${fwSizeKB} KB)"
$confirm = Read-Host "Upload and flash this firmware? [y/N]"
if ($confirm -ne "y" -and $confirm -ne "Y") {
    Write-Host "Cancelled."; exit 0
}

# Step 1: Upload to SD card
Write-Host ""
Write-Host "Step 1/3: Uploading firmware to SD card..."
$resp = & curl @CurlBase -X POST "$BaseURL/update" -H "Content-Type: application/octet-stream" --data-binary "@$Firmware" 2>$null

if ($resp -ne "OK") {
    Write-Error "Upload failed: $resp"; exit 1
}
Write-Host "Firmware uploaded to SD card."

# Step 2: Verify upload
Write-Host ""
Write-Host "Step 2/3: Verifying upload..."
$applyJson = & curl @CurlBase "$BaseURL/apply" 2>$null
if ($applyJson) {
    $applyStatus = $applyJson | ConvertFrom-Json
    if (-not $applyStatus.exists) {
        Write-Error "Firmware not found on SD card after upload"; exit 1
    }
    if ($applyStatus.size -ne $fwSize) {
        Write-Error "Size mismatch - local $fwSize bytes, SD card $($applyStatus.size) bytes"
        exit 1
    }
    Write-Host "Verified: ${fwSizeKB} KB on SD card matches local firmware."
} else {
    Write-Host "Warning: Could not verify upload"
}

# Step 3: Apply (backup current + flash new + reboot)
Write-Host ""
Write-Host "Step 3/3: Applying firmware (backing up current version)..."
$resp = & curl @CurlBase -X POST "$BaseURL/apply" 2>$null

if ($resp -ne "OK") {
    Write-Error "Apply failed: $resp"; exit 1
}
Write-Host "Apply accepted. Device is rebooting..."

Wait-ForReboot

# Post-reboot verification
Write-Host "Verifying new firmware..."
$newHeap = & curl @CurlBase --connect-timeout 5 "$BaseURL/heap" 2>$null
if ($newHeap) {
    Write-Host "Device healthy. $newHeap"
} else {
    Write-Host "Warning: Could not verify device health after reboot."
}

# Confirm firmware.new was cleaned up and backup exists
$applyAfter = & curl @CurlBase "$BaseURL/apply" 2>$null
$revertAfter = & curl @CurlBase "$BaseURL/revert" 2>$null

if ($applyAfter) {
    $a = $applyAfter | ConvertFrom-Json
    if ($a.exists) {
        Write-Host "Warning: /firmware.new still on SD card (apply may not have completed)"
    }
}
if ($revertAfter) {
    $r = $revertAfter | ConvertFrom-Json
    if ($r.exists) {
        $bkKB = [math]::Floor($r.size / 1024)
        Write-Host "Previous firmware backed up (${bkKB} KB) - use -Revert to roll back."
    }
}

Write-Host ""
Write-Host "OTA update complete."
