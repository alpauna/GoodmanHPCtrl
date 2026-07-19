# Restore config.txt to device SD card from a local backup
# Usage:
#   .\scripts\restore-config.ps1                              — pick from available backups
#   .\scripts\restore-config.ps1 -File backups\config-latest.txt  — restore a specific file

param(
    [string]$File = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackupDir = Join-Path (Split-Path -Parent $ScriptDir) "backups"

$FtpUser = "admin"
$FtpPass = ""

# --- Determine which file to restore ---

$ConfigFile = ""
if ($File -ne "") {
    if (-not (Test-Path $File)) {
        Write-Host "Error: File not found: $File"
        exit 1
    }
    $ConfigFile = (Resolve-Path $File).Path
} else {
    if (-not (Test-Path $BackupDir)) {
        Write-Host "Error: No backups directory found. Run backup-config.ps1 first."
        exit 1
    }

    Write-Host "Available backups:"
    Write-Host ""

    $backupFiles = @()

    # Latest copy
    $latestFile = Join-Path $BackupDir "config-latest.txt"
    if (Test-Path $latestFile) {
        $size = (Get-Item $latestFile).Length
        $backupFiles += $latestFile
        Write-Host ("  [{0}] config-latest.txt ({1} bytes)" -f $backupFiles.Count, $size)
    }

    # Timestamped backups (newest first)
    $dirs = Get-ChildItem -Path $BackupDir -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}$' } | Sort-Object Name -Descending
    foreach ($dir in $dirs) {
        $cfg = Join-Path $dir.FullName "config.txt"
        if (Test-Path $cfg) {
            $size = (Get-Item $cfg).Length
            $backupFiles += $cfg
            Write-Host ("  [{0}] {1}\config.txt ({2} bytes)" -f $backupFiles.Count, $dir.Name, $size)
        }
    }

    if ($backupFiles.Count -eq 0) {
        Write-Host "  (none found)"
        Write-Host ""
        Write-Host "Run backup-config.ps1 first, or specify a file with -File."
        exit 1
    }

    Write-Host ""
    $choice = Read-Host "Select backup to restore [1-$($backupFiles.Count)]"
    $idx = 0
    if (-not [int]::TryParse($choice, [ref]$idx) -or $idx -lt 1 -or $idx -gt $backupFiles.Count) {
        Write-Host "Invalid selection."
        exit 1
    }
    $ConfigFile = $backupFiles[$idx - 1]
}

$fileSize = (Get-Item $ConfigFile).Length
Write-Host ""
Write-Host "Restoring: $ConfigFile ($fileSize bytes)"

# Validate JSON
try {
    Get-Content $ConfigFile -Raw | ConvertFrom-Json | Out-Null
} catch {
    Write-Host "Warning: File does not appear to be valid JSON."
    $cont = Read-Host "Continue anyway? [y/N]"
    if ($cont -ne "y" -and $cont -ne "Y") {
        Write-Host "Cancelled."
        exit 0
    }
}

# --- Connect to device ---

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrWhiteSpace($DeviceIP)) {
    Write-Host "Error: IP address required"
    exit 1
}

$AdminPwSecure = Read-Host "Admin password (blank if none set)" -AsSecureString
$AdminPw = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto(
    [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($AdminPwSecure))

$BaseUrl = "http://$DeviceIP"
$headers = @{ "Content-Type" = "application/json" }
$authHeader = @{}
if ($AdminPw -ne "") {
    $pair = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("admin:$AdminPw"))
    $authHeader = @{ "Authorization" = "Basic $pair" }
}

# Verify device is reachable
Write-Host "Checking device at $DeviceIP..."
try {
    $heap = Invoke-RestMethod -Uri "$BaseUrl/heap" -TimeoutSec 5 -ErrorAction Stop
    Write-Host "Device online."
} catch {
    Write-Host "Error: Could not reach device at $DeviceIP"
    exit 1
}

# Enable FTP for 10 minutes
Write-Host "Enabling FTP for 10 minutes..."
try {
    $resp = Invoke-RestMethod -Uri "$BaseUrl/ftp" -Method Post -Headers ($headers + $authHeader) `
        -Body '{"duration":10}' -TimeoutSec 10
    if ($resp.error) {
        Write-Host "Error: $($resp.error)"
        exit 1
    }
    Write-Host "FTP enabled."
} catch {
    Write-Host "Error enabling FTP: $_"
    exit 1
}

# Get FTP password from status endpoint
try {
    $ftpStatus = Invoke-RestMethod -Uri "$BaseUrl/ftp" -Headers $authHeader -TimeoutSec 5
    $FtpPass = if ($ftpStatus.password) { $ftpStatus.password } else { "admin" }
} catch {
    $FtpPass = "admin"
}
Write-Host "FTP password: $FtpPass"

Start-Sleep -Seconds 2

# Upload config.txt via FTP
Write-Host "Uploading config.txt to device..."
$ftpUri = "ftp://${DeviceIP}/config.txt"
$ftpClient = New-Object System.Net.WebClient
$ftpClient.Credentials = New-Object System.Net.NetworkCredential($FtpUser, $FtpPass)
try {
    $ftpClient.UploadFile($ftpUri, $ConfigFile)
    Write-Host "Upload complete."
} catch {
    Write-Host "Error uploading: $_"
    exit 1
}

# Verify upload
Write-Host "Verifying upload..."
try {
    $tempFile = [System.IO.Path]::GetTempFileName()
    $ftpClient.DownloadFile($ftpUri, $tempFile)
    $remoteSize = (Get-Item $tempFile).Length
    Remove-Item $tempFile -Force
    if ($remoteSize -eq $fileSize) {
        Write-Host "Verified: $remoteSize bytes on device matches local file."
    } else {
        Write-Host "Warning: Size mismatch - local $fileSize bytes, device $remoteSize bytes"
    }
} catch {
    Write-Host "Warning: Could not verify upload"
}

# Disable FTP
try {
    Invoke-RestMethod -Uri "$BaseUrl/ftp" -Method Post -Headers ($headers + $authHeader) `
        -Body '{"duration":0}' -TimeoutSec 5 | Out-Null
} catch { }

Write-Host ""
Write-Host "Config restored. Reboot the device to load the new configuration."
$reboot = Read-Host "Reboot now? [y/N]"
if ($reboot -eq "y" -or $reboot -eq "Y") {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/reboot" -Method Post -Headers $authHeader -TimeoutSec 5 | Out-Null
    } catch { }
    Write-Host "Reboot initiated."
}
