param(
    [Parameter(Mandatory = $true)]
    [string]$InputConfig,

    [Parameter(Mandatory = $true)]
    [string]$OutputConfig,

    [int]$SlaveAddress = 2,
    [string]$MeterCode = "COMM202600104_DEHUMIDIFIER",
    [switch]$OnlyDehumidifier,
    [switch]$ReadOnly
)

$ErrorActionPreference = "Stop"

function New-Point {
    param(
        [int]$Index,
        [string]$PointCode,
        [string]$Name,
        [string]$Description,
        [string]$Category,
        [int]$Address,
        [string]$DataType,
        [double]$Scale = 1,
        [double]$Offset = 0,
        [string]$Unit = "",
        [bool]$Signed = $false,
        [bool]$Store = $true,
        [bool]$ReportOnChange = $false,
        [int]$IntervalMs = 500,
        [hashtable]$ValueMap = $null
    )

    $point = [ordered]@{
        index = $Index
        legacyIndex = $Index
        pointCode = $PointCode
        name = $Name
        desc = $Description
        category = $Category
        address = $Address
        enabled = $true
        isStore = $Store
        fullUpload = $true
        reportOnChange = $ReportOnChange
        persistIntervalSec = 60
        tags = @("production", "mobile_ess", "dehumidifier", "dehumidifier_60w")
        read = [ordered]@{
            enable = $true
            function = 3
            length = 1
            dataType = $DataType
            scale = $Scale
            offset = $Offset
            byteOrder = "AB"
            signed = $Signed
            unit = $Unit
            intervalMs = $IntervalMs
        }
        write = [ordered]@{ enable = $false }
        alarms = @()
        collectPriority = 0
    }
    if ($null -ne $ValueMap) {
        $point.valueMap = $ValueMap
    }
    return $point
}

function Enable-RegisterWrite {
    param(
        [System.Collections.IDictionary]$Point,
        [double]$Minimum,
        [double]$Maximum,
        [double]$Step = 1,
        [double[]]$AllowedValues = @(),
        [bool]$VerifyAfterWrite = $true
    )

    $write = [ordered]@{
        enable = $true
        function = 16
        length = 1
        dataType = $Point.read.dataType
        scale = $Point.read.scale
        offset = $Point.read.offset
        byteOrder = "AB"
        min = $Minimum
        minValue = $Minimum
        max = $Maximum
        maxValue = $Maximum
        step = $Step
        verifyAfterWrite = $VerifyAfterWrite
        verifyDelayMs = 100
        verifyByRead = $VerifyAfterWrite
    }
    if ($AllowedValues.Count -gt 0) {
        $write.allowedValues = $AllowedValues
    }
    $Point.write = $write
}

if ($SlaveAddress -lt 1 -or $SlaveAddress -gt 247) {
    throw "SlaveAddress must be in the Modbus range 1..247."
}

$config = Get-Content -Raw -LiteralPath $InputConfig | ConvertFrom-Json
$targetMeters = @($config.meters | Where-Object { $_.meterCode -eq $MeterCode })
if ($targetMeters.Count -ne 1) {
    throw "Expected exactly one meter '$MeterCode', found $($targetMeters.Count)."
}
$meter = $targetMeters[0]

$points = [System.Collections.ArrayList]::new()

[void]$points.Add((New-Point -Index 188 -PointCode "mb5_s5_188" -Name "通讯状态" -Description "60W塑料型智能除湿装置在线状态" -Category "status" -Address 0 -DataType "device_online" -Store $false -ReportOnChange $true))
$points[0].read.function = 0
$points[0].read.scale = 1
$points[0].read.intervalMs = 500

[void]$points.Add((New-Point -Index 189 -PointCode "mb5_s5_189" -Name "柜内温度（除湿机传感器）" -Description "03H读寄存器0，寄存器返回整数工程值；0.1degC表示设备显示精度（分辨力），不作为Modbus倍率" -Category "telemetry" -Address 0 -DataType "int16" -Unit "degC" -Signed $true))
[void]$points.Add((New-Point -Index 190 -PointCode "mb5_s5_190" -Name "柜内湿度（除湿机传感器）" -Description "03H读寄存器1，寄存器返回整数工程值；0.1%RH表示设备显示精度（分辨力），不作为Modbus倍率" -Category "telemetry" -Address 1 -DataType "uint16" -Unit "%RH"))
[void]$points.Add((New-Point -Index 217 -PointCode "dehumidifier_60w_internal_temperature" -Name "除湿器内部温度" -Description "03H读寄存器4，寄存器返回整数工程值；0.1degC表示设备显示精度（分辨力），不作为Modbus倍率" -Category "telemetry" -Address 4 -DataType "int16" -Unit "degC" -Signed $true))
[void]$points.Add((New-Point -Index 218 -PointCode "dehumidifier_60w_sensor_status" -Name "温湿度状态原始值" -Description "03H读寄存器5；说明书未定义状态枚举，保留原始值并在变化时上报，禁止据此猜测内部故障" -Category "status" -Address 5 -DataType "uint16" -ReportOnChange $true))

$communicationAddress = New-Point -Index 219 -PointCode "dehumidifier_60w_communication_address" -Name "通讯地址" -Description "03H读/10H写寄存器7；修改后必须同步更新设备从站地址" -Category "setting" -Address 7 -DataType "uint16" -ReportOnChange $true
Enable-RegisterWrite -Point $communicationAddress -Minimum 1 -Maximum 247 -VerifyAfterWrite $false
[void]$points.Add($communicationAddress)

