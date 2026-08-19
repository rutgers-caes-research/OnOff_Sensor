param(
    [switch]$DetectOnly
)

$ErrorActionPreference = "Stop"
$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolPath = Join-Path $packageRoot "esptool.exe"
$manifestPath = Join-Path $packageRoot "manifest.json"

function Stop-Installer([string]$Message) {
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
    Stop-Installer "esptool.exe is missing. Download the complete installer ZIP again."
}
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Stop-Installer "manifest.json is missing. Download the complete installer ZIP again."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

foreach ($boardName in @("c3", "s3")) {
    $board = $manifest.boards.$boardName
    foreach ($file in $board.files.PSObject.Properties) {
        $filePath = Join-Path $packageRoot $file.Name
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            Stop-Installer "Required firmware file is missing: $($file.Name)"
        }
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne [string]$file.Value) {
            Stop-Installer "Firmware verification failed: $($file.Name)"
        }
    }
}

function Get-BoardAtPort([string]$Port) {
    $lines = & $toolPath --port $Port --connect-attempts 2 chip-id 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($lines | Out-String)
    if ($exitCode -ne 0) { return $null }
    if ($text -match "ESP32-C3") {
        return [pscustomobject]@{ Port = $Port; Profile = "c3"; Name = "XIAO ESP32-C3" }
    }
    if ($text -match "ESP32-S3") {
        return [pscustomobject]@{ Port = $Port; Profile = "s3"; Name = "XIAO ESP32-S3" }
    }
    return $null
}

Write-Host "OnOff Sensor Installer $($manifest.version)"
Write-Host "Connect one sensor by USB and close Arduino Serial Monitor."
Write-Host "Searching for a supported sensor..."

$ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
if ($ports.Count -eq 0) {
    Stop-Installer "No COM ports were found. Reconnect the sensor with a USB data cable."
}

$detected = @()
foreach ($port in $ports) {
    $board = Get-BoardAtPort $port
    if ($null -ne $board) { $detected += $board }
}

if ($detected.Count -eq 0) {
    Stop-Installer "No supported C3 or S3 sensor responded. Hold BOOT, reconnect USB, and try again."
}

if ($detected.Count -gt 1) {
    Write-Host "More than one sensor was detected:"
    for ($index = 0; $index -lt $detected.Count; $index++) {
        Write-Host "  $($index + 1). $($detected[$index].Name) on $($detected[$index].Port)"
    }
    $choice = Read-Host "Enter the sensor number"
    $number = 0
    if (-not [int]::TryParse($choice, [ref]$number) -or $number -lt 1 -or $number -gt $detected.Count) {
        Stop-Installer "Invalid sensor selection."
    }
    $target = $detected[$number - 1]
} else {
    $target = $detected[0]
}

Write-Host "Detected $($target.Name) on $($target.Port)." -ForegroundColor Green
if ($DetectOnly) {
    Write-Host "Detection test passed. No flash was written."
    exit 0
}

$answer = Read-Host "Install OnOff Sensor $($manifest.version)? Type YES to continue"
if ($answer -cne "YES") {
    Write-Host "Cancelled."
    exit 2
}

$profile = $manifest.boards.($target.Profile)
$bootloaderPath = Join-Path $packageRoot ([string]$profile.bootloader)
$partitionsPath = Join-Path $packageRoot ([string]$profile.partitions)
$bootAppPath = Join-Path $packageRoot ([string]$profile.bootApp)
$applicationPath = Join-Path $packageRoot ([string]$profile.application)
$arguments = @(
    "--chip", [string]$profile.chip,
    "--port", [string]$target.Port,
    "--baud", "460800",
    "--before", "default-reset",
    "--after", "hard-reset",
    "write-flash",
    "--flash-mode", "dio",
    "--flash-freq", "80m",
    "--flash-size", [string]$profile.flashSize,
    "0x0", $bootloaderPath,
    "0x8000", $partitionsPath,
    "0xe000", $bootAppPath,
    "0x10000", $applicationPath
)

Write-Host "Installing and verifying firmware..."
& $toolPath @arguments
if ($LASTEXITCODE -ne 0) {
    Stop-Installer "Flashing failed. Reconnect the sensor, hold BOOT, and run the installer again."
}

Write-Host "Installation complete. The sensor is restarting." -ForegroundColor Green
exit 0
