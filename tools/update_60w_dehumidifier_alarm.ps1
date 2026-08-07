param(
    [Parameter(Mandatory = $true)]
    [string]$SourceGraph,

    [Parameter(Mandatory = $true)]
    [string]$SourceVirtualConfig,

    [Parameter(Mandatory = $true)]
    [string]$OutputGraph,

    [Parameter(Mandatory = $true)]
    [string]$OutputVirtualConfig,

    [int]$AlarmContactIndex = 0,
    [double]$AlarmContactNormalValue = 0
)

$ErrorActionPreference = "Stop"

$graph = Get-Content -Raw -LiteralPath $SourceGraph | ConvertFrom-Json
$virtualConfig = Get-Content -Raw -LiteralPath $SourceVirtualConfig | ConvertFrom-Json

function Set-ControlGateConditions {
    param(
        [Parameter(Mandatory = $true)]$Node,
        [Parameter(Mandatory = $true)][array]$Conditions
    )

    $Node.parameters.combine = "any"
    $Node.parameters.conditions = $Conditions
    $inputPorts = @()
    for ($i = 0; $i -lt $Conditions.Count; $i++) {
        $inputPorts += [ordered]@{
            id = "conditions_${i}_index"
            displayName = "index"
            direction = "input"
            valueType = "number"
            unit = ""
            required = $true
            runtimePath = "/conditions/$i/index"
            binding = [ordered]@{
                kind = "point"
                index = [int]$Conditions[$i].index
                pointCode = ""
                semanticRole = ""
                constant = $null
            }
        }
    }
    $outputPorts = @($Node.ports | Where-Object { $_.direction -eq "output" })
    if ($outputPorts.Count -ne 1) {
        throw "Expected one output port on node '$($Node.id)', found $($outputPorts.Count)."
    }
    $Node.ports = @($inputPorts) + @($outputPorts)
}

$nodes = @($graph.nodes | Where-Object {
    $_.id -in @("dehumidifier_fault_summary", "dehumidifier_alarm_summary")
})
if ($nodes.Count -ne 1) {
    throw "Expected one dehumidifier alarm summary node, found $($nodes.Count)."
}

$conditions = @(
    [ordered]@{ index = 188; operator = "ne"; value = 1.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 189; operator = "lt"; value = -20.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 189; operator = "gt"; value = 55.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 190; operator = "lt"; value = 0.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 190; operator = "gt"; value = 99.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 217; operator = "lt"; value = -20.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 217; operator = "gt"; value = 55.0; missingValue = 0; skipIfUnrouted = $true }
)
if ($AlarmContactIndex -gt 0) {
    $conditions += [ordered]@{
        index = $AlarmContactIndex
        operator = "ne"
        value = $AlarmContactNormalValue
        missingValue = 0
        skipIfUnrouted = $true
    }
}

$node = $nodes[0]
$outputIndex = [int]$node.parameters.outputIndex
$oldNodeId = [string]$node.id
$node.id = "dehumidifier_alarm_summary"
$node.displayName = "dehumidifier_alarm_summary"
Set-ControlGateConditions -Node $node -Conditions $conditions
foreach ($edge in @($graph.edges)) {
    if ($edge.fromNodeId -eq $oldNodeId) { $edge.fromNodeId = $node.id }
    if ($edge.toNodeId -eq $oldNodeId) { $edge.toNodeId = $node.id }
}
foreach ($link in @($graph.links)) {
    if ($link.fromNodeId -eq $oldNodeId) { $link.fromNodeId = $node.id }
    if ($link.toNodeId -eq $oldNodeId) { $link.toNodeId = $node.id }
}

$bmsNodes = @($graph.nodes | Where-Object { $_.id -eq "bms_fault_summary" })
$pcsNodes = @($graph.nodes | Where-Object { $_.id -eq "pcs_fault_summary" })
$globalNodes = @($graph.nodes | Where-Object { $_.id -eq "local_dio_fault_state" })
if ($bmsNodes.Count -ne 1 -or $pcsNodes.Count -ne 1 -or $globalNodes.Count -ne 1) {
    throw "Expected one BMS, PCS and global fault summary node."
}
Set-ControlGateConditions -Node $bmsNodes[0] -Conditions @(
    @($bmsNodes[0].parameters.conditions | Where-Object { [int]$_.index -notin @(8197, 8198) })
)
Set-ControlGateConditions -Node $pcsNodes[0] -Conditions @(
    @($pcsNodes[0].parameters.conditions | Where-Object { [int]$_.index -ne 1213 })
)
Set-ControlGateConditions -Node $globalNodes[0] -Conditions @(
    @($globalNodes[0].parameters.conditions | Where-Object { [int]$_.index -ne $outputIndex })
)

