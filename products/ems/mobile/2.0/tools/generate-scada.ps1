[CmdletBinding()]
param(
    [string]$SourceProjectDirectory,
    [string]$RuntimeConfigDirectory,
    [string]$OutputProjectDirectory,
    [string]$OutputPackage,
    [string]$MachineCode = "COMM202600999",
    [string]$DisplayName = "凯源移动储能车",
    [string]$PackageVersion = "2.0.9-compact",
    [string]$NodeId = "edge-1",
    [string]$LocalOperatorUsername = "operator",
    [Security.SecureString]$LocalOperatorPassword,
    [ValidateRange(60, 86400)]
    [int]$LocalSessionTimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\..\..\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SourceProjectDirectory)) {
    throw "SourceProjectDirectory is required; use the project pulled from the target edge or an approved baseline"
}
if ([string]::IsNullOrWhiteSpace($LocalOperatorUsername)) {
    throw "LocalOperatorUsername is required"
}
if ($PackageVersion -notmatch '^2\.0\.[0-9]+(?:[-._][A-Za-z0-9._-]+)?$') {
    throw "EMS 2.0 packageVersion must stay in the 2.0.x product line: $PackageVersion"
}
if ($null -eq $LocalOperatorPassword -or $LocalOperatorPassword.Length -eq 0) {
    throw "LocalOperatorPassword is required; pass a SecureString so protected pages are never generated without credentials"
}
if ([string]::IsNullOrWhiteSpace($OutputProjectDirectory)) {
    $OutputProjectDirectory = Join-Path $repoRoot "generated\artifacts\releases\storage-ems-2.0-compact\scada-project"
}
if ([string]::IsNullOrWhiteSpace($OutputPackage)) {
    $OutputPackage = Join-Path $repoRoot "generated\artifacts\releases\storage-ems-2.0-compact\storage-ems-2.0.9-compact.kyscada"
}

$sourceRoot = (Resolve-Path -LiteralPath $SourceProjectDirectory).Path
$outputRoot = [System.IO.Path]::GetFullPath($OutputProjectDirectory)
if ($sourceRoot -eq $outputRoot) {
    throw "source and output SCADA project directories must be different"
}

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-Json([string]$Path, $Value) {
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    ConvertTo-Json -InputObject $Value -Depth 40 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Convert-BytesToHex([byte[]]$Bytes) {
    return [BitConverter]::ToString($Bytes).Replace('-', '').ToLowerInvariant()
}

function Get-RelativePathCompat([string]$Root, [string]$Path) {
    $relativePathMethod = [System.IO.Path].GetMethod(
        "GetRelativePath",
        [type[]]@([string], [string]))
    if ($null -ne $relativePathMethod) {
        return [System.IO.Path]::GetRelativePath($Root, $Path)
    }

    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $pathValue = [System.IO.Path]::GetFullPath($Path)
    $rootUri = [System.Uri]::new($rootPath)
    $pathUri = [System.Uri]::new($pathValue)
    return [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString()).Replace(
        '/',
        [System.IO.Path]::DirectorySeparatorChar)
}

function New-LocalPasswordRecord(
    [string]$Username,
    [Security.SecureString]$Password
) {
    $saltBytes = [byte[]]::new(16)
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($saltBytes)
    } finally {
        $random.Dispose()
    }
    $salt = Convert-BytesToHex $saltBytes
    $passwordPointer = [IntPtr]::Zero
    $plainPassword = $null
    try {
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
        $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
        $payload = [Text.Encoding]::UTF8.GetBytes("$salt`:$plainPassword")
        try {
            $sha256 = [Security.Cryptography.SHA256]::Create()
            try {
                $digest = $sha256.ComputeHash($payload)
                $passwordHash = Convert-BytesToHex $digest
            } finally {
                $sha256.Dispose()
            }
        } finally {
            [Array]::Clear($payload, 0, $payload.Length)
        }
    } finally {
        $plainPassword = $null
        if ($passwordPointer -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer)
        }
    }
    return [pscustomobject][ordered]@{
        username = $Username
        salt = $salt
        passwordSha256 = $passwordHash
        roles = @("operator")
    }
}

$tagsDocument = Read-Json (Join-Path $sourceRoot "tags.json")
$runtimeDocument = Read-Json (Join-Path $sourceRoot "runtime-map.json")
# Windows PowerShell 5.1 emits a root JSON array as one Object[] pipeline item.
$tags = @($tagsDocument | ForEach-Object { $_ })
$runtimeMappings = @($runtimeDocument | ForEach-Object { $_ })
if ($tags.Count -eq 0 -or $tags.Count -ne $runtimeMappings.Count) {
    throw "authoritative SCADA tags/routes are missing or inconsistent"
}

$systemMonitorSharedMemoryName = "gateway_point_store_system_monitor"
$systemMonitorPoints = @(
    @{ Index=920000001; Code="cellular_enabled"; Name="4G 监测启用"; Unit=""; Role="system.cellular.enabled" },
    @{ Index=920000002; Code="cellular_present"; Name="4G 模块存在"; Unit=""; Role="system.cellular.present" },
    @{ Index=920000003; Code="cellular_connected"; Name="4G 网络连接"; Unit=""; Role="system.cellular.connected" },
    @{ Index=920000004; Code="cellular_using_route"; Name="当前使用 4G 出口"; Unit=""; Role="system.cellular.routeActive" },
    @{ Index=920000005; Code="cellular_signal_percent"; Name="4G 信号强度"; Unit="%"; Role="system.cellular.signalPercent" },
    @{ Index=920000006; Code="cellular_rx_total_mib"; Name="4G 本次连接下载流量"; Unit="MiB"; Role="system.cellular.rxTotalMiB" },
    @{ Index=920000007; Code="cellular_tx_total_mib"; Name="4G 本次连接上传流量"; Unit="MiB"; Role="system.cellular.txTotalMiB" },
    @{ Index=920000008; Code="cellular_rx_rate_kibps"; Name="4G 实时下行速率"; Unit="KiB/s"; Role="system.cellular.rxRateKiBps" },
    @{ Index=920000009; Code="cellular_tx_rate_kibps"; Name="4G 实时上行速率"; Unit="KiB/s"; Role="system.cellular.txRateKiBps" }
)
$existingTagIds = @{}
$existingMappingIndexes = @{}
foreach ($tag in $tags) { $existingTagIds[[string]$tag.tagId] = $true }
foreach ($mapping in $runtimeMappings) { $existingMappingIndexes[[uint32]$mapping.index] = [string]$mapping.tagId }
foreach ($point in $systemMonitorPoints) {
    $tagId = "SYSTEM_CELLULAR.$($point.Code)"
    $index = [uint32]$point.Index
    if ($existingTagIds.ContainsKey($tagId)) { continue }
    if ($existingMappingIndexes.ContainsKey($index)) {
        throw "system monitor index is already used: $index/$($existingMappingIndexes[$index])"
    }
    $tags += [pscustomobject][ordered]@{
        tagId = $tagId
        nodeId = $NodeId
        deviceId = "SYSTEM_CELLULAR"
        meterCode = "SYSTEM_CELLULAR"
        pointCode = [string]$point.Code
        semanticRole = [string]$point.Role
        displayName = [string]$point.Name
        unit = [string]$point.Unit
        dataType = "float64"
        access = "read"
        indexFallback = $index
    }
    $runtimeMappings += [pscustomobject][ordered]@{
        nodeId = $NodeId
        tagId = $tagId
        sharedMemoryName = $systemMonitorSharedMemoryName
        index = $index
        writable = $false
        dataType = "float64"
        unit = [string]$point.Unit
    }
    $existingTagIds[$tagId] = $true
    $existingMappingIndexes[$index] = $tagId
}

$tagById = @{}
foreach ($tag in $tags) { $tagById[[string]$tag.tagId] = $tag }
$mappingByIndex = @{}
$mappingByTagId = @{}
foreach ($mapping in $runtimeMappings) {
    $index = [uint32]$mapping.index
    if ($mappingByIndex.ContainsKey($index)) { throw "duplicate authoritative SCADA index: $index" }
    if (-not $tagById.ContainsKey([string]$mapping.tagId)) {
        throw "runtime route references unknown tag: $($mapping.tagId)"
    }
    $mappingByIndex[$index] = $mapping
    $mappingByTagId[[string]$mapping.tagId] = $mapping
}

$runtimePointByIndex = @{}
if (-not [string]::IsNullOrWhiteSpace($RuntimeConfigDirectory)) {
    $runtimeRoot = (Resolve-Path -LiteralPath $RuntimeConfigDirectory).Path
    $runtimeDevicesDirectory = Join-Path $runtimeRoot "devices"
    if (-not (Test-Path -LiteralPath $runtimeDevicesDirectory -PathType Container)) {
        throw "runtime config directory does not contain devices: $runtimeRoot"
    }

    foreach ($configFile in Get-ChildItem -LiteralPath $runtimeDevicesDirectory -Filter "*.json" -File) {
        $config = Read-Json $configFile.FullName
        if ($null -eq $config.PSObject.Properties["meters"]) { continue }
        foreach ($meter in @($config.meters)) {
            if ($null -eq $meter -or $null -eq $meter.PSObject.Properties["points"]) { continue }
            foreach ($point in @($meter.points)) {
                if ($null -eq $point -or $null -eq $point.PSObject.Properties["index"]) { continue }
                $index = [uint32]$point.index
                if ($runtimePointByIndex.ContainsKey($index)) {
                    throw "duplicate runtime point index: $index ($($configFile.Name))"
                }
                $runtimePointByIndex[$index] = [pscustomobject][ordered]@{
                    point = $point
                    meterCode = if ($null -ne $meter.PSObject.Properties["meterCode"]) { [string]$meter.meterCode } else { "" }
                    sourceFile = $configFile.FullName
                }
            }
        }
    }
}

function Has-Index([uint32]$Index) {
    return $script:mappingByIndex.ContainsKey($Index)
}

function Point-Text([uint32]$Index, [string]$PropertyName) {
    if ($script:runtimePointByIndex.ContainsKey($Index)) {
        $runtimePoint = $script:runtimePointByIndex[$Index].point
        $property = $runtimePoint.PSObject.Properties[$PropertyName]
        if ($null -ne $property -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            return [string]$property.Value
        }
    }
    if (-not (Has-Index $Index)) { return "" }
    $tag = $script:tagById[[string]$script:mappingByIndex[$Index].tagId]
    $tagPropertyName = switch ($PropertyName) {
        "name" { "displayName" }
        default { $PropertyName }
    }
    $tagProperty = $tag.PSObject.Properties[$tagPropertyName]
    if ($null -ne $tagProperty) { return [string]$tagProperty.Value }
    return ""
}

function Find-PointIndex(
    [string[]]$Names = @(),
    [string[]]$PointCodes = @(),
    [string]$SharedMemoryPattern = "",
    [switch]$RequireWritable
) {
    $candidateIndexes = [System.Collections.Generic.List[uint32]]::new()
    foreach ($indexValue in @($script:mappingByIndex.Keys | Sort-Object)) {
        $index = [uint32]$indexValue
        $mapping = $script:mappingByIndex[$index]
        if (-not [string]::IsNullOrWhiteSpace($SharedMemoryPattern) -and
            [string]$mapping.sharedMemoryName -notmatch $SharedMemoryPattern) {
            continue
        }
        if ($RequireWritable -and -not [bool]$mapping.writable) { continue }

        $name = Point-Text $index "name"
        $pointCode = Point-Text $index "pointCode"
        if (($Names.Count -gt 0 -and $Names -contains $name) -or
            ($PointCodes.Count -gt 0 -and $PointCodes -contains $pointCode)) {
            $candidateIndexes.Add($index)
        }
    }
    if ($candidateIndexes.Count -gt 1) {
        throw "ambiguous semantic point: names=$($Names -join ',') pointCodes=$($PointCodes -join ',') indexes=$($candidateIndexes -join ',')"
    }
    if ($candidateIndexes.Count -eq 1) { return [uint32]$candidateIndexes[0] }
    return $null
}

function Normal-ValueFor([uint32]$Index, [string]$DefaultValue = "0") {
    $name = Point-Text $Index "name"
    if ($name -match '1\s*(正常|=正常)') { return "1" }
    if ($name -match '按下闭点') { return "0" }
    if ($name -match '1\s*(开门|动作|报警)') { return "0" }
    if ($name -match '(关闭|合位)') { return "1" }
    return $DefaultValue
}

$dioMemoryPattern = '(?i)(^|_)dio($|_)'
$dioPoints = [ordered]@{
    fan = Find-PointIndex -Names @("风机继电器") -SharedMemoryPattern $dioMemoryPattern -RequireWritable
    runLamp = Find-PointIndex -Names @("运行指示灯") -SharedMemoryPattern $dioMemoryPattern -RequireWritable
    chargeLamp = Find-PointIndex -Names @("充电指示灯") -SharedMemoryPattern $dioMemoryPattern -RequireWritable
    dischargeLamp = Find-PointIndex -Names @("放电指示灯") -SharedMemoryPattern $dioMemoryPattern -RequireWritable
    emergency = Find-PointIndex -Names @("急停（按下闭点）", "急停反馈-1正常", "急停反馈") -SharedMemoryPattern $dioMemoryPattern
    surge = Find-PointIndex -Names @("浪涌保护器动作", "浪涌保护器-1动作") -SharedMemoryPattern $dioMemoryPattern
    water = Find-PointIndex -Names @('水浸（设备文档原文为“税金”）', "水浸", "税金") -SharedMemoryPattern $dioMemoryPattern
    fireAction = Find-PointIndex -Names @("消防动作", "气溶胶-1动作", "气溶胶") -SharedMemoryPattern $dioMemoryPattern
    frontDoor = Find-PointIndex -Names @("前门关闭", "前门反馈-1开门") -SharedMemoryPattern $dioMemoryPattern
    rearDoor = Find-PointIndex -Names @("后门关闭", "后门反馈-1开门") -SharedMemoryPattern $dioMemoryPattern
    storageBreaker = Find-PointIndex -Names @("储能断路器合位") -SharedMemoryPattern $dioMemoryPattern
    gridBreaker = Find-PointIndex -Names @("并网断路器合位", "并网断路器-1合位") -SharedMemoryPattern $dioMemoryPattern
    loadBreaker = Find-PointIndex -Names @("负荷断路器合位") -SharedMemoryPattern $dioMemoryPattern
}

$dehumidifierPoints = [ordered]@{
    cabinetTemperature = Find-PointIndex -PointCodes @("mb5_s5_189")
    cabinetHumidity = Find-PointIndex -PointCodes @("mb5_s5_190")
    workState = Find-PointIndex -Names @("除湿器工作状态")
    fault = Find-PointIndex -Names @("除湿故障码", "除湿器故障码")
    internalTemperature = Find-PointIndex -Names @("除湿器内部温度")
    sensorStatus = Find-PointIndex -Names @("温湿度状态原始值")
    manualAuto = Find-PointIndex -Names @("手动/自动状态")
    relayMode = Find-PointIndex -Names @("继电器2输出模式")
}

function Tag-At([uint32]$Index) {
    if (-not $script:mappingByIndex.ContainsKey($Index)) {
        throw "required EMS 2.0 point is missing: index=$Index"
    }
    return $script:tagById[[string]$script:mappingByIndex[$Index].tagId]
}

function Set-TagMetadata([uint32]$Index, [string]$DisplayName, [string]$Unit) {
    $tag = Tag-At $Index
    $tag.displayName = $DisplayName
    $tag.unit = $Unit
}

foreach ($metadata in @(
    @{ Index=$dehumidifierPoints.cabinetTemperature; Name="柜内温度（除湿机传感器）"; Unit="℃" },
    @{ Index=$dehumidifierPoints.cabinetHumidity; Name="柜内湿度（除湿机传感器）"; Unit="%RH" },
    @{ Index=$dehumidifierPoints.internalTemperature; Name="除湿机内部温度"; Unit="℃" }
)) {
    if ($null -ne $metadata.Index -and [uint32]$metadata.Index -ne 0) {
        Set-TagMetadata ([uint32]$metadata.Index) $metadata.Name $metadata.Unit
    }
}

function New-Geometry([double]$X, [double]$Y, [double]$Width, [double]$Height) {
    return [pscustomobject][ordered]@{ x = $X; y = $Y; width = $Width; height = $Height }
}

function New-Binding([uint32]$Index, [string]$Slot = "value") {
    $tag = Tag-At $Index
    return [pscustomobject][ordered]@{ nodeId = $NodeId; tagId = [string]$tag.tagId; slot = $Slot }
}

function New-Action(
    [string]$Type = "none",
    [string]$TargetScreen = "",
    [uint32]$Index = 0,
    [string]$Value = "",
    [bool]$RequiresConfirmation = $true,
    [bool]$HighPriority = $false
) {
    $tagId = ""
    if ($Index -gt 0) { $tagId = [string](Tag-At $Index).tagId }
    return [pscustomobject][ordered]@{
        type = $Type
        targetScreen = $TargetScreen
        nodeId = $NodeId
        tagId = $tagId
        value = $Value
        requiresConfirmation = $RequiresConfirmation
        highPriority = $HighPriority
    }
}

function New-Widget(
    [string]$Id,
    [string]$Type,
    [string]$Title,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [int]$ZIndex = 1,
    [object[]]$Bindings = @(),
    [object[]]$StateRules = @(),
    $Action = $null,
    $Properties = $null
) {
    if ($null -eq $Properties) { $Properties = [ordered]@{} }
    return [pscustomobject][ordered]@{
        widgetId = $Id
        type = $Type
        title = $Title
        geometry = New-Geometry $X $Y $Width $Height
        zIndex = $ZIndex
        visible = $true
        styleClass = "ems2"
        bindings = @($Bindings)
        stateRules = @($StateRules)
        action = $Action
        properties = [pscustomobject]$Properties
    }
}

