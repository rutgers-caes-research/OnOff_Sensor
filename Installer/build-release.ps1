param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$C3Build,
    [Parameter(Mandatory = $true)][string]$S3Build,
    [Parameter(Mandatory = $true)][string]$BootAppPath,
    [Parameter(Mandatory = $true)][string]$EsptoolPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "dist")
)

$ErrorActionPreference = "Stop"
$packageName = "OnOff-Sensor-Installer-$Version"
$packageRoot = Join-Path $OutputDirectory $packageName
$zipPath = Join-Path $OutputDirectory "$packageName.zip"

if (Test-Path -LiteralPath $packageRoot) { throw "Package folder already exists: $packageRoot" }
if (Test-Path -LiteralPath $zipPath) { throw "Package ZIP already exists: $zipPath" }

New-Item -ItemType Directory -Path $packageRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot "firmware\c3") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot "firmware\s3") | Out-Null

Copy-Item -LiteralPath (Join-Path $PSScriptRoot "Install OnOff Sensor.bat") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "install.ps1") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "README.txt") -Destination $packageRoot
Copy-Item -LiteralPath $EsptoolPath -Destination (Join-Path $packageRoot "esptool.exe")

$licensePath = Join-Path (Split-Path -Parent $EsptoolPath) "LICENSE"
if (Test-Path -LiteralPath $licensePath) {
    Copy-Item -LiteralPath $licensePath -Destination (Join-Path $packageRoot "esptool-LICENSE.txt")
}

$sourceNames = @(
    "new_xiao_onoff_v6.ino.bootloader.bin",
    "new_xiao_onoff_v6.ino.partitions.bin",
    "new_xiao_onoff_v6.ino.bin"
)
$targetNames = @("bootloader.bin", "partitions.bin", "firmware.bin")

foreach ($profile in @(@("c3", $C3Build), @("s3", $S3Build))) {
    for ($index = 0; $index -lt $sourceNames.Count; $index++) {
        $source = Join-Path $profile[1] $sourceNames[$index]
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing build file: $source" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $packageRoot "firmware\$($profile[0])\$($targetNames[$index])")
    }
    if (-not (Test-Path -LiteralPath $BootAppPath -PathType Leaf)) { throw "Missing build file: $BootAppPath" }
    Copy-Item -LiteralPath $BootAppPath -Destination (Join-Path $packageRoot "firmware\$($profile[0])\boot_app0.bin")
}

function Get-RelativeHashes([string]$Profile) {
    $hashes = [ordered]@{}
    Get-ChildItem -LiteralPath (Join-Path $packageRoot "firmware\$Profile") -File | Sort-Object Name | ForEach-Object {
        $relative = "firmware/$Profile/$($_.Name)"
        $hashes[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}

$manifest = [ordered]@{
    version = $Version
    boards = [ordered]@{
        c3 = [ordered]@{
            chip = "esp32c3"; flashSize = "4MB"
            bootloader = "firmware/c3/bootloader.bin"; partitions = "firmware/c3/partitions.bin"
            bootApp = "firmware/c3/boot_app0.bin"; application = "firmware/c3/firmware.bin"
            files = Get-RelativeHashes "c3"
        }
        s3 = [ordered]@{
            chip = "esp32s3"; flashSize = "8MB"
            bootloader = "firmware/s3/bootloader.bin"; partitions = "firmware/s3/partitions.bin"
            bootApp = "firmware/s3/boot_app0.bin"; application = "firmware/s3/firmware.bin"
            files = Get-RelativeHashes "s3"
        }
    }
}

$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $packageRoot "manifest.json") -Encoding UTF8
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host $zipPath
