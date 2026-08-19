param(
    [switch]$DetectOnly,
    [switch]$SkipUpdate,
    [switch]$CheckForUpdatesOnly,
    [switch]$Confirmed
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

function Convert-Version([string]$Value) {
    if ($Value -notmatch '^v?(\d+)\.(\d+)\.(\d+)(?:-([A-Za-z]+)(\d+)?)?$') { return $null }
    return [pscustomobject]@{
        Major = [int]$matches[1]
        Minor = [int]$matches[2]
        Patch = [int]$matches[3]
        Label = [string]$matches[4]
        LabelNumber = if ($matches[5]) { [int]$matches[5] } else { 0 }
    }
}

function Compare-Version([string]$Left, [string]$Right) {
    $a = Convert-Version $Left
    $b = Convert-Version $Right
    if ($null -eq $a -or $null -eq $b) { return [string]::Compare($Left, $Right, $true) }
    foreach ($property in @('Major', 'Minor', 'Patch')) {
        if ($a.$property -lt $b.$property) { return -1 }
        if ($a.$property -gt $b.$property) { return 1 }
    }
    if (-not $a.Label -and $b.Label) { return 1 }
    if ($a.Label -and -not $b.Label) { return -1 }
    $labelComparison = [string]::Compare($a.Label, $b.Label, $true)
    if ($labelComparison -ne 0) { return $labelComparison }
    return $a.LabelNumber.CompareTo($b.LabelNumber)
}

function Copy-PackageContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
        New-Item -ItemType Directory -Path $Destination | Out-Null
    }
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

function Start-NewerInstaller($Release, $Asset) {
    $installerParent = Split-Path -Parent $packageRoot
    $stablePackageRoot = Join-Path $installerParent "OnOff-Sensor-Installer"
    $savedZipPath = Join-Path $installerParent "OnOff-Sensor-Installer.zip"
    $stagingRoot = Join-Path $installerParent (".OnOff-Sensor-Update-" + [guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $stagingRoot | Out-Null
        $stagedZipPath = Join-Path $stagingRoot "OnOff-Sensor-Installer.zip"
        Write-Host "Downloading newer release $($Release.tag_name)..."
        Invoke-WebRequest -UseBasicParsing -Uri $Asset.browser_download_url -OutFile $stagedZipPath
        Expand-Archive -LiteralPath $stagedZipPath -DestinationPath $stagingRoot
        $stagedScript = Get-ChildItem -LiteralPath $stagingRoot -Recurse -File -Filter "install.ps1" | Select-Object -First 1
        if ($null -eq $stagedScript) { throw "Downloaded release does not contain install.ps1" }
        $stagedPackageRoot = Split-Path -Parent $stagedScript.FullName
        $stagedManifestPath = Join-Path $stagedPackageRoot "manifest.json"
        if (-not (Test-Path -LiteralPath $stagedManifestPath -PathType Leaf)) {
            throw "Downloaded release does not contain manifest.json"
        }
        $stagedManifest = Get-Content -LiteralPath $stagedManifestPath -Raw | ConvertFrom-Json
        if ([string]$stagedManifest.version -ne [string]$Release.tag_name) {
            throw "Downloaded installer version does not match release $($Release.tag_name)"
        }
        $childArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $stagedScript.FullName, '-SkipUpdate')
        if ($DetectOnly) { $childArguments += '-DetectOnly' }
        if ($CheckForUpdatesOnly) { $childArguments += '-CheckForUpdatesOnly' }
        if ($Confirmed) { $childArguments += '-Confirmed' }
        & powershell.exe @childArguments
        $result = $LASTEXITCODE
        if ($result -ne 0) { return $result }

        Copy-PackageContents $stagedPackageRoot $stablePackageRoot
        Copy-Item -LiteralPath $stagedZipPath -Destination $savedZipPath -Force
        $savedManifest = Get-Content -LiteralPath (Join-Path $stablePackageRoot "manifest.json") -Raw | ConvertFrom-Json
        if ([string]$savedManifest.version -ne [string]$Release.tag_name) {
            throw "Saved installer does not match release $($Release.tag_name)"
        }
        Write-Host "Installer updated in: $stablePackageRoot" -ForegroundColor Green
        return 0
    } finally {
        $resolvedStaging = [IO.Path]::GetFullPath($stagingRoot)
        $resolvedParent = [IO.Path]::GetFullPath($installerParent).TrimEnd('\') + '\'
        if ($resolvedStaging.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedStaging) -like '.OnOff-Sensor-Update-*' -and
            (Test-Path -LiteralPath $resolvedStaging)) {
            Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
        }
    }
}

$availableRelease = $null
$availableAsset = $null
if (-not $SkipUpdate) {
    $release = $null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $headers = @{ 'User-Agent' = 'OnOff-Sensor-Installer'; 'Accept' = 'application/vnd.github+json' }
        $releases = @(Invoke-RestMethod -Headers $headers -Uri 'https://api.github.com/repos/rutgers-caes-research/OnOff_Sensor/releases?per_page=20')
        $usePrereleases = ([string]$manifest.version) -match '-'
        $release = $releases | Where-Object { -not $_.draft -and ($usePrereleases -or -not $_.prerelease) } | Select-Object -First 1
    } catch {
        Write-Host "Update check unavailable; using included firmware."
        $release = $null
    }

    if ($null -ne $release -and (Compare-Version ([string]$release.tag_name) ([string]$manifest.version)) -gt 0) {
        $asset = $release.assets | Where-Object {
            $_.name -eq 'OnOff-Sensor-Installer.zip' -or $_.name -like 'OnOff-Sensor-Installer-*.zip'
        } | Select-Object -First 1
        if ($null -eq $asset) {
            Stop-Installer "Release $($release.tag_name) does not contain a complete installer ZIP."
        }

        if ($CheckForUpdatesOnly) {
            Write-Host "A newer OnOff Sensor release is available: $($release.tag_name)"
            Write-Host "Update check complete."
            exit 0
        }
        $availableRelease = $release
        $availableAsset = $asset
    } else {
        Write-Host "Installer firmware is current: $($manifest.version)"
    }
}

if ($CheckForUpdatesOnly) {
    Write-Host "Update check complete."
    exit 0
}

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

if (-not $Confirmed) {
    if ($null -ne $availableRelease) {
        Write-Host " UPDATE AVAILABLE: $($availableRelease.tag_name) " -ForegroundColor Black -BackgroundColor Yellow
        $answer = Read-Host "Are you sure you want to install included $($manifest.version)? Type YES, or UPDATE for the latest release"
        if ($answer -ieq "UPDATE") {
            try {
                $Confirmed = $true
                $result = Start-NewerInstaller $availableRelease $availableAsset
                exit $result
            } catch {
                Stop-Installer "The update could not be downloaded or started. No firmware was installed. $($_.Exception.Message)"
            }
        }
        if ($answer -cne "YES") {
            Write-Host "Cancelled. No firmware was installed."
            exit 2
        }
    } else {
        $answer = Read-Host "Install OnOff Sensor $($manifest.version)? Type YES to continue"
        if ($answer -cne "YES") {
            Write-Host "Cancelled."
            exit 2
        }
    }
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