function New-Frame([string]$Id, [double]$X, [double]$Y, [double]$Width, [double]$Height, [string]$Color, [int]$ZIndex = 0) {
    return New-Widget $Id "qtFrame" "" $X $Y $Width $Height $ZIndex @() @() $null ([ordered]@{
        qtText = ""
        qtBackgroundColor = $Color
        qtTransparent = $false
    })
}

function New-Text(
    [string]$Id,
    [string]$Text,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [int]$FontSize = 16,
    [string]$Color = "#EAF6FA",
    [string]$Alignment = "Left",
    [bool]$Bold = $false,
    [int]$ZIndex = 5
) {
    return New-Widget $Id "qtLabel" $Text $X $Y $Width $Height $ZIndex @() @() $null ([ordered]@{
        qtText = $Text
        qtTextColor = $Color
        qtBackgroundColor = "transparent"
        qtFontSize = $FontSize
        qtFontWeight = if ($Bold) { "Bold" } else { "Normal" }
        qtTextAlignment = $Alignment
        qtTransparent = $true
    })
}

function New-BoundText(
    [string]$Id,
    [uint32]$Index,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [int]$FontSize = 20,
    [string]$Color = "#F3F8FA",
    [string]$Alignment = "Left",
    [string]$ValueMapJson = ""
) {
    return New-Widget $Id "qtValue" "" $X $Y $Width $Height 6 @((New-Binding $Index)) @() $null ([ordered]@{
        qtText = "--"
        qtTextColor = $Color
        qtBackgroundColor = "transparent"
        qtFontSize = $FontSize
        qtFontWeight = "Bold"
        qtTextAlignment = $Alignment
        qtTransparent = $true
        valueMapJson = $ValueMapJson
    })
}

function New-ValueCard(
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$ValueMapJson = ""
) {
    return New-Widget $Id "value" $Title $X $Y $Width $Height 3 @((New-Binding $Index)) @() $null ([ordered]@{
        qtBackgroundColor = "#0B2633"
        qtTextColor = "#F3F8FA"
        qtFontSize = 21
        qtFontWeight = "Bold"
        qtTextAlignment = "Left"
        valueMapJson = $ValueMapJson
    })
}

function New-StateRule(
    [string]$Code,
    [string]$Label,
    [string]$Color,
    [uint32]$Index,
    [string]$Comparison,
    [string]$Value,
    [int]$Priority
) {
    return [pscustomobject][ordered]@{
        code = $Code
        label = $Label
        color = $Color
        image = ""
        priority = $Priority
        match = "all"
        conditions = @([pscustomobject][ordered]@{
            nodeId = $NodeId
            tagId = [string](Tag-At $Index).tagId
            comparison = $Comparison
            value = $Value
        })
    }
}

function New-BinaryStatus(
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [string]$NormalValue,
    [string]$NormalLabel,
    [string]$AbnormalLabel,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$AbnormalColor = "#E45858"
) {
    $rules = @(
        (New-StateRule "normal" $NormalLabel "#20C879" $Index "eq" $NormalValue 20),
        (New-StateRule "abnormal" $AbnormalLabel $AbnormalColor $Index "ne" $NormalValue 10)
    )
    return New-Widget $Id "statusLamp" $Title $X $Y $Width $Height 3 @((New-Binding $Index)) $rules $null ([ordered]@{
        qtBackgroundColor = "#0B2633"
        qtTextColor = "#F3F8FA"
        qtFontSize = 16
        defaultStateLabel = "未知"
        defaultStateColor = "#71808A"
        stateColorMode = "indicator"
    })
}

function New-MultiStatus(
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [object[]]$States,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    $rules = [System.Collections.Generic.List[object]]::new()
    $priority = 100
    foreach ($state in $States) {
        $rules.Add((New-StateRule ([string]$state.Code) ([string]$state.Label) ([string]$state.Color) $Index "eq" ([string]$state.Value) $priority))
        $priority--
    }
    return New-Widget $Id "statusLamp" $Title $X $Y $Width $Height 3 @((New-Binding $Index)) @($rules) $null ([ordered]@{
        qtBackgroundColor = "#0B2633"
        qtTextColor = "#F3F8FA"
        qtFontSize = 16
        defaultStateLabel = "其他状态"
        defaultStateColor = "#D9A441"
        stateColorMode = "indicator"
    })
}

function New-ProgressCard(
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [int]$FontSize = 26
) {
    return New-Widget $Id "batterySoc" $Title $X $Y $Width $Height 3 @((New-Binding $Index)) @() $null ([ordered]@{
        qtBackgroundColor = "#0B2633"
        qtTextColor = "#F3F8FA"
        qtFontSize = $FontSize
        qtFontWeight = "Bold"
        progressMaxValue = 100
        progressOrientation = "horizontal"
    })
}

function New-CellularSignalCard(
    [string]$Id,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    return New-Widget $Id "cellularSignal" "4G 信号" $X $Y $Width $Height 3 @((New-Binding 920000005)) @() $null ([ordered]@{
        qtBackgroundColor = "#0B2633"
        qtTextColor = "#F3F8FA"
        qtFontSize = 29
        qtFontWeight = "Bold"
        valueSuffix = "%"
    })
}

function New-CellularLinkStatus(
    [string]$Id,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    $rules = @(
        [pscustomobject][ordered]@{
            code="disabled";label="监测已关闭";color="#71808A";image="";priority=100;match="all"
            conditions=@([pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000001).tagId;comparison="eq";value="0"})
        },
        [pscustomobject][ordered]@{
            code="missing";label="未检测到模块";color="#E45858";image="";priority=90;match="all"
            conditions=@(
                [pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000001).tagId;comparison="eq";value="1"},
                [pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000002).tagId;comparison="eq";value="0"}
            )
        },
        [pscustomobject][ordered]@{
            code="offline";label="未联网";color="#D9A441";image="";priority=80;match="all"
            conditions=@(
                [pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000002).tagId;comparison="eq";value="1"},
                [pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000003).tagId;comparison="eq";value="0"}
            )
        },
        [pscustomobject][ordered]@{
            code="online";label="已连接";color="#20C879";image="";priority=70;match="all"
            conditions=@([pscustomobject][ordered]@{nodeId=$NodeId;tagId=[string](Tag-At 920000003).tagId;comparison="eq";value="1"})
        }
    )
    return New-Widget $Id "statusLamp" "" $X $Y $Width $Height 3 @(
        (New-Binding 920000001),(New-Binding 920000002),(New-Binding 920000003)
    ) $rules $null ([ordered]@{
        qtBackgroundColor="transparent";qtTextColor="#F3F8FA";qtFontSize=23
        defaultStateLabel="状态未知";defaultStateColor="#71808A";stateColorMode="indicator"
    })
}

function New-EnergyFlow(
    [string]$Id,
    [uint32]$PowerIndex,
    [string]$ForwardWhen,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [double]$RatedPower = 30.0
) {
    if ($ForwardWhen -notin @("positive", "negative")) {
        throw "energy flow ForwardWhen must be positive or negative: $Id"
    }
    return New-Widget $Id "energyFlow" "" $X $Y $Width $Height 5 @((New-Binding $PowerIndex "power")) @() $null ([ordered]@{
        flowForwardWhen = $ForwardWhen
        flowDeadband = 0.2
        flowRatedPower = $RatedPower
        flowParticleCount = 4
        flowAnimationIntervalMs = 80
        flowColor = "#20DBE9"
        flowIdleColor = "#2C6078"
    })
}

function Add-ParallelStrategyCard(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [uint32]$StatusIndex,
    [string]$PriorityLabel,
    [uint32]$OutputIndex,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$Accent,
    [object[]]$States = @()
) {
    if (-not (Has-Index $StatusIndex)) { return }

    $prefix = "overview-strategy-parallel-$Id"
    $Widgets.Add((New-Frame "$prefix-frame" $X $Y $Width $Height "#0B2633" 1))
    $Widgets.Add((New-Frame "$prefix-accent" $X $Y 3 $Height $Accent 2))
    $Widgets.Add((New-Text "$prefix-title" $Title ($X + 9) ($Y + 3) 110 18 12 "#EAF6FA" "Left" $true 5))
    $Widgets.Add((New-Text "$prefix-priority" $PriorityLabel ($X + 9) ($Y + 22) 62 17 10 $Accent "Left" $true 5))

    if ($OutputIndex -gt 0 -and (Has-Index $OutputIndex)) {
        $Widgets.Add((New-BoundText "$prefix-value" $OutputIndex ($X + 72) ($Y + 21) 48 18 12 "#F3F8FA" "Right"))
    }

    $statusX = $X + $Width - 98
    $status = if ($States.Count -gt 0) {
        New-MultiStatus $prefix "" $StatusIndex $States $statusX ($Y + 2) 94 ($Height - 4)
    } else {
        New-BinaryStatus $prefix "" $StatusIndex "1" "运行" "待机" $statusX ($Y + 2) 94 ($Height - 4) "#71808A"
    }
    $status.properties.qtBackgroundColor = "#0B2633"
    $status.properties.qtFontSize = 12
    $Widgets.Add($status)
}

function Add-StrategyOutcomeStage(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [object[]]$Rows,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$Accent
) {
    $Widgets.Add((New-Frame $Id $X $Y $Width $Height "#0B2633" 1))
    $Widgets.Add((New-Frame "$Id-accent" $X $Y $Width 3 $Accent 2))
    $Widgets.Add((New-Text "$Id-title" $Title ($X + 5) ($Y + 8) ($Width - 10) 20 12 $Accent "Center" $true 5))
    $rowY = $Y + 34
    foreach ($row in $Rows) {
        $rowId = [string]$row.Id
        $Widgets.Add((New-Text "$Id-label-$rowId" ([string]$row.Label) ($X + 6) $rowY 42 18 10 "#8FB2BF" "Left" $false 5))
        if ($null -ne $row.PSObject.Properties["Index"] -and
            [uint32]$row.Index -gt 0 -and
            (Has-Index ([uint32]$row.Index))) {
            $valueMapJson = if ($null -ne $row.PSObject.Properties["ValueMapJson"]) { [string]$row.ValueMapJson } else { "" }
            $Widgets.Add((New-BoundText "$Id-value-$rowId" ([uint32]$row.Index) ($X + 44) $rowY ($Width - 50) 18 11 "#F3F8FA" "Right" $valueMapJson))
        } elseif ($null -ne $row.PSObject.Properties["Value"]) {
            $Widgets.Add((New-Text "$Id-value-$rowId" ([string]$row.Value) ($X + 44) $rowY ($Width - 50) 18 10 "#F3F8FA" "Right" $true 5))
        }
        $rowY += 24
    }
}

function Add-StrategyMetric(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Label,
    [uint32]$Index,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [string]$ValueColor = "#F3F8FA",
    [string]$ValueMapJson = ""
) {
    if (-not (Has-Index $Index)) { return }
    $Widgets.Add((New-Text "$Id-label" $Label $X $Y ($Width - 94) 20 12 "#8FB2BF" "Left" $false 5))
    $Widgets.Add((New-BoundText "$Id-value" $Index ($X + $Width - 92) $Y 92 20 13 $ValueColor "Right" $ValueMapJson))
}

function New-NavigationButton(
    [string]$Id,
    [string]$Text,
    [string]$Target,
    [bool]$Active,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    return New-Widget $Id "qtButton" $Text $X $Y $Width $Height 20 @() @() (New-Action "navigate" $Target 0 "" $false $false) ([ordered]@{
        qtText = $Text
        qtTextColor = if ($Active) { "#FFFFFF" } else { "#A9C0CA" }
        qtBackgroundColor = if ($Active) { "#14576B" } else { "transparent" }
        qtFontSize = 16
        qtTransparent = -not $Active
    })
}

function New-ControlButton(
    [string]$Id,
    [string]$Text,
    [uint32]$Index,
    [string]$Type,
    [string]$Value,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [bool]$HighPriority = $false,
    [string]$Background = "#113746"
) {
    $mapping = $script:mappingByIndex[$Index]
    $tag = Tag-At $Index
    if (-not [bool]$mapping.writable -or [string]$tag.access -eq "read") {
        throw "control button targets read-only point: index=$Index"
    }
    return New-Widget $Id "qtButton" $Text $X $Y $Width $Height 10 @() @() (New-Action $Type "" $Index $Value $true $HighPriority) ([ordered]@{
        qtText = $Text
        qtTextColor = "#F3F8FA"
        qtBackgroundColor = $Background
        qtFontSize = 16
        qtTransparent = $false
    })
}

function New-Chart(
    [string]$Id,
    [string]$Title,
    [uint32[]]$Indexes,
    [string[]]$Names,
    [string[]]$Units,
    [string[]]$Colors,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [object[]]$SeriesOptions = @(),
    [int]$DefaultWindowMinutes = 60,
    [int]$SampleIntervalSeconds = 1,
    [int]$RenderIntervalMs = 1000
) {
    $bindings = [System.Collections.Generic.List[object]]::new()
    $series = [System.Collections.Generic.List[object]]::new()
    $boundIndexes = [System.Collections.Generic.HashSet[uint32]]::new()
    for ($i = 0; $i -lt $Indexes.Count; $i++) {
        $option = if ($i -lt @($SeriesOptions).Count) { $SeriesOptions[$i] } else { $null }
        $scale = 1.0
        $offset = 0.0
        $penStyle = "solid"
        $aggregation = "first"
        [uint32[]]$sourceIndexes = @($Indexes[$i])
        if ($null -ne $option) {
            if ($null -ne $option.PSObject.Properties["Scale"]) { $scale = [double]$option.Scale }
            if ($null -ne $option.PSObject.Properties["Offset"]) { $offset = [double]$option.Offset }
            if ($null -ne $option.PSObject.Properties["PenStyle"] -and
                -not [string]::IsNullOrWhiteSpace([string]$option.PenStyle)) {
                $penStyle = [string]$option.PenStyle
            }
            if ($null -ne $option.PSObject.Properties["Indexes"] -and @($option.Indexes).Count -gt 0) {
                [uint32[]]$sourceIndexes = @($option.Indexes | ForEach-Object { [uint32]$_ })
            }
            if ($null -ne $option.PSObject.Properties["Aggregation"] -and
                -not [string]::IsNullOrWhiteSpace([string]$option.Aggregation)) {
                $aggregation = [string]$option.Aggregation
            }
        }
        foreach ($sourceIndex in $sourceIndexes) {
            if ($boundIndexes.Add([uint32]$sourceIndex)) {
                $bindings.Add((New-Binding ([uint32]$sourceIndex)))
            }
        }
        $series.Add([pscustomobject][ordered]@{
            index = $Indexes[$i]
            indexes = @($sourceIndexes)
            aggregation = $aggregation
            name = $Names[$i]
            unit = $Units[$i]
            color = $Colors[$i]
            penStyle = $penStyle
            scale = $scale
            offset = $offset
            lineWidth = if ($null -ne $option -and $null -ne $option.PSObject.Properties["LineWidth"]) {
                [double]$option.LineWidth
            } else { 2.0 }
            renderLayer = if ($null -ne $option -and $null -ne $option.PSObject.Properties["RenderLayer"]) {
                [double]$option.RenderLayer
            } else { 0.0 }
        })
    }
    return New-Widget $Id "qtChart" $Title $X $Y $Width $Height 4 @($bindings) @() $null ([ordered]@{
        qtText = ""
        qtBackgroundColor = "transparent"
        chartSampleIntervalSeconds = $SampleIntervalSeconds
        chartRenderIntervalMs = $RenderIntervalMs
        chartDefaultWindowMinutes = $DefaultWindowMinutes
        chartMinWindowMinutes = 5
        chartMaxPoints = 86402
        chartSeriesJson = (@($series) | ConvertTo-Json -Depth 8 -Compress)
    })
}

