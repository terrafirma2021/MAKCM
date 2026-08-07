param(
    [Parameter(Mandatory = $true)]
    [string]$LeftPort,
    [Parameter(Mandatory = $true)]
    [string]$RightPort,
    [switch]$AllowOtaBaseMigration
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$python = Join-Path $repo '.tools\espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
$esptool = Join-Path $repo '.tools\espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
if (-not $AllowOtaBaseMigration) {
    throw 'Full A/B base migration requires explicit -AllowOtaBaseMigration.'
}

$leftBuild = Join-Path $repo 'MAKCM_ESP32s3_Device_Mouse_Left_IDF\build'
$rightBuild = Join-Path $repo 'MAKCM_ESP32s3_HID_Mouse_Right\.pio\build\RIGHT'
$trustAnchor = Join-Path $repo 'common\ota_trust_anchor.local.inc'
$trustAnchorVerifier = Join-Path $repo 'tools\verify_ota_trust_anchor.py'
$leftBootConfig = Join-Path $leftBuild 'bootloader\config\sdkconfig.h'
$leftSegments = [ordered]@{
    '0x0' = Join-Path $leftBuild 'bootloader\bootloader.bin'
    '0x8000' = Join-Path $leftBuild 'partition_table\partition-table.bin'
    '0xf000' = Join-Path $leftBuild 'ota_data_initial.bin'
    '0x20000' = Join-Path $leftBuild 'makcm_left_idf.bin'
}
$rightSegments = [ordered]@{
    '0x0' = Join-Path $rightBuild 'bootloader.bin'
    '0x8000' = Join-Path $rightBuild 'partitions.bin'
    '0xe000' = Join-Path $rightBuild 'ota_data_initial.bin'
    '0x10000' = Join-Path $rightBuild 'firmware.bin'
}

if (-not (Test-Path -LiteralPath $python)) {
    throw "Pinned ESP-IDF Python runtime not found: $python"
}
$allImages = @($leftSegments.Values) + @($rightSegments.Values)
foreach ($image in $allImages) {
    if (-not (Test-Path -LiteralPath $image)) {
        throw "Firmware image not found: $image"
    }
}
if (-not (Test-Path -LiteralPath $trustAnchor)) {
    throw 'Pinned OTA trust anchor is absent. Generate common\ota_trust_anchor.local.inc and rebuild both boards first.'
}
if (-not (Test-Path -LiteralPath $trustAnchorVerifier)) {
    throw "OTA trust-anchor verifier is absent: $trustAnchorVerifier"
}
if (-not (Test-Path -LiteralPath $leftBootConfig) -or
    -not (Select-String -LiteralPath $leftBootConfig -Quiet `
            -SimpleMatch '#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1')) {
    throw 'Left bootloader build does not prove CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1.'
}
$expectedRightBootloaderHash = 'E01A2300DE23C8D601E9F2A37684B5DBC83AE37132D5A8833E23520AFD895F92'
$rightBootloaderHash = (Get-FileHash -LiteralPath $rightSegments['0x0'] -Algorithm SHA256).Hash
if ($rightBootloaderHash -ne $expectedRightBootloaderHash) {
    throw "Right rollback-capable pinned bootloader hash mismatch: $rightBootloaderHash"
}
$anchorTime = (Get-Item -LiteralPath $trustAnchor).LastWriteTimeUtc
foreach ($application in @($leftSegments['0x20000'], $rightSegments['0x10000'])) {
    if ((Get-Item -LiteralPath $application).LastWriteTimeUtc -lt $anchorTime) {
        throw "Application predates the trust anchor and must be rebuilt: $application"
    }
}
& $python $trustAnchorVerifier `
    --anchor $trustAnchor `
    --left-app $leftSegments['0x20000'] `
    --right-app $rightSegments['0x10000']
if ($LASTEXITCODE -ne 0) {
    throw 'Built applications do not contain the pinned OTA trust anchor.'
}
$allEntries = @($rightSegments.GetEnumerator()) + @($leftSegments.GetEnumerator())
foreach ($entry in $allEntries) {
    $hash = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash
    Write-Host "$($entry.Key) $($entry.Value) SHA256=$hash"
}

$ports = [System.IO.Ports.SerialPort]::GetPortNames()
foreach ($port in @($LeftPort, $RightPort)) {
    if ($port -notin $ports) {
        throw "$port is not present. Put that board into download mode and retry. Present ports: $($ports -join ', ')"
    }
    if ($port -eq 'COM5') {
        throw 'COM5 is the runtime/shared debug port, not a download target.'
    }
}
if ($LeftPort -eq $RightPort) {
    throw 'LeftPort and RightPort must be different.'
}

function Write-EspSegments([string]$port, [System.Collections.Specialized.OrderedDictionary]$segments) {
    $arguments = @('--chip', 'esp32s3', '--port', $port, '--baud', '460800',
                   '--before', 'no_reset', '--after', 'hard_reset', 'write_flash',
                   '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '4MB')
    foreach ($entry in $segments.GetEnumerator()) {
        $arguments += @($entry.Key, $entry.Value)
    }
    Write-Host "Flashing complete OTA base image to $port ..."
    & $esptool -m esptool @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "esptool failed for $port with exit code $LASTEXITCODE"
    }
}

Write-EspSegments $RightPort $rightSegments
Write-EspSegments $LeftPort $leftSegments
Write-Host 'Both complete A/B base images flashed and verified by esptool.'
