$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = [IO.Path]::GetFullPath((Join-Path $tempBase "gateway-dehumidifier-config-test"))
if (-not $tempRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) {
    throw "test path escaped the system temporary directory: $tempRoot"
}

if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

$inputPath = Join-Path $tempRoot "input.json"
$outputPath = Join-Path $tempRoot "output.json"
$fixture = [ordered]@{
    schemaVersion = "1.1.0"
    meters = @(
        [ordered]@{
            meterCode = "COMM202600104_LIQUID_COOLING"
            deviceName = "liquid cooling fixture"
            slave = 1
            points = @(
                [ordered]@{
                    index = 131
                    pointCode = "liquid_temperature_fixture"
                    name = "liquid temperature fixture"
                    read = [ordered]@{ scale = 0.1 }
                    write = [ordered]@{ enable = $false }
                }
            )
        },
        [ordered]@{
            meterCode = "COMM202600104_DEHUMIDIFIER"
            deviceName = "dehumidifier fixture"
            enabled = $false
            slave = 2
            protocolType = "modbus_rtu"
            sourceCom = "COM5"
            sourceAddress = 2
            points = @()
        }
    )
}
$fixture | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $inputPath -Encoding UTF8

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

try {
    & (Join-Path $PSScriptRoot "generate_60w_dehumidifier_config.ps1") `
        -InputConfig $inputPath `
        -OutputConfig $outputPath `
        -SlaveAddress 2 `
        -ReadOnly | Out-Null

    $generated = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $dehumidifier = @($generated.meters | Where-Object meterCode -eq "COMM202600104_DEHUMIDIFIER")
    Require ($dehumidifier.Count -eq 1) "generated config must contain one dehumidifier meter"

    $points = @($dehumidifier[0].points)
    $cabinetTemperature = @($points | Where-Object index -eq 189)
    $cabinetHumidity = @($points | Where-Object index -eq 190)
    $internalTemperature = @($points | Where-Object index -eq 217)
    Require ($cabinetTemperature.Count -eq 1) "cabinet temperature point 189 is missing"
    Require ($cabinetHumidity.Count -eq 1) "cabinet humidity point 190 is missing"
    Require ($internalTemperature.Count -eq 1) "dehumidifier internal temperature point 217 is missing"

    Require ([double]$cabinetTemperature[0].read.scale -eq 1.0) `
        "point 189 must use the integer register value; display precision is not a scale"
    Require ([double]$cabinetHumidity[0].read.scale -eq 1.0) `
        "point 190 must use the integer register value; display precision is not a scale"
    Require ([double]$internalTemperature[0].read.scale -eq 1.0) `
        "point 217 must use the integer register value; display precision is not a scale"
    Require ([string]$cabinetTemperature[0].desc -match "显示精度.*不作为Modbus倍率") `
        "point 189 must explain that display precision does not change the Modbus scale"
    Require ([string]$cabinetHumidity[0].desc -match "显示精度.*不作为Modbus倍率") `
        "point 190 must explain that display precision does not change the Modbus scale"
    Require ([string]$internalTemperature[0].desc -match "显示精度.*不作为Modbus倍率") `
        "point 217 must explain that display precision does not change the Modbus scale"
    Require ([string]$cabinetTemperature[0].name -eq "柜内温度（除湿机传感器）") `
        "point 189 must identify the dehumidifier sensor as the cabinet temperature source"
    Require ([string]$cabinetHumidity[0].name -eq "柜内湿度（除湿机传感器）") `
        "point 190 must identify the dehumidifier sensor as the cabinet humidity source"

    $liquidPoint = @($generated.meters | Where-Object meterCode -eq "COMM202600104_LIQUID_COOLING").points
    Require (@($liquidPoint | Where-Object index -eq 131).Count -eq 1) `
        "generating the dehumidifier must preserve the liquid-cooling meter"

    Write-Output "PASS 60W dehumidifier register scaling, precision semantics, and source labels"
} finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