function Add-HeaderAndFooter([System.Collections.Generic.List[object]]$Widgets, [string]$ActiveScreen) {
    $Widgets.Add((New-Frame "chrome-header" 0 0 1920 112 "#071B26" -10))
    $Widgets.Add((New-Frame "chrome-brand" 30 34 38 38 "#D9F4F7" 1))
    $Widgets.Add((New-Text "chrome-brand-text" "KY" 37 42 28 24 14 "#09202B" "Center" $true 3))
    $Widgets.Add((New-Text "chrome-title" $DisplayName 82 25 390 34 21 "#F3F8FA" "Left" $true 3))
    $Widgets.Add((New-Text "chrome-subtitle" "$MachineCode · 现场监控中心" 82 62 390 26 14 "#74B9D0" "Left" $false 3))

    $pages = @(
        @{ Id = "Overview"; Text = "总览" },
        @{ Id = "Strategy"; Text = "策略执行" },
        @{ Id = "Devices"; Text = "设备" },
        @{ Id = "Alarms"; Text = "告警" },
        @{ Id = "Trends"; Text = "趋势" },
        @{ Id = "Control"; Text = "控制" },
        @{ Id = "Maintenance"; Text = "运维" }
    )
    $x = 623
    foreach ($page in $pages) {
        $Widgets.Add((New-NavigationButton "nav-$($page.Id)" $page.Text $page.Id ($page.Id -eq $ActiveScreen) $x 24 104 64))
        $x += 112
    }
    $Widgets.Add((New-Text "chrome-runtime" "● 边端本地运行" 1510 37 360 28 15 "#55E0AA" "Right" $true 3))

    $kpis = @(
        @{ Label = "并网有功"; Index = 1039; Unit = "kW"; Color = "#55D8E8" },
        @{ Label = "SOC"; Index = 1569; Unit = "%"; Color = "#F3F8FA" },
        @{ Label = "今日充电"; Index = 1615; Unit = "kWh"; Color = "#55D8E8" },
        @{ Label = "今日放电"; Index = 1616; Unit = "kWh"; Color = "#F9CC44" },
        @{ Label = "可放电量"; Index = 1591; Unit = "kWh"; Color = "#55E0AA" }
    )
    for ($i = 0; $i -lt $kpis.Count; $i++) {
        $kx = $i * 384
        $frameColor = if ($i % 2 -eq 0) { "#091F2B" } else { "#0A2430" }
        $Widgets.Add((New-Frame "kpi-frame-$i" $kx 112 384 106 $frameColor -5))
        if ($i -gt 0) { $Widgets.Add((New-Frame "kpi-divider-$i" $kx 112 1 106 "#244957" 0)) }
        $Widgets.Add((New-Text "kpi-label-$i" $kpis[$i].Label ($kx + 20) 137 150 24 14 "#78B6CC" "Left" $false 2))
        $Widgets.Add((New-BoundText "kpi-value-$i" $kpis[$i].Index ($kx + 20) 166 245 36 23 $kpis[$i].Color "Left"))
        $Widgets.Add((New-Text "kpi-unit-$i" $kpis[$i].Unit ($kx + 270) 171 92 24 14 "#9CB4BE" "Right" $false 2))
    }

    $Widgets.Add((New-Frame "chrome-footer" 0 1038 1920 42 "#061720" -10))
    $Widgets.Add((New-Text "footer-source" "数据源：共享内存直读 · 页面刷新：500 ms" 24 1049 620 24 13 "#78B6CC" "Left" $false 3))
    $Widgets.Add((New-Text "footer-machine" $MachineCode 1540 1049 350 24 13 "#78B6CC" "Right" $false 3))
}

function New-Screen([string]$Id, [string]$Title, [System.Collections.Generic.List[object]]$Widgets) {
    return [pscustomobject][ordered]@{
        screenId = $Id
        title = "$DisplayName - $Title"
        width = 1920
        height = 1080
        background = ""
        widgets = @($Widgets)
    }
}

function Build-OverviewScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Overview"
    $widgets.Add((New-Frame "overview-status-panel" 24 240 382 786 "#081E29" 0))
    $widgets.Add((New-Text "overview-status-title" "设备状态" 42 258 330 30 18 "#EAF6FA" "Left" $true 3))
    $statusRows = @(
        @{ Id="pcs"; Title="PCS 通讯"; Index=1399; Normal="1"; Good="在线"; Bad="离线" },
        @{ Id="bms"; Title="BMS 通讯"; Index=3999; Normal="1"; Good="在线"; Bad="离线" },
        @{ Id="cooling"; Title="液冷机组"; Index=127; Normal="1"; Good="在线"; Bad="离线" },
        @{ Id="dehumidifier"; Title="除湿机"; Index=188; Normal="1"; Good="在线"; Bad="离线" },
        @{ Id="fire"; Title="消防探测器"; Index=310000; Normal="1"; Good="在线"; Bad="离线" }
    )
    if ($null -ne $dioPoints.emergency) {
        $statusRows += @{ Id="emergency"; Title="急停回路"; Index=$dioPoints.emergency; Normal=(Normal-ValueFor $dioPoints.emergency "0"); Good="正常"; Bad="急停" }
    }
    $y = 304
    foreach ($row in $statusRows) {
        if (-not (Has-Index ([uint32]$row.Index))) { continue }
        $widgets.Add((New-BinaryStatus "overview-state-$($row.Id)" $row.Title $row.Index $row.Normal $row.Good $row.Bad 42 $y 346 92))
        $y += 108
    }

    $widgets.Add((New-Frame "overview-flow-panel" 424 240 956 444 "#081E29" 0))
    $widgets.Add((New-Text "overview-flow-title" "能量流与设备主状态" 446 258 440 30 18 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-ValueCard "overview-grid-power" "并网总有功 (kW)" 1039 468 342 228 158))
    $widgets.Add((New-EnergyFlow "overview-flow-grid-to-pcs" 1039 "positive" 710 386 72 52 30))
    $widgets.Add((New-BinaryStatus "overview-pcs-running" "PCS 运行状态" 1211 "1" "运行" "停机" 790 342 240 158 "#D9A441"))
    $widgets.Add((New-EnergyFlow "overview-flow-pcs-to-battery" 1230 "negative" 1040 386 72 52 30))
    $widgets.Add((New-ProgressCard "overview-battery-soc" "电池 SOC (%)" 1569 1120 318 224 206))
    $widgets.Add((New-ValueCard "overview-pcs-power" "PCS 总有功 (kW)" 1230 468 530 204 120))
    $widgets.Add((New-ValueCard "overview-battery-voltage" "电池总电压 (V)" 1566 690 530 204 120))
    $widgets.Add((New-ValueCard "overview-battery-current" "电池总电流 (A)" 1567 912 530 204 120))
    $widgets.Add((New-ValueCard "overview-battery-soh" "电池 SOH (%)" 1570 1134 530 204 120))

    $widgets.Add((New-Frame "overview-detail-panel" 424 700 956 326 "#081E29" 0))
    $widgets.Add((New-Text "overview-detail-title" "关键运行数据" 446 718 420 30 18 "#EAF6FA" "Left" $true 3))
    $details = @(
        @{ Id="grid-frequency"; Title="电网频率 (Hz)"; Index=1052 },
        @{ Id="pcs-dc"; Title="PCS 直流功率 (kW)"; Index=1243 },
        @{ Id="charge-available"; Title="可充电量 (kWh)"; Index=1590 },
        @{ Id="cooling-out"; Title="液冷出水温度 (℃)"; Index=131 },
        @{ Id="environment-temp"; Title="柜内温度-除湿机 (℃)"; Index=$dehumidifierPoints.cabinetTemperature },
        @{ Id="environment-humidity"; Title="柜内湿度-除湿机 (%RH)"; Index=$dehumidifierPoints.cabinetHumidity }
    )
    for ($i = 0; $i -lt $details.Count; $i++) {
        $col = $i % 3
        $row = [math]::Floor($i / 3)
        $widgets.Add((New-ValueCard "overview-$($details[$i].Id)" $details[$i].Title $details[$i].Index (446 + $col * 302) (764 + $row * 124) 282 104))
    }

    $widgets.Add((New-Widget "overview-active-alarms" "alarmTable" "活动设备告警" 1398 240 498 786 2 @() @() $null ([ordered]@{
        qtBackgroundColor = "#071A2D"
        qtTextColor = "#E8F0F2"
        qtFontSize = 15
    })))
    return New-Screen "Overview" "总览" $widgets
}