[void]$points.Add((New-Point -Index 228 -PointCode "dehumidifier_60w_communication_format" -Name "通讯格式原始值" -Description "03H读寄存器8；高字节为校验模式，低字节为波特率代号" -Category "setting" -Address 8 -DataType "uint16" -ReportOnChange $true))

$heaterStart = New-Point -Index 220 -PointCode "dehumidifier_60w_heater_start_temperature" -Name "加热器启动温度" -Description "03H读/10H写寄存器10，范围-10..25degC" -Category "setting" -Address 10 -DataType "int16" -Unit "degC" -Signed $true -ReportOnChange $true
Enable-RegisterWrite -Point $heaterStart -Minimum -10 -Maximum 25
[void]$points.Add($heaterStart)

$heaterHysteresis = New-Point -Index 221 -PointCode "dehumidifier_60w_heater_hysteresis" -Name "加热器回差温度" -Description "03H读/10H写寄存器11，范围1..10degC" -Category "setting" -Address 11 -DataType "uint16" -Unit "degC" -ReportOnChange $true
Enable-RegisterWrite -Point $heaterHysteresis -Minimum 1 -Maximum 10
[void]$points.Add($heaterHysteresis)

$dehumidifierStart = New-Point -Index 222 -PointCode "dehumidifier_60w_start_humidity" -Name "除湿器启动湿度" -Description "03H读/10H写寄存器12，范围40..99%RH" -Category "setting" -Address 12 -DataType "uint16" -Unit "%RH" -ReportOnChange $true
Enable-RegisterWrite -Point $dehumidifierStart -Minimum 40 -Maximum 99
[void]$points.Add($dehumidifierStart)

$dehumidifierHysteresis = New-Point -Index 223 -PointCode "dehumidifier_60w_humidity_hysteresis" -Name "除湿器回差湿度" -Description "03H读/10H写寄存器13，范围-20..-5%RH" -Category "setting" -Address 13 -DataType "int16" -Unit "%RH" -Signed $true -ReportOnChange $true
Enable-RegisterWrite -Point $dehumidifierHysteresis -Minimum -20 -Maximum -5
[void]$points.Add($dehumidifierHysteresis)

$fanStart = New-Point -Index 224 -PointCode "dehumidifier_60w_fan_start_temperature" -Name "排风启动温度" -Description "03H读/10H写寄存器14，范围30..50degC" -Category "setting" -Address 14 -DataType "uint16" -Unit "degC" -ReportOnChange $true
Enable-RegisterWrite -Point $fanStart -Minimum 30 -Maximum 50
[void]$points.Add($fanStart)

$fanHysteresis = New-Point -Index 225 -PointCode "dehumidifier_60w_fan_hysteresis" -Name "排风回差温度" -Description "03H读/10H写寄存器15，范围-10..-1degC" -Category "setting" -Address 15 -DataType "int16" -Unit "degC" -Signed $true -ReportOnChange $true
Enable-RegisterWrite -Point $fanHysteresis -Minimum -10 -Maximum -1
[void]$points.Add($fanHysteresis)

$manualAuto = New-Point -Index 226 -PointCode "dehumidifier_60w_manual_auto_state" -Name "手动/自动状态" -Description "03H读/10H写寄存器20；说明书未定义0/1对应关系，写入前需现场确认" -Category "setting" -Address 20 -DataType "uint16" -ReportOnChange $true
Enable-RegisterWrite -Point $manualAuto -Minimum 0 -Maximum 1 -AllowedValues @(0, 1)
[void]$points.Add($manualAuto)

$relayMode = New-Point -Index 227 -PointCode "dehumidifier_60w_relay2_output_mode" -Name "继电器2输出模式" -Description "03H读/10H写寄存器21；0=报警模式，1=风机模式" -Category "setting" -Address 21 -DataType "uint16" -ReportOnChange $true -ValueMap @{ "0" = "报警模式"; "1" = "风机模式" }
Enable-RegisterWrite -Point $relayMode -Minimum 0 -Maximum 1 -AllowedValues @(0, 1)
[void]$points.Add($relayMode)

if ($ReadOnly) {
    foreach ($point in $points) {
        $point.write = [ordered]@{ enable = $false }
    }
}

$meter.deviceName = "60W塑料型智能除湿装置"
$meter.enabled = $true
$meter.slave = $SlaveAddress
$meter.protocolType = "modbus_rtu"
$meter.sourceCom = "COM5"
$meter.sourceAddress = $SlaveAddress
$meter.points = @($points)

if ($OnlyDehumidifier) {
    $config.meters = @($meter)
}

$allIndexes = @{}
foreach ($item in @($config.meters)) {
    foreach ($point in @($item.points)) {
        $index = [int]$point.index
        if ($allIndexes.ContainsKey($index)) {
            throw "Duplicate global point index $index in meters '$($allIndexes[$index])' and '$($item.meterCode)'."
        }
        $allIndexes[$index] = $item.meterCode
    }
}

$outputDirectory = Split-Path -Parent $OutputConfig
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$config | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $OutputConfig -Encoding utf8

Write-Output "generated=$OutputConfig"
Write-Output "meterCode=$MeterCode slave=$SlaveAddress points=$($points.Count)"
Write-Output "readOnly=$($ReadOnly.IsPresent) onlyDehumidifier=$($OnlyDehumidifier.IsPresent)"
Write-Output "preservedIndexes=188,189,190 removedLegacyIndexes=196,199,209,210,211"
