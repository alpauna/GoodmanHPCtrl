# Configure WiFi and MQTT credentials on the device
# Usage:
#   .\scripts\configure.ps1          - prompts for IP and updates via HTTPS API
#   .\scripts\configure.ps1 -Local   - write config.txt for SD card
# Requires: curl (built into Windows 10+)

param(
    [switch]$Local
)

$ErrorActionPreference = "Stop"

# --- Helpers ---

function Read-Prompt {
    param([string]$Label, [string]$Default)
    if ($Default) {
        $input = Read-Host "$Label [$Default]"
        if ([string]::IsNullOrEmpty($input)) { return $Default }
        return $input
    } else {
        return Read-Host $Label
    }
}

function Read-Secret {
    param([string]$Label)
    $secure = Read-Host $Label -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

function Validate-SystemName {
    param([string]$Raw)
    # Strip non-alphanumeric/space, truncate to 20, trim
    $cleaned = ($Raw -replace '[^A-Za-z0-9 ]', '')
    if ($cleaned.Length -gt 20) { $cleaned = $cleaned.Substring(0, 20) }
    $cleaned = $cleaned.Trim()
    if ([string]::IsNullOrEmpty($cleaned)) {
        Write-Error "System Name must contain at least one alphanumeric character"
        exit 1
    }
    if ($cleaned -ne $Raw) {
        Write-Host "Note: System Name sanitized to: $cleaned"
    }
    return $cleaned
}

# --- Local config.txt generation ---

if ($Local) {
    Write-Host "=== Generate config.txt for SD card ==="
    Write-Host "Passwords will be stored in plaintext and encrypted on first device boot."
    Write-Host ""

    $SysName    = Read-Prompt "System Name (max 20 chars, alphanumeric+spaces)" "Goodman HP"
    $SysName    = Validate-SystemName $SysName
    $MqttPrefix = Read-Prompt "MQTT Topic Prefix" "goodman"
    $WifiSSID   = Read-Prompt "WiFi SSID"
    $WifiPW     = Read-Secret "WiFi Password"
    $MqttHost   = Read-Prompt "MQTT Host" "192.168.0.46"
    $MqttPort   = Read-Prompt "MQTT Port" "1883"
    $MqttUser   = Read-Prompt "MQTT User" "debian"
    $MqttPW     = Read-Secret "MQTT Password"

    $OutFile = Join-Path $PSScriptRoot "..\data\config.txt"
    $DateTime = Get-Date -Format "MMM dd yyyy HH:mm:ss"

    $config = @{
        project     = "Goodman Heatpump Control"
        created     = $DateTime
        description = "Control Goodman heatpump including defrost mode."
        system      = @{ name = $SysName; mqttPrefix = $MqttPrefix }
        wifi        = @{ ssid = $WifiSSID; password = $WifiPW }
        mqtt        = @{ user = $MqttUser; password = $MqttPW; host = $MqttHost; port = [int]$MqttPort }
        logging     = @{ maxLogSize = 52428800; maxOldLogCount = 10 }
        runtime     = @{ heatAccumulatedMs = 0 }
        timezone    = @{ gmtOffset = -21600; daylightOffset = 3600 }
        heatpump    = @{ lowTemp = @{ threshold = 20.0 } }
        admin       = @{ password = "" }
        sensors     = @{ temp = @{} }
    } | ConvertTo-Json -Depth 4

    $config | Set-Content -Path $OutFile -Encoding UTF8
    Write-Host ""
    Write-Host "Written to: $OutFile"
    Write-Host "Copy this file to the root of the SD card as /config.txt"
    exit 0
}

# --- Network config via HTTPS API ---

$DeviceIP = Read-Host "Device IP"
if ([string]::IsNullOrEmpty($DeviceIP)) {
    Write-Error "IP address required"
    exit 1
}

$AdminPW = Read-Secret "Admin password (blank if none set)"

$BaseURL = "https://$DeviceIP"
$CurlBase = @("-sk")
if ($AdminPW) { $CurlBase += @("-u", "admin:$AdminPW") }

# Fetch current config
Write-Host "Fetching current config from $DeviceIP..."
$TempFile = [IO.Path]::GetTempFileName()
& curl @CurlBase "$BaseURL/config?format=json" -o $TempFile 2>$null
if (!(Test-Path $TempFile) -or (Get-Item $TempFile).Length -eq 0) {
    Write-Error "Could not reach device at $DeviceIP"
    exit 1
}

try {
    $current = Get-Content $TempFile -Raw | ConvertFrom-Json
} catch {
    Write-Error "Could not reach device at $DeviceIP"
    exit 1
} finally {
    Remove-Item $TempFile -ErrorAction SilentlyContinue
}

$CurSysName    = if ($current.systemName)  { $current.systemName }  else { "Goodman HP" }
$CurMqttPrefix = if ($current.mqttPrefix)  { $current.mqttPrefix }  else { "goodman" }
$CurSSID       = $current.wifiSSID
$CurMqttHost   = $current.mqttHost
$CurMqttPort   = $current.mqttPort
$CurMqttUser   = $current.mqttUser

Write-Host ""
Write-Host "=== Configure Device ==="
Write-Host "Leave blank to keep current value. Passwords always required for changes."
Write-Host ""

$SysName    = Read-Prompt "System Name (max 20 chars, alphanumeric+spaces)" $CurSysName
$SysName    = Validate-SystemName $SysName
$MqttPrefix = Read-Prompt "MQTT Topic Prefix" $CurMqttPrefix
$WifiSSID   = Read-Prompt "WiFi SSID" $CurSSID

$WifiPW = Read-Secret "WiFi Password (blank=no change)"
$CurWifiPW = ""
if ($WifiPW) {
    $CurWifiPW = Read-Secret "Current WiFi Password (required to change)"
}

$MqttHost = Read-Prompt "MQTT Host" $CurMqttHost
$MqttPort = Read-Prompt "MQTT Port" $CurMqttPort
$MqttUser = Read-Prompt "MQTT User" $CurMqttUser

$MqttPW = Read-Secret "MQTT Password (blank=no change)"
$CurMqttPW = ""
if ($MqttPW) {
    $CurMqttPW = Read-Secret "Current MQTT Password (required to change)"
}

# Build JSON payload
$payload = @{
    systemName = $SysName
    mqttPrefix = $MqttPrefix
    wifiSSID   = $WifiSSID
    mqttHost   = $MqttHost
    mqttPort   = [int]$MqttPort
    mqttUser   = $MqttUser
}
if ($WifiPW) {
    $payload.wifiPassword = $WifiPW
    $payload.curWifiPw    = $CurWifiPW
}
if ($MqttPW) {
    $payload.mqttPassword = $MqttPW
    $payload.curMqttPw    = $CurMqttPW
}

$jsonFile = [IO.Path]::GetTempFileName()
$payload | ConvertTo-Json | Set-Content $jsonFile -Encoding UTF8

Write-Host ""
Write-Host "Saving configuration..."
$resp = & curl @CurlBase -X POST "$BaseURL/config" -H "Content-Type: application/json" -d "@$jsonFile" 2>$null
Remove-Item $jsonFile -ErrorAction SilentlyContinue

Write-Host "Response: $resp"