function Build-StrategyScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Strategy"

    $widgets.Add((New-Frame "strategy-execution-panel" 24 240 1168 374 "#081E29" 0))
    $widgets.Add((New-Text "strategy-execution-title" "计划、下发与实际执行曲线" 44 254 420 28 18 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Text "strategy-execution-sign" "统一显示方向：充电为正，放电为负" 650 257 512 24 13 "#78B6CC" "Right" $false 3))
    $strategySeriesOptions = @(
        [pscustomobject]@{ PenStyle="DashLine"; Scale=1.0; Offset=0.0; LineWidth=4.0; RenderLayer=0.35; Indexes=@([uint32]627,[uint32]628,[uint32]629); Aggregation="sum" },
        [pscustomobject]@{ PenStyle="solid"; Scale=-1.0; Offset=0.0; LineWidth=2.0; RenderLayer=0.15 }
    )
    $widgets.Add((New-Chart -Id "strategy-execution-chart" -Title "计划、下发与实际执行曲线" `
        -Indexes ([uint32[]]@(627,1230)) `
        -Names ([string[]]@("仲裁后下发计划","PCS 实际执行")) `
        -Units ([string[]]@("kW","kW")) `
        -Colors ([string[]]@("#55D8E8","#55E0AA")) `
        -X 34 -Y 286 -Width 1148 -Height 318 `
        -SeriesOptions $strategySeriesOptions -DefaultWindowMinutes 30 `
        -SampleIntervalSeconds 5 -RenderIntervalMs 3000))

    $widgets.Add((New-Frame "strategy-decision-panel" 1208 240 688 374 "#081E29" 0))
    $widgets.Add((New-Text "strategy-decision-title" "当前执行决策" 1228 254 360 28 18 "#EAF6FA" "Left" $true 3))
    $phaseStates = @(
        @{ Code="idle"; Label="待机"; Color="#71808A"; Value="0" },
        @{ Code="discharge"; Label="放电阶段"; Color="#F9CC44"; Value="1" },
        @{ Code="charge"; Label="充电阶段"; Color="#55E0AA"; Value="2" }
    )
    $widgets.Add((New-BinaryStatus "strategy-status-schedule" "计划曲线" 18 "1" "运行" "待机" 1228 292 204 62 "#71808A"))
    $widgets.Add((New-MultiStatus "strategy-status-cycle" "充放电测试" 17 $phaseStates 1446 292 204 62))
    $widgets.Add((New-BinaryStatus "strategy-status-override" "受控模式" 26 "1" "介入" "未介入" 1664 292 204 62 "#71808A"))
    Add-StrategyMetric $widgets "strategy-current-plan" "当前计划 (kW)" 461 1228 372 300 "#F9CC44"
    Add-StrategyMetric $widgets "strategy-current-output" "策略候选幅值 (kW)" 618 1560 372 308 "#55D8E8"
    Add-StrategyMetric $widgets "strategy-current-actual" "PCS 原始实际 (kW)" 1230 1228 402 300 "#55E0AA"
    Add-StrategyMetric $widgets "strategy-current-soc" "实际 / 目标 SOC (%)" 1569 1560 402 150 "#F3F8FA"
    $widgets.Add((New-BoundText "strategy-current-target-soc" 462 1780 402 88 20 13 "#F9CC44" "Right"))
    $widgets.Add((New-Text "strategy-command-title" "安全约束后的 PCS 三相指令 (kW)" 1228 448 420 22 13 "#78B6CC" "Left" $true 3))
    foreach ($phase in @(
        @{ Id="a"; Label="A 相"; Index=627; X=1228 },
        @{ Id="b"; Label="B 相"; Index=628; X=1446 },
        @{ Id="c"; Label="C 相"; Index=629; X=1664 }
    )) {
        $widgets.Add((New-Text "strategy-command-$($phase.Id)-label" $phase.Label $phase.X 478 64 22 12 "#8FB2BF" "Left" $false 5))
        $widgets.Add((New-BoundText "strategy-command-$($phase.Id)-value" $phase.Index ($phase.X + 68) 476 136 26 17 "#F9CC44" "Right"))
    }
    $widgets.Add((New-Text "strategy-decision-flow" "并行候选  >  优先级仲裁  >  充放电与 SOC 限制  >  三相指令  >  PCS 反馈" 1228 526 640 24 13 "#55D8E8" "Center" $true 3))
    $widgets.Add((New-Text "strategy-decision-note" "右侧实际值保留设备原始符号；上方曲线已转换为统一业务方向。" 1228 565 640 22 12 "#8FB2BF" "Center" $false 3))

    $widgets.Add((New-Frame "strategy-cycle-panel" 24 630 600 184 "#081E29" 0))
    $widgets.Add((New-Text "strategy-cycle-title" "充放电与手动策略" 44 642 270 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-MultiStatus "strategy-cycle-phase" "循环阶段" 17 $phaseStates 44 674 172 40))
    $widgets.Add((New-BinaryStatus "strategy-status-manual-charge" "手动充电" 14 "1" "运行" "待机" 228 674 172 40 "#71808A"))
    $widgets.Add((New-BinaryStatus "strategy-status-manual-discharge" "手动放电" 16 "1" "运行" "待机" 412 674 172 40 "#71808A"))
    $widgets.Add((New-ProgressCard "strategy-cycle-soc" "实际 SOC (%)" 1569 44 728 326 74 18))
    Add-StrategyMetric $widgets "strategy-cycle-target-soc" "计划目标 SOC" 462 388 730 196 "#F9CC44"
    Add-StrategyMetric $widgets "strategy-cycle-charge-output" "手动充电候选" 613 388 756 196 "#55E0AA"
    Add-StrategyMetric $widgets "strategy-cycle-discharge-output" "手动放电候选" 614 388 782 196 "#F9CC44"

    $widgets.Add((New-Frame "strategy-voltage-panel" 640 630 600 184 "#081E29" 0))
    $widgets.Add((New-Text "strategy-voltage-title" "低压 / 高压补偿" 660 642 270 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "strategy-status-low-voltage" "低压补偿" 10 "1" "介入" "未介入" 660 674 260 40 "#71808A"))
    $widgets.Add((New-BinaryStatus "strategy-status-high-voltage" "高压补偿" 12 "1" "介入" "未介入" 940 674 260 40 "#71808A"))
    foreach ($phase in @(
        @{ Id="a"; Label="A 相电压"; Index=1220; X=660 },
        @{ Id="b"; Label="B 相电压"; Index=1221; X=840 },
        @{ Id="c"; Label="C 相电压"; Index=1222; X=1020 }
    )) {
        Add-StrategyMetric $widgets "strategy-voltage-$($phase.Id)" $phase.Label $phase.Index $phase.X 730 160 "#F3F8FA"
    }
    Add-StrategyMetric $widgets "strategy-low-limit" "低压下限" 544 660 756 160
    Add-StrategyMetric $widgets "strategy-low-upper" "低压上限" 545 840 756 160
    Add-StrategyMetric $widgets "strategy-low-output" "低压输出" 608 1020 756 160 "#F9CC44"
    Add-StrategyMetric $widgets "strategy-high-limit" "高压下限" 546 660 782 160
    Add-StrategyMetric $widgets "strategy-high-upper" "高压上限" 547 840 782 160
    Add-StrategyMetric $widgets "strategy-high-output" "高压输出" 612 1020 782 160 "#55E0AA"

    $widgets.Add((New-Frame "strategy-pv-panel" 1256 630 640 184 "#081E29" 0))
    $widgets.Add((New-Text "strategy-pv-title" "光伏优先充电" 1276 642 260 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "strategy-status-solar" "光伏优先" 22 "1" "介入" "未介入" 1276 674 260 40 "#71808A"))
    Add-StrategyMetric $widgets "strategy-pv-grid" "并网功率 (kW)" 1039 1552 680 304 "#55D8E8"
    $widgets.Add((New-Text "strategy-pv-flow-grid" "并网" 1278 730 80 20 12 "#8FB2BF" "Center" $false 5))
    $widgets.Add((New-BoundText "strategy-pv-flow-grid-value" 1039 1278 752 80 24 15 "#55D8E8" "Center"))
    $widgets.Add((New-Text "strategy-pv-arrow-1" ">" 1370 744 28 24 18 "#55D8E8" "Center" $true 5))
    $widgets.Add((New-Text "strategy-pv-flow-output" "充电候选" 1408 730 110 20 12 "#8FB2BF" "Center" $false 5))
    $widgets.Add((New-BoundText "strategy-pv-flow-output-value" 622 1408 752 110 24 15 "#55E0AA" "Center"))
    $widgets.Add((New-Text "strategy-pv-arrow-2" ">" 1528 744 28 24 18 "#55D8E8" "Center" $true 5))
    $widgets.Add((New-Text "strategy-pv-flow-soc" "电池 SOC" 1566 730 100 20 12 "#8FB2BF" "Center" $false 5))
    $widgets.Add((New-BoundText "strategy-pv-flow-soc-value" 1569 1566 752 100 24 15 "#F3F8FA" "Center"))
    Add-StrategyMetric $widgets "strategy-pv-start" "开始小时" 581 1682 730 174
    Add-StrategyMetric $widgets "strategy-pv-end" "结束小时" 583 1682 764 174

    $widgets.Add((New-Frame "strategy-phase-panel" 24 830 600 196 "#081E29" 0))
    $widgets.Add((New-Text "strategy-phase-title" "三相平衡执行矩阵" 44 842 280 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "strategy-status-phase-balance" "三相平衡" 20 "1" "介入" "未介入" 44 876 540 40 "#71808A"))
    foreach ($header in @(
        @{ Id="phase"; Text="相别"; X=44; W=42 },
        @{ Id="load"; Text="负荷有功"; X=92; W=112 },
        @{ Id="candidate"; Text="平衡候选"; X=220; W=112 },
        @{ Id="command"; Text="最终指令"; X=348; W=112 },
        @{ Id="actual"; Text="PCS 实际"; X=476; W=108 }
    )) {
        $widgets.Add((New-Text "strategy-phase-header-$($header.Id)" $header.Text $header.X 924 $header.W 18 11 "#78B6CC" "Center" $true 5))
    }
    $phaseRows = @(
        @{ Id="a"; Label="A"; Load=309; Candidate=623; Command=627; Actual=1227 },
        @{ Id="b"; Label="B"; Load=310; Candidate=624; Command=628; Actual=1228 },
        @{ Id="c"; Label="C"; Load=311; Candidate=625; Command=629; Actual=1229 }
    )
    for ($i=0; $i -lt $phaseRows.Count; $i++) {
        $row = $phaseRows[$i]
        $y = 950 + $i * 26
        $widgets.Add((New-Text "strategy-phase-$($row.Id)-label" $row.Label 44 $y 42 22 13 "#EAF6FA" "Center" $true 5))
        $widgets.Add((New-BoundText "strategy-phase-$($row.Id)-load" $row.Load 92 $y 112 22 13 "#F3F8FA" "Center"))
        $widgets.Add((New-BoundText "strategy-phase-$($row.Id)-candidate" $row.Candidate 220 $y 112 22 13 "#55D8E8" "Center"))
        $widgets.Add((New-BoundText "strategy-phase-$($row.Id)-command" $row.Command 348 $y 112 22 13 "#F9CC44" "Center"))
        $widgets.Add((New-BoundText "strategy-phase-$($row.Id)-actual" $row.Actual 476 $y 108 22 13 "#55E0AA" "Center"))
    }

    $widgets.Add((New-Frame "strategy-reactive-panel" 640 830 600 196 "#081E29" 0))
    $widgets.Add((New-Text "strategy-reactive-title" "无功补偿" 660 842 220 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "strategy-status-reactive" "无功补偿" 8 "1" "介入" "未介入" 660 876 540 40 "#71808A"))
    Add-StrategyMetric $widgets "strategy-reactive-target-pf" "目标功率因数" 514 660 928 250 "#F9CC44"
    Add-StrategyMetric $widgets "strategy-reactive-actual-pf" "负荷实际功率因数" 324 930 928 270 "#55E0AA"
    foreach ($phase in @(
        @{ Id="a"; Label="A 相 Q"; Index=601; X=660 },
        @{ Id="b"; Label="B 相 Q"; Index=602; X=840 },
        @{ Id="c"; Label="C 相 Q"; Index=603; X=1020 }
    )) {
        Add-StrategyMetric $widgets "strategy-reactive-$($phase.Id)" $phase.Label $phase.Index $phase.X 958 160 "#55D8E8"
    }
    Add-StrategyMetric $widgets "strategy-reactive-total" "补偿总输出 (kvar)" 604 660 992 250 "#55D8E8"
    Add-StrategyMetric $widgets "strategy-reactive-pcs" "PCS 实际无功 (kvar)" 1234 930 992 270 "#F3F8FA"

    $widgets.Add((New-Frame "strategy-safety-panel" 1256 830 640 196 "#081E29" 0))
    $widgets.Add((New-Text "strategy-safety-title" "受控模式与安全约束" 1276 842 330 26 17 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "strategy-status-reserve" "动态增容" 24 "1" "介入" "未介入" 1276 876 280 40 "#71808A"))
    $widgets.Add((New-BinaryStatus "strategy-safety-override-status" "受控模式" 26 "1" "介入" "未介入" 1572 876 304 40 "#71808A"))
    Add-StrategyMetric $widgets "strategy-safety-override" "受控目标 (kW)" 590 1276 930 280 "#F9CC44"
    Add-StrategyMetric $widgets "strategy-safety-rated" "PCS 容量 (kVA)" 151 1580 930 296
    Add-StrategyMetric $widgets "strategy-safety-charge" "允许充电 (kW)" 1552 1276 962 280 "#55E0AA"
    Add-StrategyMetric $widgets "strategy-safety-discharge" "允许放电 (kW)" 1553 1580 962 296 "#55E0AA"
    Add-StrategyMetric $widgets "strategy-safety-soc-min" "SOC 下限 (%)" 162 1276 994 280
    Add-StrategyMetric $widgets "strategy-safety-soc-max" "SOC 上限 (%)" 161 1580 994 296

    return New-Screen "Strategy" "策略执行" $widgets
}

function Build-DevicesScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Devices"
    $columns = @(24, 492, 960, 1428)
    foreach ($i in 0..3) { $widgets.Add((New-Frame "devices-panel-$i" $columns[$i] 240 448 786 "#081E29" 0)) }
    $widgets.Add((New-Text "devices-pcs-title" "PCS" 44 258 400 30 19 "#55D8E8" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "devices-pcs-run" "运行状态" 1211 "1" "运行" "停机" 44 304 408 78 "#D9A441"))
    $widgets.Add((New-BinaryStatus "devices-pcs-fault" "总故障" 1212 "0" "正常" "故障" 44 394 408 78))
    $pcsValues = @(
        @{ T="总有功 (kW)"; I=1230 }, @{ T="总无功 (kVar)"; I=1234 },
        @{ T="A 相电压 (V)"; I=1220 }, @{ T="A 相电流 (A)"; I=1223 },
        @{ T="B 相电压 (V)"; I=1221 }, @{ T="B 相电流 (A)"; I=1224 },
        @{ T="C 相电压 (V)"; I=1222 }, @{ T="C 相电流 (A)"; I=1225 },
        @{ T="直流电压 (V)"; I=1244 }, @{ T="直流电流 (A)"; I=1245 }
    )
    for ($i=0; $i -lt $pcsValues.Count; $i++) {
        $widgets.Add((New-ValueCard "devices-pcs-value-$i" $pcsValues[$i].T $pcsValues[$i].I (44 + ($i % 2) * 204) (484 + [math]::Floor($i / 2) * 102) 192 90))
    }

    $widgets.Add((New-Text "devices-bms-title" "BMS" 512 258 400 30 19 "#55E0AA" "Left" $true 3))
    $bmsStates = @(
        @{ Code="ready"; Label="正常"; Color="#20C879"; Value="0" },
        @{ Code="charge-disabled"; Label="禁充"; Color="#D9A441"; Value="1" },
        @{ Code="discharge-disabled"; Label="禁放"; Color="#D9A441"; Value="2" },
        @{ Code="standby"; Label="待机"; Color="#78B6CC"; Value="3" },
        @{ Code="stopped"; Label="停机"; Color="#8A9AA2"; Value="4" }
    )
    $widgets.Add((New-MultiStatus "devices-bms-state" "运行状态" 1550 $bmsStates 512 304 408 78))
    $widgets.Add((New-ProgressCard "devices-bms-soc" "SOC (%)" 1569 512 394 408 118))
    $bmsValues = @(
        @{ T="SOH (%)"; I=1570 }, @{ T="总电压 (V)"; I=1566 },
        @{ T="总电流 (A)"; I=1567 }, @{ T="平均温度 (℃)"; I=1573 },
        @{ T="最高单体电压 (mV)"; I=1574 }, @{ T="最低单体电压 (mV)"; I=1577 },
        @{ T="最高单体温度 (℃)"; I=1580 }, @{ T="最低单体温度 (℃)"; I=1583 },
        @{ T="可充电量 (kWh)"; I=1590 }, @{ T="可放电量 (kWh)"; I=1591 }
    )
    for ($i=0; $i -lt $bmsValues.Count; $i++) {
        $widgets.Add((New-ValueCard "devices-bms-value-$i" $bmsValues[$i].T $bmsValues[$i].I (512 + ($i % 2) * 204) (526 + [math]::Floor($i / 2) * 94) 192 82))
    }

    $widgets.Add((New-Text "devices-thermal-title" "热管理设备" 980 258 400 30 19 "#F9CC44" "Left" $true 3))

    $widgets.Add((New-Text "devices-liquid-title" "液冷机" 980 304 190 28 17 "#55D8E8" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "devices-cooling-online" "液冷通讯" 127 "1" "在线" "离线" 1186 294 194 74))
    $liquidCoolingValues = @(
        @{ T="出水温度 (℃)"; I=131 }, @{ T="回水温度 (℃)"; I=132 },
        @{ T="液冷机外环境温度 (℃)"; I=135 }, @{ T="出水压力 (Bar)"; I=141 },
        @{ T="回水压力 (Bar)"; I=142 }, @{ T="液冷故障代码"; I=153 }
    )
    for ($i=0; $i -lt $liquidCoolingValues.Count; $i++) {
        if (-not (Has-Index ([uint32]$liquidCoolingValues[$i].I))) { continue }
        $widgets.Add((New-ValueCard "devices-liquid-value-$i" $liquidCoolingValues[$i].T $liquidCoolingValues[$i].I (980 + ($i % 2) * 204) (380 + [math]::Floor($i / 2) * 94) 192 82))
    }

    $widgets.Add((New-Text "devices-dehumidifier-title" "除湿机 / 柜内环境" 980 676 200 28 17 "#F9CC44" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "devices-dehumidifier-online" "除湿通讯" 188 "1" "在线" "离线" 1186 664 194 74))
    if ($null -ne $dehumidifierPoints.cabinetTemperature) {
        $widgets.Add((New-ValueCard "devices-dehumidifier-cabinet-temperature" "柜内温度 (℃)" $dehumidifierPoints.cabinetTemperature 980 748 400 84))
    }
    if ($null -ne $dehumidifierPoints.cabinetHumidity) {
        $widgets.Add((New-ValueCard "devices-dehumidifier-cabinet-humidity" "柜内湿度 (%RH)" $dehumidifierPoints.cabinetHumidity 980 844 192 84))
    }
    if ($null -ne $dehumidifierPoints.internalTemperature) {
        $widgets.Add((New-ValueCard "devices-dehumidifier-internal-temperature" "除湿机内部温度 (℃)" $dehumidifierPoints.internalTemperature 1186 844 194 84))
    }

    $widgets.Add((New-Text "devices-fire-title" "消防与柜体 IO" 1448 258 400 30 19 "#FF8A7A" "Left" $true 3))
    $widgets.Add((New-BinaryStatus "devices-fire-online" "消防探测器" 310000 "1" "在线" "离线" 1448 304 194 78))
    $widgets.Add((New-BinaryStatus "devices-fire-alarm" "消防综合报警" 310012 "0" "正常" "报警" 1654 304 194 78))
    $fireValues = @(
        @{ T="探测温度 (℃)"; I=310101 }, @{ T="烟雾减光率 (%)"; I=310102 },
        @{ T="CO (ppm)"; I=310103 }, @{ T="H2 (ppm)"; I=310104 },
        @{ T="VOC (ppm)"; I=310105 }
    )
    for ($i=0; $i -lt $fireValues.Count; $i++) {
        $widgets.Add((New-ValueCard "devices-fire-value-$i" $fireValues[$i].T $fireValues[$i].I (1448 + ($i % 2) * 204) (394 + [math]::Floor($i / 2) * 104) 192 92))
    }
    $ioRows = @()
    if ($null -ne $dioPoints.emergency) { $ioRows += @{ Id="emergency"; T="急停"; I=$dioPoints.emergency; N=(Normal-ValueFor $dioPoints.emergency "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.surge) { $ioRows += @{ Id="surge"; T="浪涌保护"; I=$dioPoints.surge; N=(Normal-ValueFor $dioPoints.surge "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.water) { $ioRows += @{ Id="water"; T="水浸"; I=$dioPoints.water; N=(Normal-ValueFor $dioPoints.water "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.fireAction) { $ioRows += @{ Id="fire-action"; T="消防动作"; I=$dioPoints.fireAction; N=(Normal-ValueFor $dioPoints.fireAction "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.frontDoor) { $ioRows += @{ Id="front-door"; T="前门"; I=$dioPoints.frontDoor; N=(Normal-ValueFor $dioPoints.frontDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.rearDoor) { $ioRows += @{ Id="rear-door"; T="后门"; I=$dioPoints.rearDoor; N=(Normal-ValueFor $dioPoints.rearDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.storageBreaker) { $ioRows += @{ Id="storage-breaker"; T="储能断路器"; I=$dioPoints.storageBreaker; N="1"; G="合位"; B="分位" } }
    if ($null -ne $dioPoints.gridBreaker) { $ioRows += @{ Id="grid-breaker"; T="并网断路器"; I=$dioPoints.gridBreaker; N="1"; G="合位"; B="分位" } }
    if ($null -ne $dioPoints.loadBreaker) { $ioRows += @{ Id="load-breaker"; T="负荷断路器"; I=$dioPoints.loadBreaker; N="1"; G="合位"; B="分位" } }
    for ($i=0; $i -lt $ioRows.Count; $i++) {
        $row=$ioRows[$i]
        $widgets.Add((New-BinaryStatus "devices-io-$($row.Id)" $row.T $row.I $row.N $row.G $row.B (1448 + ($i % 2) * 204) (706 + [math]::Floor($i / 2) * 80) 192 70 "#D9A441"))
    }
    return New-Screen "Devices" "设备" $widgets
}

function Build-AlarmsScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Alarms"
    $widgets.Add((New-Frame "alarms-summary-panel" 24 240 348 786 "#081E29" 0))
    $widgets.Add((New-Text "alarms-summary-title" "设备告警摘要" 44 258 300 30 18 "#EAF6FA" "Left" $true 3))
    $rows = @(
        @{ Id="pcs-fault"; T="PCS 总故障"; I=1212; N="0"; G="正常"; B="故障" },
        @{ Id="pcs-alarm"; T="PCS 总报警"; I=1213; N="0"; G="正常"; B="报警" },
        @{ Id="bms-fault"; T="BMS 故障"; I=1462; N="0"; G="正常"; B="故障" },
        @{ Id="cooling"; T="液冷故障码"; I=153; N="0"; G="正常"; B="故障" },
        @{ Id="fire-alarm"; T="消防综合报警"; I=310012; N="0"; G="正常"; B="报警" },
        @{ Id="fire-fault"; T="消防综合故障"; I=310013; N="0"; G="正常"; B="故障" }
    )
    if ($null -ne $dehumidifierPoints.fault) { $rows += @{ Id="dehumidifier"; T="除湿故障码"; I=$dehumidifierPoints.fault; N="0"; G="正常"; B="故障" } }
    if ($null -ne $dioPoints.emergency) { $rows += @{ Id="emergency"; T="急停回路"; I=$dioPoints.emergency; N=(Normal-ValueFor $dioPoints.emergency "0"); G="正常"; B="动作" } }
    for ($i=0; $i -lt $rows.Count; $i++) {
        $row=$rows[$i]
        $widgets.Add((New-BinaryStatus "alarms-summary-$($row.Id)" $row.T $row.I $row.N $row.G $row.B 44 (304 + $i * 86) 308 74))
    }
    $widgets.Add((New-Widget "alarms-device-table" "alarmTable" "活动设备告警" 390 240 1506 786 2 @() @() $null ([ordered]@{
        qtBackgroundColor = "#071A2D"
        qtTextColor = "#E8F0F2"
        qtFontSize = 16
    })))
    return New-Screen "Alarms" "告警" $widgets
}

function Build-TrendsScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Trends"
    $panels = @(
        @{ Id="pcs"; X=24; Y=240; Title="PCS 三相有功功率"; Indexes=[uint32[]]@(1227,1228,1229,1230); Names=@("A 相","B 相","C 相","合相"); Units=@("kW","kW","kW","kW"); Colors=@("#55D8E8","#55E0AA","#F9CC44","#F3F8FA") },
        @{ Id="grid"; X=974; Y=240; Title="并网与储能侧功率"; Indexes=[uint32[]]@(1039,1139); Names=@("并网侧","储能侧"); Units=@("kW","kW"); Colors=@("#55D8E8","#F9CC44") },
        @{ Id="battery"; X=24; Y=648; Title="电池 SOC / SOH"; Indexes=[uint32[]]@(1569,1570); Names=@("SOC","SOH"); Units=@("%","%"); Colors=@("#55E0AA","#55D8E8") },
        @{ Id="energy"; X=974; Y=648; Title="电池可充 / 可放电量"; Indexes=[uint32[]]@(1590,1591); Names=@("可充电量","可放电量"); Units=@("kWh","kWh"); Colors=@("#55D8E8","#F9CC44") }
    )
    foreach ($panel in $panels) {
        $widgets.Add((New-Frame "trends-frame-$($panel.Id)" $panel.X $panel.Y 922 378 "#081E29" 0))
        $widgets.Add((New-Text "trends-title-$($panel.Id)" $panel.Title ($panel.X + 18) ($panel.Y + 14) 520 28 17 "#EAF6FA" "Left" $true 3))
        $widgets.Add((New-Chart "trends-chart-$($panel.Id)" $panel.Title $panel.Indexes $panel.Names $panel.Units $panel.Colors ($panel.X + 10) ($panel.Y + 42) 902 326))
    }
    return New-Screen "Trends" "趋势" $widgets
}

function Build-ControlScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Control"
    $widgets.Add((New-Frame "control-interlock-panel" 24 240 376 786 "#081E29" 0))
    $widgets.Add((New-Text "control-interlock-title" "控制前置状态" 44 258 330 30 18 "#EAF6FA" "Left" $true 3))
    $interlocks = @(
        @{ Id="pcs-online"; T="PCS 通讯"; I=1399; N="1"; G="在线"; B="离线" },
        @{ Id="pcs-fault"; T="PCS 故障"; I=1212; N="0"; G="正常"; B="故障" },
        @{ Id="pcs-alarm"; T="PCS 报警"; I=1213; N="0"; G="正常"; B="报警" },
        @{ Id="bms-fault"; T="BMS 故障"; I=1462; N="0"; G="正常"; B="故障" }
    )
    if ($null -ne $dioPoints.emergency) { $interlocks += @{ Id="emergency"; T="急停回路"; I=$dioPoints.emergency; N=(Normal-ValueFor $dioPoints.emergency "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.gridBreaker) { $interlocks += @{ Id="breaker"; T="并网断路器"; I=$dioPoints.gridBreaker; N="1"; G="合位"; B="分位" } }
    for ($i=0; $i -lt $interlocks.Count; $i++) {
        $row=$interlocks[$i]
        $widgets.Add((New-BinaryStatus "control-interlock-$($row.Id)" $row.T $row.I $row.N $row.G $row.B 44 (304 + $i * 94) 336 82 "#D9A441"))
    }
    $widgets.Add((New-Text "control-priority-note" "功率控件可单独选择高优先级" 44 894 330 28 14 "#F9CC44" "Left" $false 3))
    $widgets.Add((New-Text "control-safety-note" "并网脱扣继电器仅监视，不提供普通控制" 44 932 330 50 14 "#FF8A7A" "Left" $false 3))

    $widgets.Add((New-Frame "control-pcs-command-panel" 418 240 1052 160 "#081E29" 0))
    $widgets.Add((New-Text "control-pcs-command-title" "PCS 运行控制" 438 256 400 28 18 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-ControlButton "control-pcs-start" "PCS 启动" 1201 "pulse" "1" 438 306 238 64 $false "#124536"))
    $widgets.Add((New-ControlButton "control-pcs-standby" "PCS 待机" 1207 "pulse" "1" 696 306 238 64 $false "#113746"))
    $widgets.Add((New-ControlButton "control-pcs-stop" "PCS 停机" 1202 "pulse" "1" 954 306 238 64 $true "#4B272B"))
    $widgets.Add((New-ControlButton "control-pcs-reset" "PCS 故障复位" 1200 "pulse" "1" 1212 306 238 64 $true "#493E20"))

    $powerBindings = @(
        (New-Binding 1318 "activeA"), (New-Binding 1319 "activeB"), (New-Binding 1320 "activeC"),
        (New-Binding 1321 "reactiveA"), (New-Binding 1322 "reactiveB"), (New-Binding 1323 "reactiveC")
    )
    $widgets.Add((New-Widget "control-pcs-phase-power" "pcsPhasePowerControl" "PCS 三相功率设定" 418 416 1052 430 5 $powerBindings @() $null ([ordered]@{})))

    $widgets.Add((New-Frame "control-do-panel" 418 862 1052 164 "#081E29" 0))
    $widgets.Add((New-Text "control-do-title" "安全 DO 输出" 438 876 400 28 18 "#EAF6FA" "Left" $true 3))
    $doControls = @()
    if ($null -ne $dioPoints.fan) {
        $doControls += @{ Id="fan-on"; T="风机启动"; I=$dioPoints.fan; V="1"; C="#124536" }, @{ Id="fan-off"; T="风机停止"; I=$dioPoints.fan; V="0"; C="#273C46" }
    }
    if ($null -ne $dioPoints.runLamp) {
        $doControls += @{ Id="run-on"; T="运行灯点亮"; I=$dioPoints.runLamp; V="1"; C="#124536" }, @{ Id="run-off"; T="运行灯熄灭"; I=$dioPoints.runLamp; V="0"; C="#273C46" }
    }
    if ($null -ne $dioPoints.chargeLamp) {
        $doControls += @{ Id="charge-on"; T="充电灯点亮"; I=$dioPoints.chargeLamp; V="1"; C="#124536" }, @{ Id="charge-off"; T="充电灯熄灭"; I=$dioPoints.chargeLamp; V="0"; C="#273C46" }
    }
    if ($null -ne $dioPoints.dischargeLamp) {
        $doControls += @{ Id="discharge-on"; T="放电灯点亮"; I=$dioPoints.dischargeLamp; V="1"; C="#124536" }, @{ Id="discharge-off"; T="放电灯熄灭"; I=$dioPoints.dischargeLamp; V="0"; C="#273C46" }
    }
    for ($i=0; $i -lt $doControls.Count; $i++) {
        $item=$doControls[$i]
        $widgets.Add((New-ControlButton "control-do-$($item.Id)" $item.T $item.I "writeSetpoint" $item.V (438 + ($i % 4) * 252) (916 + [math]::Floor($i / 4) * 50) 234 42 $false $item.C))
    }

    $widgets.Add((New-Frame "control-di-panel" 1488 240 408 786 "#081E29" 0))
    $widgets.Add((New-Text "control-di-title" "DI 只读反馈" 1508 258 360 30 18 "#EAF6FA" "Left" $true 3))
    $diRows = @()
    if ($null -ne $dioPoints.emergency) { $diRows += @{ Id="emergency"; T="急停反馈"; I=$dioPoints.emergency; N=(Normal-ValueFor $dioPoints.emergency "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.surge) { $diRows += @{ Id="surge"; T="浪涌保护器"; I=$dioPoints.surge; N=(Normal-ValueFor $dioPoints.surge "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.water) { $diRows += @{ Id="water"; T="水浸"; I=$dioPoints.water; N=(Normal-ValueFor $dioPoints.water "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.fireAction) { $diRows += @{ Id="fire-action"; T="消防动作"; I=$dioPoints.fireAction; N=(Normal-ValueFor $dioPoints.fireAction "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.frontDoor) { $diRows += @{ Id="front-door"; T="前门"; I=$dioPoints.frontDoor; N=(Normal-ValueFor $dioPoints.frontDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.rearDoor) { $diRows += @{ Id="rear-door"; T="后门"; I=$dioPoints.rearDoor; N=(Normal-ValueFor $dioPoints.rearDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.storageBreaker) { $diRows += @{ Id="storage-breaker"; T="储能断路器"; I=$dioPoints.storageBreaker; N="1"; G="合位"; B="分位" } }
    if ($null -ne $dioPoints.gridBreaker) { $diRows += @{ Id="grid-breaker"; T="并网断路器"; I=$dioPoints.gridBreaker; N="1"; G="合位"; B="分位" } }
    if ($null -ne $dioPoints.loadBreaker) { $diRows += @{ Id="load-breaker"; T="负荷断路器"; I=$dioPoints.loadBreaker; N="1"; G="合位"; B="分位" } }
    for ($i=0; $i -lt $diRows.Count; $i++) {
        $row=$diRows[$i]
        $widgets.Add((New-BinaryStatus "control-di-$($row.Id)" $row.T $row.I $row.N $row.G $row.B 1508 (304 + $i * 88) 368 76 "#D9A441"))
    }
    return New-Screen "Control" "控制" $widgets
}

function Build-MaintenanceScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-HeaderAndFooter $widgets "Maintenance"
    $panels = @(24, 642, 1260)
    foreach($i in 0..2) { $widgets.Add((New-Frame "maintenance-panel-$i" $panels[$i] 240 594 786 "#081E29" 0)) }
    $widgets.Add((New-Text "maintenance-comms-title" "设备通讯" 46 258 540 30 19 "#55D8E8" "Left" $true 3))
    $comms = @(
        @{ Id="pcs"; T="PCS"; I=1399 }, @{ Id="bms"; T="BMS"; I=3999 },
        @{ Id="cooling"; T="液冷机组"; I=127 }, @{ Id="dehumidifier"; T="除湿机"; I=188 },
        @{ Id="fire"; T="消防探测器"; I=310000 }
    )
    for($i=0; $i -lt $comms.Count; $i++) {
        $widgets.Add((New-BinaryStatus "maintenance-comms-$($comms[$i].Id)" $comms[$i].T $comms[$i].I "1" "在线" "离线" 46 (310 + $i * 112) 550 98))
    }
    $widgets.Add((New-ValueCard "maintenance-grid-frequency" "并网频率 (Hz)" 1052 46 886 265 112))
    $widgets.Add((New-ValueCard "maintenance-pcs-frequency" "PCS 电网频率 (Hz)" 1226 331 886 265 112))

    $widgets.Add((New-Text "maintenance-ems-title" "EMS 策略运行" 664 258 540 30 19 "#55E0AA" "Left" $true 3))
    $strategy = @(
        @{ Id="auto"; T="EMS 自动模式"; I=1 },
        @{ Id="manual-charge"; T="手动充电"; I=14 }, @{ Id="manual-discharge"; T="手动放电"; I=16 },
        @{ Id="schedule"; T="计划曲线"; I=18 }, @{ Id="balance"; T="三相平衡"; I=20 },
        @{ Id="capacity"; T="动态增容"; I=24 }, @{ Id="override"; T="受控模式"; I=26 }
    )
    $strategyY = 310
    foreach ($item in $strategy) {
        if (-not (Has-Index ([uint32]$item.I))) { continue }
        $widgets.Add((New-BinaryStatus "maintenance-strategy-$($item.Id)" $item.T $item.I "1" "运行" "未运行" 664 $strategyY 550 84 "#8A9AA2"))
        $strategyY += 96
    }

    $widgets.Add((New-Text "maintenance-project-title" "工程与柜体状态" 1282 258 540 30 19 "#F9CC44" "Left" $true 3))
    $projectRows = @(
        @{ Label="工程名称"; Value=$DisplayName },
        @{ Label="工程版本"; Value=$PackageVersion },
        @{ Label="Machine Code"; Value=$MachineCode },
        @{ Label="运行拓扑"; Value="边端一体化" },
        @{ Label="实时数据源"; Value="共享内存直读" },
        @{ Label="刷新周期"; Value="500 ms" }
    )
    for($i=0; $i -lt $projectRows.Count; $i++) {
        $y=312 + $i * 64
        $widgets.Add((New-Text "maintenance-project-label-$i" $projectRows[$i].Label 1282 $y 190 28 14 "#78B6CC" "Left" $false 3))
        $widgets.Add((New-Text "maintenance-project-value-$i" $projectRows[$i].Value 1480 $y 350 28 15 "#F3F8FA" "Right" $true 3))
    }
    $cabinetRows = @()
    if ($null -ne $dioPoints.frontDoor) { $cabinetRows += @{ Id="front-door"; T="前门反馈"; I=$dioPoints.frontDoor; N=(Normal-ValueFor $dioPoints.frontDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.rearDoor) { $cabinetRows += @{ Id="rear-door"; T="后门反馈"; I=$dioPoints.rearDoor; N=(Normal-ValueFor $dioPoints.rearDoor "0"); G="关闭"; B="打开" } }
    if ($null -ne $dioPoints.storageBreaker) { $cabinetRows += @{ Id="storage-breaker"; T="储能断路器"; I=$dioPoints.storageBreaker; N="1"; G="合位"; B="分位" } }
    if ($null -ne $dioPoints.surge) { $cabinetRows += @{ Id="surge"; T="浪涌保护"; I=$dioPoints.surge; N=(Normal-ValueFor $dioPoints.surge "0"); G="正常"; B="动作" } }
    if ($null -ne $dioPoints.fireAction) { $cabinetRows += @{ Id="fire-action"; T="消防动作"; I=$dioPoints.fireAction; N=(Normal-ValueFor $dioPoints.fireAction "0"); G="正常"; B="动作" } }
    for ($i=0; $i -lt [math]::Min(4, $cabinetRows.Count); $i++) {
        $row = $cabinetRows[$i]
        $widgets.Add((New-BinaryStatus "maintenance-$($row.Id)" $row.T $row.I $row.N $row.G $row.B (1282 + ($i % 2) * 284) (730 + [math]::Floor($i / 2) * 112) 264 94 "#D9A441"))
    }
    return New-Screen "Maintenance" "运维" $widgets
}

function Add-CompactChrome(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$ActiveSection
) {
    $Widgets.Add((New-Frame "chrome-header" 0 0 1920 104 "#071B26" -10))
    $Widgets.Add((New-Frame "chrome-brand" 26 26 50 50 "#D9F4F7" 1))
    $Widgets.Add((New-Text "chrome-brand-text" "KY" 34 37 34 28 16 "#09202B" "Center" $true 3))
    $Widgets.Add((New-Text "chrome-title" $DisplayName 92 19 360 38 24 "#F3F8FA" "Left" $true 3))
    $Widgets.Add((New-Text "chrome-subtitle" $MachineCode 92 59 360 26 16 "#74B9D0" "Left" $false 3))

    $pages = @(
        @{ Id="Overview"; Text="总览"; Target="Overview" },
        @{ Id="Strategy"; Text="策略"; Target="Strategy-Overview" },
        @{ Id="Devices"; Text="设备"; Target="Devices-Pcs" },
        @{ Id="Alarms"; Text="告警"; Target="Alarms" },
        @{ Id="Trends"; Text="趋势"; Target="Trends-Power" },
        @{ Id="Control"; Text="控制"; Target="Control-Pcs" },
        @{ Id="Maintenance"; Text="运维"; Target="Maintenance" }
    )
    $x = 520
    foreach ($page in $pages) {
        $button = New-NavigationButton "nav-$($page.Id)" $page.Text $page.Target ($page.Id -eq $ActiveSection) $x 18 128 70
        $button.properties.qtFontSize = 20
        $Widgets.Add($button)
        $x += 138
    }
    $Widgets.Add((New-Text "chrome-runtime" "● 本地运行" 1510 37 370 30 18 "#55E0AA" "Right" $true 3))
    $Widgets.Add((New-Text "chrome-footer" "共享内存直读 · 页面刷新 500 ms" 24 1048 760 24 15 "#74B9D0" "Left" $false 3))
    $Widgets.Add((New-Text "chrome-machine" $MachineCode 1570 1048 326 24 15 "#74B9D0" "Right" $false 3))
}

function Add-CompactTabs(
    [System.Collections.Generic.List[object]]$Widgets,
    [object[]]$Tabs,
    [string]$ActiveId
) {
    $x = 24
    foreach ($tab in $Tabs) {
        $button = New-NavigationButton "subnav-$($tab.Id)" $tab.Text $tab.Target ($tab.Id -eq $ActiveId) $x 120 210 66
        $button.properties.qtFontSize = 20
        $Widgets.Add($button)
        $x += 222
    }
}

function Add-CompactMetricCard(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$Unit = "",
    [string]$Accent = "#55D8E8",
    [string]$ValueMapJson = ""
) {
    if ($Index -eq 0 -or -not (Has-Index $Index)) { return }
    $Widgets.Add((New-Frame "$Id-frame" $X $Y $Width $Height "#0B2633" 0))
    $Widgets.Add((New-Frame "$Id-accent" $X $Y 5 $Height $Accent 1))
    $contentTop = $Y + [math]::Max(14.0, ($Height - 94.0) / 2.0)
    $Widgets.Add((New-Text "$Id-title" $Title ($X + 18) $contentTop ($Width - 36) 30 22 "#9FC0CC" "Center" $false 4))
    $value = New-BoundText "$Id-value" $Index ($X + 18) ($contentTop + 34) ($Width - 36) 56 42 "#F3F8FA" "Center" $ValueMapJson
    $value.properties | Add-Member -NotePropertyName qtVerticalAlignment -NotePropertyValue "Center" -Force
    if (-not [string]::IsNullOrWhiteSpace($Unit)) {
        $value.properties | Add-Member -NotePropertyName valueSuffix -NotePropertyValue " $Unit" -Force
    }
    $Widgets.Add($value)
}

function Add-CompactBinaryCard(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [string]$NormalValue,
    [string]$NormalLabel,
    [string]$AbnormalLabel,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$AbnormalColor = "#E45858"
) {
    if ($Index -eq 0 -or -not (Has-Index $Index)) { return }
    $Widgets.Add((New-Frame "$Id-frame" $X $Y $Width $Height "#0B2633" 0))
    $compact = $Height -le 100
    $titleFont = if ($compact) { 18 } else { 22 }
    $statusFont = if ($compact) { 25 } else { 32 }
    $titleHeight = if ($compact) { 26 } else { 30 }
    $statusTop = if ($compact) { $Y + 34 } else { $Y + 48 }
    $Widgets.Add((New-Text "$Id-title" $Title ($X + 14) ($Y + 10) ($Width - 28) $titleHeight $titleFont "#9FC0CC" "Center" $false 4))
    $status = New-BinaryStatus $Id "" $Index $NormalValue $NormalLabel $AbnormalLabel ($X + 12) $statusTop ($Width - 24) ($Y + $Height - $statusTop - 8) $AbnormalColor
    $status.properties.qtBackgroundColor = "transparent"
    $status.properties.qtFontSize = $statusFont
    $status.properties | Add-Member -NotePropertyName qtTextAlignment -NotePropertyValue "Center" -Force
    $status.properties | Add-Member -NotePropertyName qtVerticalAlignment -NotePropertyValue "Center" -Force
    $status.properties | Add-Member -NotePropertyName qtBorderless -NotePropertyValue "true" -Force
    $Widgets.Add($status)
}

function Add-CompactMultiCard(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [uint32]$Index,
    [object[]]$States,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    if ($Index -eq 0 -or -not (Has-Index $Index)) { return }
    $Widgets.Add((New-Frame "$Id-frame" $X $Y $Width $Height "#0B2633" 0))
    $Widgets.Add((New-Text "$Id-title" $Title ($X + 14) ($Y + 10) ($Width - 28) 30 22 "#9FC0CC" "Center" $false 4))
    $status = New-MultiStatus $Id "" $Index $States ($X + 12) ($Y + 40) ($Width - 24) ($Height - 50)
    $status.properties.qtBackgroundColor = "transparent"
    $status.properties.qtFontSize = 32
    $status.properties | Add-Member -NotePropertyName qtTextAlignment -NotePropertyValue "Center" -Force
    $status.properties | Add-Member -NotePropertyName qtVerticalAlignment -NotePropertyValue "Center" -Force
    $status.properties | Add-Member -NotePropertyName qtBorderless -NotePropertyValue "true" -Force
    $Widgets.Add($status)
}

function Add-CompactMetricGrid(
    [System.Collections.Generic.List[object]]$Widgets,
    [object[]]$Items,
    [int]$Columns,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    $gap = 16.0
    $rows = [math]::Ceiling($Items.Count / [double]$Columns)
    $cardWidth = ($Width - ($Columns - 1) * $gap) / $Columns
    $cardHeight = ($Height - ($rows - 1) * $gap) / $rows
    for ($i = 0; $i -lt $Items.Count; $i++) {
        $item = $Items[$i]
        $col = $i % $Columns
        $row = [math]::Floor($i / $Columns)
        $accent = if ($null -ne $item.PSObject.Properties["Accent"]) { [string]$item.Accent } else { "#55D8E8" }
        $unit = if ($null -ne $item.PSObject.Properties["Unit"]) { [string]$item.Unit } else { "" }
        $map = if ($null -ne $item.PSObject.Properties["Map"]) { [string]$item.Map } else { "" }
        Add-CompactMetricCard $Widgets ([string]$item.Id) ([string]$item.Title) ([uint32]$item.Index) `
            ($X + $col * ($cardWidth + $gap)) ($Y + $row * ($cardHeight + $gap)) $cardWidth $cardHeight $unit $accent $map
    }
}

