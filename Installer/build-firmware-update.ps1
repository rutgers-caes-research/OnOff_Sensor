param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$MinimumInstallerVersion,
    [Parameter(Mandatory = $true)][string]$C3Build,
    [Parameter(Mandatory = $true)][string]$S3Build,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "dist")
)

$ErrorActionPreference = "Stop"
$packageRoot = Join-Path $OutputDirectory "OnOff-Sensor-Firmware"
$zipPath = Join-Path $OutputDirectory "OnOff-Sensor-Firmware.zip"
if (Test-Path -LiteralPath $packageRoot) { throw "Package folder already exists: $packageRoot" }
if (Test-Path -LiteralPath $zipPath) { throw "Package ZIP already exists: $zipPath" }

foreach ($profile in @(@("c3", $C3Build), @("s3", $S3Build))) {
    $destination = Join-Path $packageRoot "firmware\\$($profile[0])"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $source = Join-Path $profile[1] "new_xiao_onoff_v6.ino.bin"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing build file: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination "firmware.bin")
}

$manifest = [ordered]@{
    firmwareVersion = $Version
    minimumInstallerVersion = $MinimumInstallerVersion
    boards = [ordered]@{}
}
foreach ($boardName in @("c3", "s3")) {
    $relative = "firmware/$boardName/firmware.bin"
    $path = Join-Path $packageRoot $relative
    $manifest.boards[$boardName] = [ordered]@{
        application = $relative
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $packageRoot "firmware-update.json") -Encoding UTF8
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host $zipPath
