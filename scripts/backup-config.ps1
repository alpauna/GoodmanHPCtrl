# Download config.txt from device SD card for local backup
# Usage: .\scripts\backup-config.ps1
# Requires: curl (built into Windows 10+)

$ErrorActionPreference = "Stop"

$BackupDir = Join-Path $PSScriptRoot "..\backups"
$FtpUser = "admin"
$FtpPass = "admin"

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"; exit 1
}

$secure = Read-Host "Admin password (blank if none set)" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
$AdminPW = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)

$BaseURL = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# Verify device is reachable
Write-Host "Checking device at $DeviceIP..."
$heap = & curl @CurlBase --connect-timeout 5 "$BaseURL/heap" 2>$null
if ([string]::IsNullOrEmpty($heap)) {
    $heap = & curl @CurlBase --connect-timeout 5 "http://${DeviceIP}/heap" 2>$null
}
if ([string]::IsNullOrEmpty($heap)) {
    Write-Error "Could not reach device at $DeviceIP"; exit 1
}
Write-Host "Device online."

# Enable FTP for 10 minutes
Write-Host "Enabling FTP for 10 minutes..."
$resp = & curl @CurlBase -X POST "$BaseURL/ftp" -H "Content-Type: application/json" -d '{"duration":10}' 2>$null
if ($resp -match '"error"') {
    Write-Error "Error: $resp"; exit 1
}
Write-Host "FTP enabled."

Start-Sleep -Seconds 2

# Create backup directory with timestamp
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$destDir = Join-Path $BackupDir $timestamp
New-Item -ItemType Directory -Path $destDir -Force | Out-Null

# Download config.txt
Write-Host "Downloading config.txt..."
$destFile = Join-Path $destDir "config.txt"
& curl -s -o $destFile "ftp://${FtpUser}:${FtpPass}@${DeviceIP}/config.txt"

if (!(Test-Path $destFile) -or (Get-Item $destFile).Length -eq 0) {
    Write-Error "Downloaded file is empty or missing"
    Remove-Item $destDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

$size = (Get-Item $destFile).Length
Write-Host "Saved: $destFile ($size bytes)"

# Also keep a "latest" copy
New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null
Copy-Item $destFile (Join-Path $BackupDir "config-latest.txt") -Force
Write-Host "Latest copy: $(Join-Path $BackupDir 'config-latest.txt')"

Write-Host ""
Write-Host "Backup complete."