function Compact-PhaseStates {
    return @(
        @{ Code="idle"; Label="待机"; Color="#71808A"; Value="0" },
        @{ Code="discharge"; Label="放电阶段"; Color="#F9CC44"; Value="1" },
        @{ Code="charge"; Label="充电阶段"; Color="#55E0AA"; Value="2" }
    )
}

function Build-CompactOverviewScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Overview"
    $kpiWidth = 361.6
    Add-CompactBinaryCard $widgets "overview-pcs-state" "PCS 运行状态" 1211 "1" "运行" "停机" 24 126 $kpiWidth 144 "#D9A441"
    Add-CompactMetricCard $widgets "overview-grid-power" "并网有功" 1039 401.6 126 $kpiWidth 144 "kW" "#55D8E8"
    Add-CompactMetricCard $widgets "overview-soc" "电池 SOC" 1569 779.2 126 $kpiWidth 144 "%" "#55E0AA"
    Add-CompactMetricCard $widgets "overview-charge" "今日充电" 1615 1156.8 126 $kpiWidth 144 "kWh" "#55D8E8"
    Add-CompactMetricCard $widgets "overview-discharge" "今日放电" 1616 1534.4 126 $kpiWidth 144 "kWh" "#F9CC44"

    $widgets.Add((New-Frame "overview-flow-panel" 24 290 1244 738 "#081E29" 0))
    $widgets.Add((New-Text "overview-flow-title" "能量流与关键运行数据" 48 310 500 34 24 "#EAF6FA" "Left" $true 3))
    Add-CompactMetricCard $widgets "overview-flow-grid" "并网功率" 1039 60 382 330 166 "kW" "#55D8E8"
    $widgets.Add((New-EnergyFlow "overview-flow-grid-to-pcs" 1039 "positive" 406 432 94 60 30))
    Add-CompactBinaryCard $widgets "overview-flow-pcs" "PCS 状态" 1211 "1" "运行" "停机" 516 382 330 166 "#D9A441"
    $widgets.Add((New-EnergyFlow "overview-flow-pcs-to-battery" 1230 "negative" 862 432 94 60 30))
    Add-CompactMetricCard $widgets "overview-flow-battery" "电池 SOC" 1569 972 382 260 166 "%" "#55E0AA"
    $details = @(
        [pscustomobject]@{ Id="overview-pcs-power"; Title="PCS 总有功"; Index=1230; Unit="kW" },
        [pscustomobject]@{ Id="overview-battery-voltage"; Title="电池总电压"; Index=1566; Unit="V" },
        [pscustomobject]@{ Id="overview-battery-current"; Title="电池总电流"; Index=1567; Unit="A" },
        [pscustomobject]@{ Id="overview-grid-frequency"; Title="电网频率"; Index=1052; Unit="Hz" },
        [pscustomobject]@{ Id="overview-cabinet-temperature"; Title="柜内温度"; Index=$dehumidifierPoints.cabinetTemperature; Unit="℃"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="overview-cabinet-humidity"; Title="柜内湿度"; Index=$dehumidifierPoints.cabinetHumidity; Unit="%RH"; Accent="#F9CC44" }
    )
    Add-CompactMetricGrid $widgets $details 3 48 578 1196 424

    $widgets.Add((New-Frame "overview-health-panel" 1284 290 612 400 "#081E29" 0))
    $widgets.Add((New-Text "overview-health-title" "设备健康" 1308 310 300 34 24 "#EAF6FA" "Left" $true 3))
    $health = @(
        @{ Id="pcs"; T="PCS 通讯"; I=1399 }, @{ Id="bms"; T="BMS 通讯"; I=3999 },
        @{ Id="liquid"; T="液冷机"; I=127 }, @{ Id="dehumidifier"; T="除湿机"; I=188 },
        @{ Id="fire"; T="消防探测器"; I=310000 }
    )
    for ($i=0; $i -lt $health.Count; $i++) {
        $item = $health[$i]
        Add-CompactBinaryCard $widgets "overview-health-$($item.Id)" $item.T $item.I "1" "在线" "离线" `
            (1308 + ($i % 2) * 282) (360 + [math]::Floor($i / 2) * 104) 266 92
    }
    $widgets.Add((New-CellularSignalCard "overview-cellular-signal" 1590 568 282 92))
    $widgets.Add((New-Widget "overview-active-alarms" "alarmTable" "活动设备告警" 1284 706 612 322 2 @() @() $null ([ordered]@{
        qtBackgroundColor = "#071A2D"; qtTextColor = "#E8F0F2"; qtFontSize = 18
    })))
    return New-Screen "Overview" "总览" $widgets
}

function Build-CompactStrategyOverviewScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Strategy"
    Add-CompactTabs $widgets @(
        @{ Id="Overview"; Text="执行总览"; Target="Strategy-Overview" },
        @{ Id="Charge"; Text="充放电"; Target="Strategy-Charge" },
        @{ Id="Grid"; Text="电网友好"; Target="Strategy-Grid" },
        @{ Id="Balance"; Text="三相与无功"; Target="Strategy-Balance" }
    ) "Overview"
    $widgets.Add((New-Frame "strategy-chart-panel" 24 206 1190 822 "#081E29" 0))
    $widgets.Add((New-Text "strategy-chart-title" "计划与实际执行 · 最近 1 小时" 48 224 600 34 24 "#EAF6FA" "Left" $true 3))
    $seriesOptions = @(
        [pscustomobject]@{ PenStyle="DashLine"; Scale=1.0; LineWidth=4.0; RenderLayer=0.35; Indexes=@([uint32]627,[uint32]628,[uint32]629); Aggregation="sum" },
        [pscustomobject]@{ PenStyle="solid"; Scale=-1.0; LineWidth=2.5; RenderLayer=0.15 }
    )
    $widgets.Add((New-Chart -Id "strategy-execution-chart" -Title "计划与实际执行" `
        -Indexes ([uint32[]]@(627,1230)) -Names ([string[]]@("仲裁后计划","PCS 实际")) `
        -Units ([string[]]@("kW","kW")) -Colors ([string[]]@("#55D8E8","#55E0AA")) `
        -X 38 -Y 272 -Width 1162 -Height 734 -SeriesOptions $seriesOptions `
        -DefaultWindowMinutes 60 -SampleIntervalSeconds 5 -RenderIntervalMs 3000))

    $widgets.Add((New-Frame "strategy-current-panel" 1230 206 666 822 "#081E29" 0))
    $widgets.Add((New-Text "strategy-current-title" "当前决策" 1254 224 400 34 24 "#EAF6FA" "Left" $true 3))
    Add-CompactBinaryCard $widgets "strategy-current-schedule" "计划曲线" 18 "1" "运行" "待机" 1254 276 302 142 "#71808A"
    Add-CompactMultiCard $widgets "strategy-current-cycle" "充放电阶段" 17 (Compact-PhaseStates) 1570 276 302 142
    $current = @(
        [pscustomobject]@{ Id="strategy-current-plan"; Title="三相最终指令"; Index=627; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="strategy-current-actual"; Title="PCS 实际功率"; Index=1230; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="strategy-current-soc"; Title="实际 SOC"; Index=1569; Unit="%" },
        [pscustomobject]@{ Id="strategy-target-soc"; Title="目标 SOC"; Index=462; Unit="%"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="strategy-allow-charge"; Title="允许充电"; Index=1552; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="strategy-allow-discharge"; Title="允许放电"; Index=1553; Unit="kW"; Accent="#F9CC44" }
    )
    Add-CompactMetricGrid $widgets $current 2 1254 436 618 566
    return New-Screen "Strategy-Overview" "策略执行总览" $widgets
}

function Build-CompactStrategyChargeScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Strategy"
    Add-CompactTabs $widgets @(
        @{ Id="Overview"; Text="执行总览"; Target="Strategy-Overview" }, @{ Id="Charge"; Text="充放电"; Target="Strategy-Charge" },
        @{ Id="Grid"; Text="电网友好"; Target="Strategy-Grid" }, @{ Id="Balance"; Text="三相与无功"; Target="Strategy-Balance" }
    ) "Charge"
    $phaseStates = Compact-PhaseStates
    Add-CompactMultiCard $widgets "strategy-charge-cycle" "循环阶段" 17 $phaseStates 24 212 444 168
    Add-CompactBinaryCard $widgets "strategy-charge-manual-charge" "手动充电" 14 "1" "运行" "待机" 484 212 444 168 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-charge-manual-discharge" "手动放电" 16 "1" "运行" "待机" 944 212 444 168 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-charge-schedule" "计划曲线" 18 "1" "运行" "待机" 1404 212 492 168 "#71808A"
    $items = @(
        [pscustomobject]@{ Id="charge-soc"; Title="实际 SOC"; Index=1569; Unit="%"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="charge-target-soc"; Title="目标 SOC"; Index=462; Unit="%"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="charge-manual-in"; Title="手动充电候选"; Index=613; Unit="kW" },
        [pscustomobject]@{ Id="charge-manual-out"; Title="手动放电候选"; Index=614; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="charge-soc-min"; Title="SOC 下限"; Index=162; Unit="%" },
        [pscustomobject]@{ Id="charge-soc-max"; Title="SOC 上限"; Index=161; Unit="%" },
        [pscustomobject]@{ Id="charge-allowed"; Title="允许充电"; Index=1552; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="discharge-allowed"; Title="允许放电"; Index=1553; Unit="kW"; Accent="#F9CC44" }
    )
    Add-CompactMetricGrid $widgets $items 4 24 404 1872 598
    return New-Screen "Strategy-Charge" "充放电策略" $widgets
}

function Build-CompactStrategyGridScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Strategy"
    Add-CompactTabs $widgets @(
        @{ Id="Overview"; Text="执行总览"; Target="Strategy-Overview" }, @{ Id="Charge"; Text="充放电"; Target="Strategy-Charge" },
        @{ Id="Grid"; Text="电网友好"; Target="Strategy-Grid" }, @{ Id="Balance"; Text="三相与无功"; Target="Strategy-Balance" }
    ) "Grid"
    Add-CompactBinaryCard $widgets "strategy-grid-low" "低压治理" 10 "1" "介入" "未介入" 24 212 444 168 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-grid-high" "高压治理" 12 "1" "介入" "未介入" 484 212 444 168 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-grid-pv" "光伏优先" 22 "1" "介入" "未介入" 944 212 444 168 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-grid-capacity" "动态增容" 24 "1" "介入" "未介入" 1404 212 492 168 "#71808A"
    $items = @(
        [pscustomobject]@{ Id="grid-a-voltage"; Title="A 相电压"; Index=1220; Unit="V" },
        [pscustomobject]@{ Id="grid-b-voltage"; Title="B 相电压"; Index=1221; Unit="V" },
        [pscustomobject]@{ Id="grid-c-voltage"; Title="C 相电压"; Index=1222; Unit="V" },
        [pscustomobject]@{ Id="grid-power"; Title="并网功率"; Index=1039; Unit="kW" },
        [pscustomobject]@{ Id="grid-low-limit"; Title="低压下限"; Index=544; Unit="V" },
        [pscustomobject]@{ Id="grid-low-output"; Title="低压治理输出"; Index=608; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="grid-high-limit"; Title="高压上限"; Index=547; Unit="V" },
        [pscustomobject]@{ Id="grid-high-output"; Title="高压治理输出"; Index=612; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="grid-pv-output"; Title="光伏充电候选"; Index=622; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="grid-override"; Title="受控目标"; Index=590; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="grid-charge-limit"; Title="允许充电"; Index=1552; Unit="kW" },
        [pscustomobject]@{ Id="grid-discharge-limit"; Title="允许放电"; Index=1553; Unit="kW" }
    )
    Add-CompactMetricGrid $widgets $items 4 24 404 1872 598
    return New-Screen "Strategy-Grid" "电网友好策略" $widgets
}

function Build-CompactStrategyBalanceScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Strategy"
    Add-CompactTabs $widgets @(
        @{ Id="Overview"; Text="执行总览"; Target="Strategy-Overview" }, @{ Id="Charge"; Text="充放电"; Target="Strategy-Charge" },
        @{ Id="Grid"; Text="电网友好"; Target="Strategy-Grid" }, @{ Id="Balance"; Text="三相与无功"; Target="Strategy-Balance" }
    ) "Balance"
    Add-CompactBinaryCard $widgets "strategy-balance-status" "三相平衡" 20 "1" "介入" "未介入" 24 212 600 160 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-reactive-status" "无功补偿" 8 "1" "介入" "未介入" 640 212 600 160 "#71808A"
    Add-CompactBinaryCard $widgets "strategy-override-status" "受控模式" 26 "1" "介入" "未介入" 1256 212 640 160 "#71808A"
    $items = @(
        [pscustomobject]@{ Id="balance-load-a"; Title="A 相负荷"; Index=309; Unit="kW" },
        [pscustomobject]@{ Id="balance-command-a"; Title="A 相最终指令"; Index=627; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="balance-actual-a"; Title="A 相 PCS 实际"; Index=1227; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="balance-load-b"; Title="B 相负荷"; Index=310; Unit="kW" },
        [pscustomobject]@{ Id="balance-command-b"; Title="B 相最终指令"; Index=628; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="balance-actual-b"; Title="B 相 PCS 实际"; Index=1228; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="balance-load-c"; Title="C 相负荷"; Index=311; Unit="kW" },
        [pscustomobject]@{ Id="balance-command-c"; Title="C 相最终指令"; Index=629; Unit="kW"; Accent="#F9CC44" },
        [pscustomobject]@{ Id="balance-actual-c"; Title="C 相 PCS 实际"; Index=1229; Unit="kW"; Accent="#55E0AA" },
        [pscustomobject]@{ Id="reactive-target-pf"; Title="目标功率因数"; Index=514; Unit="" },
        [pscustomobject]@{ Id="reactive-actual-pf"; Title="实际功率因数"; Index=324; Unit="" },
        [pscustomobject]@{ Id="reactive-output"; Title="无功补偿输出"; Index=604; Unit="kvar"; Accent="#55D8E8" }
    )
    Add-CompactMetricGrid $widgets $items 3 24 396 1872 606
    return New-Screen "Strategy-Balance" "三相平衡与无功" $widgets
}

function Build-CompactDeviceScreen([string]$Kind) {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Devices"
    Add-CompactTabs $widgets @(
        @{ Id="Pcs"; Text="PCS"; Target="Devices-Pcs" }, @{ Id="Bms"; Text="BMS"; Target="Devices-Bms" },
        @{ Id="Thermal"; Text="热管理"; Target="Devices-Thermal" }, @{ Id="Fire"; Text="消防与 IO"; Target="Devices-Fire" }
    ) $Kind
    $items = @()
    switch ($Kind) {
        "Pcs" {
            Add-CompactBinaryCard $widgets "device-pcs-run" "运行状态" 1211 "1" "运行" "停机" 24 210 452 174 "#D9A441"
            Add-CompactBinaryCard $widgets "device-pcs-fault" "总故障" 1212 "0" "正常" "故障" 492 210 452 174
            Add-CompactBinaryCard $widgets "device-pcs-alarm" "总报警" 1213 "0" "正常" "报警" 960 210 452 174 "#D9A441"
            Add-CompactBinaryCard $widgets "device-pcs-online" "通讯状态" 1399 "1" "在线" "离线" 1428 210 468 174
            $items = @(
                [pscustomobject]@{Id="pcs-p";Title="总有功";Index=1230;Unit="kW"},[pscustomobject]@{Id="pcs-q";Title="总无功";Index=1234;Unit="kvar"},
                [pscustomobject]@{Id="pcs-va";Title="A 相电压";Index=1220;Unit="V"},[pscustomobject]@{Id="pcs-ia";Title="A 相电流";Index=1223;Unit="A"},
                [pscustomobject]@{Id="pcs-vb";Title="B 相电压";Index=1221;Unit="V"},[pscustomobject]@{Id="pcs-ib";Title="B 相电流";Index=1224;Unit="A"},
                [pscustomobject]@{Id="pcs-vc";Title="C 相电压";Index=1222;Unit="V"},[pscustomobject]@{Id="pcs-ic";Title="C 相电流";Index=1225;Unit="A"},
                [pscustomobject]@{Id="pcs-dcv";Title="直流电压";Index=1244;Unit="V"},[pscustomobject]@{Id="pcs-dci";Title="直流电流";Index=1245;Unit="A"},
                [pscustomobject]@{Id="pcs-freq";Title="电网频率";Index=1226;Unit="Hz"},[pscustomobject]@{Id="pcs-dcp";Title="直流功率";Index=1243;Unit="kW"}
            )
            Add-CompactMetricGrid $widgets $items 4 24 404 1872 598
        }
        "Bms" {
            Add-CompactMultiCard $widgets "device-bms-state" "运行状态" 1550 @(
                @{Code="ready";Label="正常";Color="#20C879";Value="0"},@{Code="charge-disabled";Label="禁充";Color="#D9A441";Value="1"},
                @{Code="discharge-disabled";Label="禁放";Color="#D9A441";Value="2"},@{Code="standby";Label="待机";Color="#78B6CC";Value="3"},
                @{Code="stopped";Label="停机";Color="#8A9AA2";Value="4"}
            ) 24 210 452 174
            Add-CompactBinaryCard $widgets "device-bms-online" "通讯状态" 3999 "1" "在线" "离线" 492 210 452 174
            Add-CompactBinaryCard $widgets "device-bms-fault" "BMS 故障" 1462 "0" "正常" "故障" 960 210 452 174
            Add-CompactMetricCard $widgets "device-bms-soc-top" "电池 SOC" 1569 1428 210 468 174 "%" "#55E0AA"
            $items = @(
                [pscustomobject]@{Id="bms-soh";Title="SOH";Index=1570;Unit="%"},[pscustomobject]@{Id="bms-voltage";Title="总电压";Index=1566;Unit="V"},
                [pscustomobject]@{Id="bms-current";Title="总电流";Index=1567;Unit="A"},[pscustomobject]@{Id="bms-temperature";Title="平均温度";Index=1573;Unit="℃"},
                [pscustomobject]@{Id="bms-cell-vmax";Title="最高单体电压";Index=1574;Unit="mV"},[pscustomobject]@{Id="bms-cell-vmin";Title="最低单体电压";Index=1577;Unit="mV"},
                [pscustomobject]@{Id="bms-cell-tmax";Title="最高单体温度";Index=1580;Unit="℃"},[pscustomobject]@{Id="bms-cell-tmin";Title="最低单体温度";Index=1583;Unit="℃"},
                [pscustomobject]@{Id="bms-charge";Title="可充电量";Index=1590;Unit="kWh"},[pscustomobject]@{Id="bms-discharge";Title="可放电量";Index=1591;Unit="kWh"}
            )
            Add-CompactMetricGrid $widgets $items 5 24 404 1872 598
        }
        "Thermal" {
            Add-CompactBinaryCard $widgets "device-liquid-online" "液冷通讯" 127 "1" "在线" "离线" 24 210 452 174
            Add-CompactBinaryCard $widgets "device-dehumidifier-online" "除湿通讯" 188 "1" "在线" "离线" 492 210 452 174
            if ($null -ne $dehumidifierPoints.workState) { Add-CompactBinaryCard $widgets "device-dehumidifier-state" "除湿状态" $dehumidifierPoints.workState "1" "运行" "停止" 960 210 452 174 "#71808A" }
            if ($null -ne $dehumidifierPoints.fault) { Add-CompactBinaryCard $widgets "device-dehumidifier-fault" "除湿故障" $dehumidifierPoints.fault "0" "正常" "故障" 1428 210 468 174 }
            $items = @(
                [pscustomobject]@{Id="thermal-out";Title="液冷出水温度";Index=131;Unit="℃"},[pscustomobject]@{Id="thermal-return";Title="液冷回水温度";Index=132;Unit="℃"},
                [pscustomobject]@{Id="thermal-ambient";Title="液冷环境温度";Index=135;Unit="℃"},[pscustomobject]@{Id="thermal-out-pressure";Title="出水压力";Index=141;Unit="Bar"},
                [pscustomobject]@{Id="thermal-return-pressure";Title="回水压力";Index=142;Unit="Bar"},[pscustomobject]@{Id="thermal-fault";Title="液冷故障码";Index=153;Unit="";Accent="#F9CC44"},
                [pscustomobject]@{Id="thermal-cabinet-temp";Title="柜内温度（除湿）";Index=$dehumidifierPoints.cabinetTemperature;Unit="℃"},
                [pscustomobject]@{Id="thermal-cabinet-humidity";Title="柜内湿度（除湿）";Index=$dehumidifierPoints.cabinetHumidity;Unit="%RH"},
                [pscustomobject]@{Id="thermal-inner-temp";Title="除湿机内部温度";Index=$dehumidifierPoints.internalTemperature;Unit="℃"}
            )
            Add-CompactMetricGrid $widgets $items 3 24 404 1872 598
        }
        "Fire" {
            Add-CompactBinaryCard $widgets "device-fire-online" "消防通讯" 310000 "1" "在线" "离线" 24 210 452 174
            Add-CompactBinaryCard $widgets "device-fire-alarm" "消防综合报警" 310012 "0" "正常" "报警" 492 210 452 174
            Add-CompactBinaryCard $widgets "device-fire-fault" "消防综合故障" 310013 "0" "正常" "故障" 960 210 452 174
            if ($null -ne $dioPoints.emergency) { Add-CompactBinaryCard $widgets "device-emergency" "急停回路" $dioPoints.emergency (Normal-ValueFor $dioPoints.emergency "0") "正常" "动作" 1428 210 468 174 }
            $items = @(
                [pscustomobject]@{Id="fire-temperature";Title="探测温度";Index=310101;Unit="℃"},[pscustomobject]@{Id="fire-smoke";Title="烟雾减光率";Index=310102;Unit="%"},
                [pscustomobject]@{Id="fire-co";Title="CO";Index=310103;Unit="ppm"},[pscustomobject]@{Id="fire-h2";Title="H2";Index=310104;Unit="ppm"},
                [pscustomobject]@{Id="fire-voc";Title="VOC";Index=310105;Unit="ppm"}
            )
            Add-CompactMetricGrid $widgets $items 5 24 404 1872 248
            $io = @()
            if ($null -ne $dioPoints.surge) { $io += @{Id="surge";T="浪涌保护";I=$dioPoints.surge;N=(Normal-ValueFor $dioPoints.surge "0");G="正常";B="动作"} }
            if ($null -ne $dioPoints.water) { $io += @{Id="water";T="水浸";I=$dioPoints.water;N=(Normal-ValueFor $dioPoints.water "0");G="正常";B="动作"} }
            if ($null -ne $dioPoints.frontDoor) { $io += @{Id="front";T="前门";I=$dioPoints.frontDoor;N=(Normal-ValueFor $dioPoints.frontDoor "0");G="关闭";B="打开"} }
            if ($null -ne $dioPoints.rearDoor) { $io += @{Id="rear";T="后门";I=$dioPoints.rearDoor;N=(Normal-ValueFor $dioPoints.rearDoor "0");G="关闭";B="打开"} }
            if ($null -ne $dioPoints.storageBreaker) { $io += @{Id="storage-breaker";T="储能断路器";I=$dioPoints.storageBreaker;N="1";G="合位";B="分位"} }
            if ($null -ne $dioPoints.gridBreaker) { $io += @{Id="grid-breaker";T="并网断路器";I=$dioPoints.gridBreaker;N="1";G="合位";B="分位"} }
            for ($i=0; $i -lt $io.Count; $i++) {
                $item=$io[$i]; Add-CompactBinaryCard $widgets "device-io-$($item.Id)" $item.T $item.I $item.N $item.G $item.B `
                    (24 + ($i % 3) * 629.3) (672 + [math]::Floor($i / 3) * 166) 613.3 150 "#D9A441"
            }
        }
    }
    return New-Screen "Devices-$Kind" "设备 - $Kind" $widgets
}