$globalAlarmConditions = @(
    [ordered]@{ index = 8197; operator = "ne"; value = 0.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 8198; operator = "ne"; value = 0.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 1213; operator = "ne"; value = 0.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 188; operator = "ne"; value = 1.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 189; operator = "lt"; value = -20.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 189; operator = "gt"; value = 55.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 190; operator = "lt"; value = 0.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 190; operator = "gt"; value = 99.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 217; operator = "lt"; value = -20.0; missingValue = 0; skipIfUnrouted = $true },
    [ordered]@{ index = 217; operator = "gt"; value = 55.0; missingValue = 0; skipIfUnrouted = $true }
)
if ($AlarmContactIndex -gt 0) {
    $globalAlarmConditions += [ordered]@{
        index = $AlarmContactIndex
        operator = "ne"
        value = $AlarmContactNormalValue
        missingValue = 0
        skipIfUnrouted = $true
    }
}
$globalAlarmOutputIndex = 730000
$globalAlarmNodes = @($graph.nodes | Where-Object { $_.id -eq "global_alarm_state" })
if ($globalAlarmNodes.Count -gt 1) {
    throw "Expected at most one global_alarm_state node, found $($globalAlarmNodes.Count)."
}
if ($globalAlarmNodes.Count -eq 0) {
    $globalAlarmNode = [pscustomobject]@{
        id = "global_alarm_state"
        type = "controlGate"
        typeVersion = "1.0.0"
        displayName = "global_alarm_state"
        enabled = $true
        order = [int](($graph.nodes.order | Measure-Object -Maximum).Maximum) + 1
        groupId = ""
        parameters = [pscustomobject]@{
            combine = "any"
            conditions = @()
            outputIndex = $globalAlarmOutputIndex
        }
        ports = @(
            [pscustomobject]@{
                id = "outputindex"
                displayName = "outputIndex"
                direction = "output"
                valueType = "number"
                unit = ""
                required = $true
                runtimePath = "/outputIndex"
                binding = [pscustomobject]@{
                    kind = "point"
                    index = $globalAlarmOutputIndex
                    pointCode = ""
                    semanticRole = ""
                    constant = $null
                }
            }
        )
        layout = [pscustomobject]@{ x = 1160; y = 20540; width = 280; height = 180 }
    }
    $graph.nodes += $globalAlarmNode
} else {
    $globalAlarmNode = $globalAlarmNodes[0]
}
Set-ControlGateConditions -Node $globalAlarmNode -Conditions $globalAlarmConditions

$virtualPoints = @($virtualConfig.meters.points | Where-Object {
    $_.pointRole -in @("dehumidifier_fault_summary", "dehumidifier_alarm_summary") -or
        [int]$_.index -eq $outputIndex
})
if ($virtualPoints.Count -ne 1) {
    throw "Expected one dehumidifier fault virtual point at $outputIndex, found $($virtualPoints.Count)."
}
$description = "除湿通信异常或温湿度测量越出设备有效范围时为 1"
if ($AlarmContactIndex -gt 0) {
    $description += "，并包含 F2 报警触点 $AlarmContactIndex"
} else {
    $description += "；内部故障需接入 F2 报警触点后补充"
}
$description += "；仅告警上报，不参与充放电故障联锁"
$virtualPoints[0].pointCode = "ems_dehumidifier_alarm_summary"
$virtualPoints[0].name = "除湿告警汇总"
$virtualPoints[0].desc = $description
$virtualPoints[0].pointRole = "dehumidifier_alarm_summary"

$globalAlarmPoints = @($virtualConfig.meters.points | Where-Object {
    [int]$_.index -eq $globalAlarmOutputIndex -or $_.pointRole -eq "global_alarm_state"
})
if ($globalAlarmPoints.Count -gt 1) {
    throw "Expected at most one global alarm virtual point, found $($globalAlarmPoints.Count)."
}
if ($globalAlarmPoints.Count -eq 0) {
    $targetMeters = @($virtualConfig.meters | Where-Object {
        @($_.points | Where-Object { [int]$_.index -eq $outputIndex }).Count -eq 1
    })
    if ($targetMeters.Count -ne 1) {
        throw "Expected one target virtual meter for global alarm output."
    }
    $globalAlarmPoint = $virtualPoints[0] | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $globalAlarmPoint.index = $globalAlarmOutputIndex
    if ($globalAlarmPoint.PSObject.Properties.Name -contains "legacyIndex") {
        $globalAlarmPoint.legacyIndex = $globalAlarmOutputIndex
    }
    $globalAlarmPoint.address = $globalAlarmOutputIndex
    $targetMeters[0].points += $globalAlarmPoint
} else {
    $globalAlarmPoint = $globalAlarmPoints[0]
}
$globalAlarmPoint.pointCode = "ems_global_alarm_summary"
$globalAlarmPoint.name = "全局告警总"
$globalAlarmPoint.desc = "任一不阻断充放电的设备告警有效时为 1；独立上报，不参与故障灯和停机联锁"
$globalAlarmPoint.pointRole = "global_alarm_state"
$globalAlarmPoint.category = "alarm"
$globalAlarmPoint.fullUpload = $true
$globalAlarmPoint.reportOnChange = $true
$globalAlarmPoint.persistIntervalSec = 0

$globalOutputIndex = [int]$globalNodes[0].parameters.outputIndex
$globalVirtualPoints = @($virtualConfig.meters.points | Where-Object {
    [int]$_.index -eq $globalOutputIndex -or $_.pointRole -eq "local_dio_fault_state"
})
if ($globalVirtualPoints.Count -eq 1) {
    $globalVirtualPoints[0].desc = "任一明确影响充放电或要求停机的设备故障时为 1；不包含非阻断性设备告警"
}

foreach ($path in @($OutputGraph, $OutputVirtualConfig)) {
    $directory = Split-Path -Parent $path
    if ($directory) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }
}
$graph | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $OutputGraph -Encoding utf8
$virtualConfig | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $OutputVirtualConfig -Encoding utf8

Write-Output "graph=$OutputGraph"
Write-Output "virtualConfig=$OutputVirtualConfig"
Write-Output "outputIndex=$outputIndex conditions=$($conditions.Count) alarmContactIndex=$AlarmContactIndex"