function Build-CompactAlarmsScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Alarms"
    $summary = @(
        @{Id="pcs";T="PCS 故障";I=1212;N="0";G="正常";B="故障"},@{Id="bms";T="BMS 故障";I=1462;N="0";G="正常";B="故障"},
        @{Id="liquid";T="液冷故障";I=153;N="0";G="正常";B="故障"},@{Id="fire";T="消防报警";I=310012;N="0";G="正常";B="报警"}
    )
    for($i=0;$i -lt $summary.Count;$i++){$item=$summary[$i];Add-CompactBinaryCard $widgets "alarm-$($item.Id)" $item.T $item.I $item.N $item.G $item.B (24+$i*468) 126 452 148}
    $widgets.Add((New-Widget "alarms-device-table" "alarmTable" "活动设备告警" 24 294 1872 734 2 @() @() $null ([ordered]@{
        qtBackgroundColor="#071A2D";qtTextColor="#E8F0F2";qtFontSize=20
    })))
    return New-Screen "Alarms" "告警" $widgets
}

function Build-CompactTrendScreen([string]$Kind) {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Trends"
    Add-CompactTabs $widgets @(
        @{Id="Power";Text="三相功率";Target="Trends-Power"},@{Id="Grid";Text="并网功率";Target="Trends-Grid"},
        @{Id="Battery";Text="电池状态";Target="Trends-Battery"},@{Id="Thermal";Text="温度";Target="Trends-Thermal"}
    ) $Kind
    $definition = switch($Kind){
        "Power" {@{Title="PCS 三相有功功率";Indexes=[uint32[]]@(1227,1228,1229,1230);Names=[string[]]@("A 相","B 相","C 相","合相");Units=[string[]]@("kW","kW","kW","kW");Colors=[string[]]@("#55D8E8","#55E0AA","#F9CC44","#F3F8FA")}}
        "Grid" {@{Title="并网与储能侧功率";Indexes=[uint32[]]@(1039,1139);Names=[string[]]@("并网侧","储能侧");Units=[string[]]@("kW","kW");Colors=[string[]]@("#55D8E8","#F9CC44")}}
        "Battery" {@{Title="电池 SOC / SOH";Indexes=[uint32[]]@(1569,1570);Names=[string[]]@("SOC","SOH");Units=[string[]]@("%","%");Colors=[string[]]@("#55E0AA","#55D8E8")}}
        "Thermal" {
            $indexes=[System.Collections.Generic.List[uint32]]::new();$names=[System.Collections.Generic.List[string]]::new();$units=[System.Collections.Generic.List[string]]::new();$colors=[System.Collections.Generic.List[string]]::new()
            foreach($item in @(@{I=131;N="液冷出水";C="#55D8E8"},@{I=132;N="液冷回水";C="#F9CC44"},@{I=$dehumidifierPoints.cabinetTemperature;N="柜内温度";C="#55E0AA"})){
                if($null -ne $item.I -and (Has-Index ([uint32]$item.I))){$indexes.Add([uint32]$item.I);$names.Add($item.N);$units.Add("℃");$colors.Add($item.C)}
            }
            @{Title="热管理温度";Indexes=[uint32[]]@($indexes);Names=[string[]]@($names);Units=[string[]]@($units);Colors=[string[]]@($colors)}
        }
    }
    $widgets.Add((New-Frame "trend-panel" 24 206 1872 822 "#081E29" 0))
    $widgets.Add((New-Text "trend-title" "$($definition.Title) · 最近 1 小时" 48 224 900 34 24 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Chart "trend-chart" $definition.Title $definition.Indexes $definition.Names $definition.Units $definition.Colors 36 270 1848 736 @() 60 1 1000))
    return New-Screen "Trends-$Kind" "趋势 - $Kind" $widgets
}

function Build-CompactControlPcsScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Control"
    Add-CompactTabs $widgets @(@{Id="Pcs";Text="PCS 控制";Target="Control-Pcs"},@{Id="Dido";Text="DI / DO";Target="Control-Dido"}) "Pcs"
    $checks=@(@{Id="online";T="PCS 通讯";I=1399;N="1";G="在线";B="离线"},@{Id="fault";T="PCS 故障";I=1212;N="0";G="正常";B="故障"},@{Id="bms";T="BMS 故障";I=1462;N="0";G="正常";B="故障"})
    for($i=0;$i -lt $checks.Count;$i++){$it=$checks[$i];Add-CompactBinaryCard $widgets "control-check-$($it.Id)" $it.T $it.I $it.N $it.G $it.B (24+$i*468) 210 452 142}
    $widgets.Add((New-ControlButton "control-pcs-start" "启动" 1201 "pulse" "1" 1428 210 218 142 $false "#124536"))
    $widgets.Add((New-ControlButton "control-pcs-stop" "停机" 1202 "pulse" "1" 1662 210 234 142 $true "#4B272B"))
    $bindings=@((New-Binding 1318 "activeA"),(New-Binding 1319 "activeB"),(New-Binding 1320 "activeC"),(New-Binding 1321 "reactiveA"),(New-Binding 1322 "reactiveB"),(New-Binding 1323 "reactiveC"))
    $widgets.Add((New-Widget "control-pcs-phase-power" "pcsPhasePowerControl" "PCS 三相功率设定" 24 376 1872 626 5 $bindings @() $null ([ordered]@{})))
    return New-Screen "Control-Pcs" "PCS 控制" $widgets
}

function Build-CompactControlDidoScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Control"
    Add-CompactTabs $widgets @(@{Id="Pcs";Text="PCS 控制";Target="Control-Pcs"},@{Id="Dido";Text="DI / DO";Target="Control-Dido"}) "Dido"
    $widgets.Add((New-Text "control-di-title" "DI 只读反馈" 24 212 600 34 24 "#EAF6FA" "Left" $true 3))
    $di=@()
    if($null -ne $dioPoints.emergency){$di+=@{Id="emergency";T="急停";I=$dioPoints.emergency;N=(Normal-ValueFor $dioPoints.emergency "0");G="正常";B="动作"}}
    if($null -ne $dioPoints.surge){$di+=@{Id="surge";T="浪涌保护";I=$dioPoints.surge;N=(Normal-ValueFor $dioPoints.surge "0");G="正常";B="动作"}}
    if($null -ne $dioPoints.frontDoor){$di+=@{Id="front";T="前门";I=$dioPoints.frontDoor;N=(Normal-ValueFor $dioPoints.frontDoor "0");G="关闭";B="打开"}}
    if($null -ne $dioPoints.rearDoor){$di+=@{Id="rear";T="后门";I=$dioPoints.rearDoor;N=(Normal-ValueFor $dioPoints.rearDoor "0");G="关闭";B="打开"}}
    if($null -ne $dioPoints.gridBreaker){$di+=@{Id="grid-breaker";T="并网断路器";I=$dioPoints.gridBreaker;N="1";G="合位";B="分位"}}
    for($i=0;$i -lt $di.Count;$i++){$it=$di[$i];Add-CompactBinaryCard $widgets "control-di-$($it.Id)" $it.T $it.I $it.N $it.G $it.B (24+($i%2)*452) (264+[math]::Floor($i/2)*190) 436 174 "#D9A441"}
    $widgets.Add((New-Frame "control-do-panel" 960 206 936 822 "#081E29" 0));$widgets.Add((New-Text "control-do-title" "DO 输出" 988 224 600 34 24 "#EAF6FA" "Left" $true 3))
    $controls=@()
    if($null -ne $dioPoints.fan){$controls+=@{Id="fan-on";T="风机启动";I=$dioPoints.fan;V="1";C="#124536"},@{Id="fan-off";T="风机停止";I=$dioPoints.fan;V="0";C="#273C46"}}
    if($null -ne $dioPoints.runLamp){$controls+=@{Id="run-on";T="运行灯点亮";I=$dioPoints.runLamp;V="1";C="#124536"},@{Id="run-off";T="运行灯熄灭";I=$dioPoints.runLamp;V="0";C="#273C46"}}
    if($null -ne $dioPoints.chargeLamp){$controls+=@{Id="charge-on";T="充电灯点亮";I=$dioPoints.chargeLamp;V="1";C="#124536"},@{Id="charge-off";T="充电灯熄灭";I=$dioPoints.chargeLamp;V="0";C="#273C46"}}
    if($null -ne $dioPoints.dischargeLamp){$controls+=@{Id="discharge-on";T="放电灯点亮";I=$dioPoints.dischargeLamp;V="1";C="#124536"},@{Id="discharge-off";T="放电灯熄灭";I=$dioPoints.dischargeLamp;V="0";C="#273C46"}}
    for($i=0;$i -lt $controls.Count;$i++){$it=$controls[$i];$widgets.Add((New-ControlButton "control-do-$($it.Id)" $it.T $it.I "writeSetpoint" $it.V (988+($i%2)*440) (286+[math]::Floor($i/2)*156) 420 132 $false $it.C))}
    return New-Screen "Control-Dido" "DI / DO 控制" $widgets
}

function Build-CompactMaintenanceScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Maintenance"
    $widgets.Add((New-Text "maintenance-title" "运行与工程信息" 24 126 700 40 26 "#EAF6FA" "Left" $true 3))
    $comms=@(@{Id="pcs";T="PCS";I=1399},@{Id="bms";T="BMS";I=3999},@{Id="liquid";T="液冷机";I=127},@{Id="dehumidifier";T="除湿机";I=188},@{Id="fire";T="消防探测器";I=310000})
    for($i=0;$i -lt $comms.Count;$i++){$it=$comms[$i];Add-CompactBinaryCard $widgets "maintenance-$($it.Id)" "$($it.T) 通讯" $it.I "1" "在线" "离线" (24+($i%3)*624) (190+[math]::Floor($i/3)*190) 608 174}
    $widgets.Add((New-CellularSignalCard "maintenance-cellular-quick" 1272 380 608 174))

    $widgets.Add((New-Frame "maintenance-info-panel" 24 590 610 438 "#081E29" 0))
    $widgets.Add((New-Text "maintenance-info-title" "工程" 48 610 260 34 24 "#EAF6FA" "Left" $true 3))
    $info=@(@{L="工程名称";V=$DisplayName},@{L="工程版本";V=$PackageVersion},@{L="Machine Code";V=$MachineCode},@{L="运行拓扑";V="边端一体化"},@{L="数据源";V="共享内存直读"})
    for($i=0;$i -lt $info.Count;$i++){$y=658+$i*68;$widgets.Add((New-Text "maintenance-label-$i" $info[$i].L 48 $y 172 30 18 "#78B6CC" "Left" $false 3));$widgets.Add((New-Text "maintenance-value-$i" $info[$i].V 224 $y 382 32 20 "#F3F8FA" "Right" $true 3))}

    $widgets.Add((New-Frame "maintenance-cellular-panel" 650 590 1246 438 "#081E29" 0))
    $widgets.Add((New-Text "maintenance-cellular-title" "4G 网络与流量" 674 610 420 34 24 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Frame "maintenance-cellular-link-frame" 674 650 280 140 "#0B2633" 0))
    $widgets.Add((New-Text "maintenance-cellular-link-title" "4G 链路" 694 666 240 28 20 "#9FC0CC" "Left" $false 4))
    $widgets.Add((New-CellularLinkStatus "maintenance-cellular-link" 686 696 256 80))
    Add-CompactBinaryCard $widgets "maintenance-cellular-route" "当前出口" 920000004 "1" "4G 出口" "有线出口" 970 650 280 140 "#55D8E8"
    $widgets.Add((New-CellularSignalCard "maintenance-cellular-signal" 1266 650 606 140))
    Add-CompactMetricCard $widgets "maintenance-cellular-rx-total" "本次连接下载" 920000006 674 806 286 194 "MiB" "#55D8E8"
    Add-CompactMetricCard $widgets "maintenance-cellular-tx-total" "本次连接上传" 920000007 974 806 286 194 "MiB" "#F9CC44"
    Add-CompactMetricCard $widgets "maintenance-cellular-rx-rate" "实时下行" 920000008 1274 806 286 194 "KiB/s" "#55E0AA"
    Add-CompactMetricCard $widgets "maintenance-cellular-tx-rate" "实时上行" 920000009 1574 806 298 194 "KiB/s" "#D9A441"
    return New-Screen "Maintenance" "运维" $widgets
}

$alarmSpecs = @(
    @{ I=153; C="ne"; V="0"; S="critical" }, @{ I=199; C="ne"; V="0"; S="warning" },
    @{ I=209; C="ne"; V="0"; S="warning" }, @{ I=210; C="ne"; V="0"; S="warning" }, @{ I=211; C="ne"; V="0"; S="warning" },
    @{ I=1212; C="ne"; V="0"; S="critical" }, @{ I=1213; C="ne"; V="0"; S="warning" },
    @{ I=1261; C="ne"; V="0"; S="critical" }, @{ I=1262; C="ne"; V="0"; S="critical" },
    @{ I=1263; C="ne"; V="0"; S="critical" }, @{ I=1264; C="ne"; V="0"; S="critical" },
    @{ I=1265; C="ne"; V="0"; S="critical" }, @{ I=1266; C="ne"; V="0"; S="critical" },
    @{ I=1267; C="ne"; V="0"; S="critical" }, @{ I=1268; C="ne"; V="0"; S="critical" },
    @{ I=1269; C="ne"; V="0"; S="critical" }, @{ I=1397; C="ne"; V="0"; S="warning" },
    @{ I=1398; C="ne"; V="0"; S="critical" }, @{ I=1462; C="ne"; V="0"; S="critical" },
    @{ I=8197; C="ne"; V="0"; S="warning" }, @{ I=8198; C="ne"; V="0"; S="warning" }, @{ I=8199; C="ne"; V="0"; S="critical" },
    @{ I=310012; C="ne"; V="0"; S="critical" }, @{ I=310013; C="ne"; V="0"; S="critical" },
    @{ I=310020; C="ne"; V="0"; S="warning" }, @{ I=310021; C="ne"; V="0"; S="warning" },
    @{ I=310022; C="ne"; V="0"; S="warning" }, @{ I=310023; C="ne"; V="0"; S="warning" },
    @{ I=310024; C="ne"; V="0"; S="warning" }, @{ I=310026; C="ne"; V="0"; S="critical" },
    @{ I=310027; C="ne"; V="0"; S="critical" }, @{ I=310030; C="ne"; V="0"; S="warning" },
    @{ I=310031; C="ne"; V="0"; S="warning" }, @{ I=310032; C="ne"; V="0"; S="warning" },
    @{ I=310033; C="ne"; V="0"; S="warning" }, @{ I=310034; C="ne"; V="0"; S="warning" },
    @{ I=310035; C="ne"; V="0"; S="critical" }
)
if ($null -ne $dioPoints.emergency) { $alarmSpecs += @{ I=$dioPoints.emergency; C="ne"; V=(Normal-ValueFor $dioPoints.emergency "0"); S="critical" } }
if ($null -ne $dioPoints.surge) { $alarmSpecs += @{ I=$dioPoints.surge; C="ne"; V=(Normal-ValueFor $dioPoints.surge "0"); S="critical" } }
if ($null -ne $dioPoints.water) { $alarmSpecs += @{ I=$dioPoints.water; C="ne"; V=(Normal-ValueFor $dioPoints.water "0"); S="critical" } }
if ($null -ne $dioPoints.fireAction) { $alarmSpecs += @{ I=$dioPoints.fireAction; C="ne"; V=(Normal-ValueFor $dioPoints.fireAction "0"); S="critical" } }
$alarms = [System.Collections.Generic.List[object]]::new()
foreach ($spec in $alarmSpecs) {
    if (-not (Has-Index ([uint32]$spec.I))) { continue }
    $tag = Tag-At $spec.I
    $alarms.Add([pscustomobject][ordered]@{
        alarmId = "device-$($spec.I)"
        nodeId = $NodeId
        tagId = [string]$tag.tagId
        severity = $spec.S
        comparison = $spec.C
        threshold = $spec.V
        delayMs = 0
        deadband = 0
        requiresAcknowledgement = $spec.S -eq "critical"
    })
}

$trendSpecs = @(
    @{ Id="pcs-active-power"; Indexes=@(1227,1228,1229,1230) },
    @{ Id="grid-storage-power"; Indexes=@(1039,1139) },
    @{ Id="battery-health"; Indexes=@(1569,1570) },
    @{ Id="battery-available-energy"; Indexes=@(1590,1591) }
)
$trends = [System.Collections.Generic.List[object]]::new()
foreach($spec in $trendSpecs) {
    $series = [System.Collections.Generic.List[object]]::new()
    foreach($index in $spec.Indexes) { $series.Add((New-Binding $index)) }
    $trends.Add([pscustomobject][ordered]@{
        trendId = $spec.Id
        sampleIntervalMs = 1000
        maxPoints = 86402
        series = @($series)
    })
}

$screens = @(
    (Build-CompactOverviewScreen),
    (Build-CompactStrategyOverviewScreen),
    (Build-CompactStrategyChargeScreen),
    (Build-CompactStrategyGridScreen),
    (Build-CompactStrategyBalanceScreen),
    (Build-CompactDeviceScreen "Pcs"),
    (Build-CompactDeviceScreen "Bms"),
    (Build-CompactDeviceScreen "Thermal"),
    (Build-CompactDeviceScreen "Fire"),
    (Build-CompactAlarmsScreen),
    (Build-CompactTrendScreen "Power"),
    (Build-CompactTrendScreen "Grid"),
    (Build-CompactTrendScreen "Battery"),
    (Build-CompactTrendScreen "Thermal"),
    (Build-CompactControlPcsScreen),
    (Build-CompactControlDidoScreen),
    (Build-CompactMaintenanceScreen)
)

[void](New-Item -ItemType Directory -Path $outputRoot -Force)
$screensDirectory = Join-Path $outputRoot "screens"
[void](New-Item -ItemType Directory -Path $screensDirectory -Force)
$resolvedScreensDirectory = (Resolve-Path -LiteralPath $screensDirectory).Path
if (-not $resolvedScreensDirectory.StartsWith($outputRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing to replace screens outside the requested output project"
}
foreach ($oldScreen in Get-ChildItem -LiteralPath $resolvedScreensDirectory -Filter "*.json" -File) {
    Remove-Item -LiteralPath $oldScreen.FullName -Force
}

$manifest = [pscustomobject][ordered]@{
    schemaVersion = "2.0"
    projectId = "ky-mobile-ems-$MachineCode"
    projectName = "$DisplayName EMS 2.0 紧凑版"
    packageVersion = $PackageVersion
    productVersion = "2.0"
    createdAt = [DateTimeOffset]::UtcNow.ToString("o")
    createdBy = "Gateway EMS 2.0 compact generator"
    entryScreen = "Overview"
    packageRole = "project"
}
$topology = [pscustomobject][ordered]@{
    mode = "integrated"
    scadaHost = "edge"
    emsHost = "edge"
    dataTransport = "sharedMemory"
    offlinePolicy = "continueLocal"
    upperComputerOfflinePolicy = [pscustomobject][ordered]@{
        timeoutMs = 10000
        action = "executeConfiguredActions"
        retainLocalSafetyRules = $true
        requireFreshLeaseForControl = $true
        safetyActions = @()
    }
}
$nodes = @([pscustomobject][ordered]@{
    nodeId = $NodeId
    machineCode = $MachineCode
    displayName = $DisplayName
    connectionProfileId = ""
    fallbackConnectionProfileId = ""
    connectionPolicy = "auto"
    roles = @("acquisition", "control", "safety")
})

Write-Json (Join-Path $outputRoot "manifest.json") $manifest
Write-Json (Join-Path $outputRoot "topology.json") $topology
Write-Json (Join-Path $outputRoot "nodes.json") $nodes
Write-Json (Join-Path $outputRoot "tags.json") $tags
Write-Json (Join-Path $outputRoot "runtime-map.json") $runtimeMappings
Write-Json (Join-Path $outputRoot "alarms.json") @($alarms)
Write-Json (Join-Path $outputRoot "trends.json") @($trends)
Write-Json (Join-Path $outputRoot "symbols.json") @()
$localUser = New-LocalPasswordRecord $LocalOperatorUsername $LocalOperatorPassword
Write-Json (Join-Path $outputRoot "permissions.json") ([pscustomobject][ordered]@{
    roles = @("operator", "maintainer")
    localAccess = [pscustomobject][ordered]@{
        sessionTimeoutSeconds = $LocalSessionTimeoutSeconds
        protectedScreenPrefixes = @("Strategy-", "Control-")
        users = @($localUser)
    }
})
foreach ($screen in $screens) {
    Write-Json (Join-Path $screensDirectory ($screen.screenId + ".json")) $screen
}

$expectedScreens = @(
    "Alarms", "Control-Dido", "Control-Pcs", "Devices-Bms", "Devices-Fire", "Devices-Pcs",
    "Devices-Thermal", "Maintenance", "Overview", "Strategy-Balance", "Strategy-Charge",
    "Strategy-Grid", "Strategy-Overview", "Trends-Battery", "Trends-Grid", "Trends-Power", "Trends-Thermal"
)
$actualScreens = @(Get-ChildItem -LiteralPath $screensDirectory -Filter "*.json" -File | ForEach-Object BaseName | Sort-Object)
if (@(Compare-Object ($expectedScreens | Sort-Object) $actualScreens).Count -ne 0) {
    throw "EMS 2.0 compact output does not contain the required 17 screens"
}
foreach ($screen in $screens) {
    if ($screen.width -ne 1920 -or $screen.height -ne 1080) { throw "screen is not 1920x1080: $($screen.screenId)" }
    $ids = @($screen.widgets | ForEach-Object widgetId)
    if (@($ids | Group-Object | Where-Object Count -gt 1).Count -gt 0) { throw "duplicate widget id: $($screen.screenId)" }
    $displayNameWidgets = @($screen.widgets | Where-Object {
        $null -ne $_.properties.PSObject.Properties["qtText"] -and
        [string]$_.properties.qtText -eq $DisplayName
    })
    if ($displayNameWidgets.Count -lt 1) {
        throw "screen does not contain the required display name: $($screen.screenId)"
    }
}

$overviewScreen = @($screens | Where-Object screenId -eq "Overview")
if ($overviewScreen.Count -ne 1) { throw "compact overview screen is missing" }
$energyFlows = @($overviewScreen[0].widgets | Where-Object type -eq "energyFlow")
if ($energyFlows.Count -ne 2) { throw "compact overview must contain two realtime energy-flow widgets" }
$overviewMetricValues = @($overviewScreen[0].widgets | Where-Object {
    $_.type -eq "qtValue" -and $_.widgetId -like "overview-*-value"
})
if ($overviewMetricValues.Count -lt 8 -or @($overviewMetricValues | Where-Object {
    [string]$_.properties.qtTextAlignment -ne "Center" -or [int]$_.properties.qtFontSize -lt 42
}).Count -gt 0) {
    throw "compact overview metric cards must use centered 42px-or-larger values"
}
$overviewStatusValues = @($overviewScreen[0].widgets | Where-Object {
    $_.type -eq "statusLamp" -and $_.widgetId -like "overview-*"
})
if ($overviewStatusValues.Count -lt 6 -or @($overviewStatusValues | Where-Object {
    [string]$_.properties.qtTextAlignment -ne "Center" -or [string]$_.properties.qtBorderless -ne "true"
}).Count -gt 0) {
    throw "compact overview status cards must be centered and borderless"
}

$strategyScreens = @($screens | Where-Object screenId -like "Strategy-*")
if ($strategyScreens.Count -ne 4) { throw "compact strategy area must contain four focused pages" }
$controlScreens = @($screens | Where-Object screenId -like "Control-*")
if ($controlScreens.Count -ne 2) { throw "compact control area must contain PCS and DI/DO pages" }
$pcsControl = @($screens | Where-Object screenId -eq "Control-Pcs")
if (@($pcsControl[0].widgets | Where-Object type -eq "pcsPhasePowerControl").Count -ne 1) {
    throw "compact PCS control page is missing the phase power controller"
}

foreach ($screen in $screens) {
    $isProtected = $screen.screenId -like "Strategy-*" -or $screen.screenId -like "Control-*"
    foreach ($widget in $screen.widgets) {
        $actionType = if ($null -ne $widget.action -and
            $null -ne $widget.action.PSObject.Properties["type"]) {
            [string]$widget.action.type
        } else {
            "none"
        }
        $isWriteWidget = $widget.type -eq "pcsPhasePowerControl" -or
            $actionType -in @("writeSetpoint", "pulse", "toggle")
        if ($isWriteWidget -and -not $isProtected) {
            throw "writable widget is outside a protected screen: $($screen.screenId)/$($widget.widgetId)"
        }
        if ($widget.type -eq "qtChart") {
            if ([int]$widget.properties.chartDefaultWindowMinutes -ne 60) {
                throw "compact trend must default to the latest hour: $($screen.screenId)/$($widget.widgetId)"
            }
        }
    }
}

$permissions = Read-Json (Join-Path $outputRoot "permissions.json")
if (@($permissions.localAccess.protectedScreenPrefixes) -join ',' -ne 'Strategy-,Control-') {
    throw "local access must protect all strategy and control pages"
}
if (@($permissions.localAccess.users).Count -ne 1 -or
    [string]$permissions.localAccess.users[0].passwordSha256 -notmatch '^[0-9a-f]{64}$' -or
    [string]$permissions.localAccess.users[0].salt -notmatch '^[0-9a-f]{32}$') {
    throw "local access credential record is incomplete"
}

$screenText = (Get-ChildItem -LiteralPath $screensDirectory -Filter "*.json" -File | ForEach-Object {
    Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8
}) -join "`n"
if ($screenText -match '(?i)\bUPS\b|不间断电源') { throw "EMS 2.0 screens still reference UPS" }
if ($screenText -match '通讯延迟') { throw "communication latency must not be presented as a device alarm" }
if ($screenText -match '目标\s*→\s*算法输出\s*→\s*PCS\s*下发') {
    throw "overview must not present concurrent EMS strategies as a serial algorithm flow"
}

$checksumTable = [ordered]@{}
$filesToChecksum = Get-ChildItem -LiteralPath $outputRoot -Recurse -File |
    Where-Object Name -ne "checksums.json" |
    Sort-Object FullName
foreach ($file in $filesToChecksum) {
    $relative = (Get-RelativePathCompat $outputRoot $file.FullName).Replace('\', '/')
    $checksumTable[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
Write-Json (Join-Path $outputRoot "checksums.json") ([pscustomobject]$checksumTable)

$packageDirectory = Split-Path -Parent $OutputPackage
[void](New-Item -ItemType Directory -Path $packageDirectory -Force)
if (Test-Path -LiteralPath $OutputPackage) { Remove-Item -LiteralPath $OutputPackage -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$packageStream = [System.IO.File]::Open(
    $OutputPackage,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::None)
$archive = $null
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $packageStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    foreach ($file in Get-ChildItem -LiteralPath $outputRoot -Recurse -File | Sort-Object FullName) {
        $entryName = (Get-RelativePathCompat $outputRoot $file.FullName).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.FullName,
            $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    if ($null -ne $archive) {
        $archive.Dispose()
    }
    $packageStream.Dispose()
}

$packageHash = (Get-FileHash -LiteralPath $OutputPackage -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "EMS 2.0 compact SCADA project generated"
Write-Host "  name: $DisplayName"
Write-Host "  machineCode: $MachineCode"
Write-Host "  tags/routes: $($tags.Count)/$($runtimeMappings.Count)"
Write-Host "  screens: $($screens.Count)"
Write-Host "  device alarms: $($alarms.Count)"
Write-Host "  project: $outputRoot"
Write-Host "  package: $OutputPackage"
Write-Host "  package sha256: $packageHash"
Write-Host "  protected screens: Strategy-*, Control-*"
Write-Host "  local user: $LocalOperatorUsername"
