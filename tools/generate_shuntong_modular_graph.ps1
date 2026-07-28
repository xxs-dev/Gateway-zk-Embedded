param(
    [string]$Source = "tools/testdata/shuntong_ems_generator_seed.json",
    [string]$Output = "config/examples/shuntong_ems_modular_graph.json",
    [string]$RuntimeOutput = "config/factory/runtime/logic/shuntong_ems_graph.json",
    [string]$VirtualSource = "config/examples/device_ems_virtual_base.json",
    [string]$VirtualOutput = "config/examples/device_ems_modular_virtual.json",
    [string]$RuntimeVirtualOutput = "config/factory/runtime/devices/device_ems_virtual.json",
    [string]$IndexRemapFile = ""
)

$ErrorActionPreference = "Stop"
$sourceDocument = Get-Content -Raw -LiteralPath $Source | ConvertFrom-Json
if ([string]$sourceDocument.schemaVersion -match '^2(?:\.|$)') {
    $sourceGraph = [pscustomobject]@{
        graphCode = $sourceDocument.graphCode
        nodes = @($sourceDocument.nodes | ForEach-Object {
            [pscustomobject]@{
                id = $_.id
                type = $_.type
                enabled = $_.enabled
                params = $_.parameters
            }
        })
    }
} else {
    $sourceGraph = $sourceDocument
}
$nodes = [System.Collections.ArrayList]::new()
$edges = [System.Collections.ArrayList]::new()
$previousNode = $null
$nextIndex = 700000
$indexRemap = @{}
$indexRemapTargets = @{}
if (-not [string]::IsNullOrWhiteSpace($IndexRemapFile)) {
    $remapObject = Get-Content -Raw -LiteralPath $IndexRemapFile | ConvertFrom-Json
    foreach ($property in $remapObject.PSObject.Properties) {
        $sourceIndex = [int]$property.Name
        $targetIndex = [int]$property.Value
        if ($sourceIndex -le 0 -or $targetIndex -le 0) {
            throw "Index remap values must be positive: $($property.Name)=$($property.Value)"
        }
        if ($indexRemapTargets.ContainsKey($targetIndex) -and $indexRemapTargets[$targetIndex] -ne $sourceIndex) {
            throw "Index remap target must be unique: $sourceIndex and $($indexRemapTargets[$targetIndex]) both map to $targetIndex"
        }
        $indexRemap[$sourceIndex] = $targetIndex
        $indexRemapTargets[$targetIndex] = $sourceIndex
    }
}

function New-Index {
    $value = $script:nextIndex
    $script:nextIndex++
    $value
}

function Input-Index([int]$Index, [Nullable[double]]$DefaultValue = $null) {
    $input = [ordered]@{ index = $Index }
    if ($null -ne $DefaultValue) { $input.defaultValue = [double]$DefaultValue }
    $input
}

function Input-Value([double]$Value) {
    [ordered]@{ value = $Value }
}

function Add-Node([string]$Id, [string]$Type, [System.Collections.IDictionary]$Params) {
    [void]$script:nodes.Add([ordered]@{
        id = $Id
        type = $Type
        enabled = $true
        params = $Params
    })
    if ($null -ne $script:previousNode) {
        [void]$script:edges.Add([ordered]@{ from = $script:previousNode; to = $Id })
    }
    $script:previousNode = $Id
}

function Set-NodeProfiles(
    [int]$Start,
    [string[]]$Required = @(),
    [string[]]$Optional = @(),
    [string[]]$Disabled = @()
) {
    for ($i = $Start; $i -lt $script:nodes.Count; $i++) {
        for ($keyIndex = 0; $keyIndex -lt $Required.Count; $keyIndex++) {
            $suffix = if ($keyIndex -eq 0) { "" } else { [string]($keyIndex + 1) }
            $script:nodes[$i].params["profileKey$suffix"] = $Required[$keyIndex]
        }
        for ($keyIndex = 0; $keyIndex -lt $Optional.Count; $keyIndex++) {
            $suffix = if ($keyIndex -eq 0) { "" } else { [string]($keyIndex + 1) }
            $script:nodes[$i].params["optionalProfileKey$suffix"] = $Optional[$keyIndex]
        }
        for ($keyIndex = 0; $keyIndex -lt $Disabled.Count; $keyIndex++) {
            $suffix = if ($keyIndex -eq 0) { "" } else { [string]($keyIndex + 1) }
            $script:nodes[$i].params["profileDisabledKey$suffix"] = $Disabled[$keyIndex]
        }
    }
}

function Get-OrdinalSortedKeys([System.Collections.IDictionary]$Value) {
    [string[]]$keys = @($Value.Keys | ForEach-Object { [string]$_ })
    [Array]::Sort($keys, [StringComparer]::Ordinal)
    $keys
}

function Add-Formula(
    [string]$Id,
    [string]$Operation,
    [object[]]$Inputs,
    [int]$OutputIndex,
    [hashtable]$Extra = @{}
) {
    $params = [ordered]@{ operation = $Operation; inputs = $Inputs; outputIndex = $OutputIndex }
    foreach ($key in Get-OrdinalSortedKeys $Extra) { $params[$key] = $Extra[$key] }
    Add-Node $Id "formula" $params
}

function Add-Switch(
    [string]$Id,
    [System.Collections.IDictionary]$Condition,
    [System.Collections.IDictionary]$WhenTrue,
    [System.Collections.IDictionary]$WhenFalse,
    [int]$OutputIndex
) {
    $params = [ordered]@{ outputIndex = $OutputIndex }
    foreach ($key in Get-OrdinalSortedKeys $Condition) { $params[$key] = $Condition[$key] }
    foreach ($key in Get-OrdinalSortedKeys $WhenTrue) { $params[$key] = $WhenTrue[$key] }
    foreach ($key in Get-OrdinalSortedKeys $WhenFalse) { $params[$key] = $WhenFalse[$key] }
    Add-Node $Id "switch" $params
}

function Add-PowerMetrics(
    [string]$Prefix,
    [int[]]$PIndexes,
    [int[]]$QIndexes,
    [int[]]$SOutputs,
    [int[]]$CosOutputs,
    [int]$BalanceOutput
) {
    for ($phase = 0; $phase -lt 4; $phase++) {
        $pSquare = New-Index
        $qSquare = New-Index
        $sumSquare = New-Index
        Add-Formula "${Prefix}_p${phase}_square" "square" @((Input-Index $PIndexes[$phase])) $pSquare
        Add-Formula "${Prefix}_q${phase}_square" "square" @((Input-Index $QIndexes[$phase])) $qSquare
        Add-Formula "${Prefix}_s${phase}_sum" "add" @((Input-Index $pSquare), (Input-Index $qSquare)) $sumSquare
        Add-Formula "${Prefix}_s${phase}" "sqrt" @((Input-Index $sumSquare)) $SOutputs[$phase]
        Add-Formula "${Prefix}_cos${phase}" "safeDivide" @((Input-Index $PIndexes[$phase]), (Input-Index $SOutputs[$phase])) $CosOutputs[$phase]
    }

    $maxPhase = New-Index
    $minPhase = New-Index
    $spread = New-Index
    $ratio = New-Index
    $scaled = New-Index
    Add-Formula "${Prefix}_phase_max" "max" @((Input-Index $PIndexes[0]), (Input-Index $PIndexes[1]), (Input-Index $PIndexes[2])) $maxPhase
    Add-Formula "${Prefix}_phase_min" "min" @((Input-Index $PIndexes[0]), (Input-Index $PIndexes[1]), (Input-Index $PIndexes[2])) $minPhase
    Add-Formula "${Prefix}_phase_spread" "subtract" @((Input-Index $maxPhase), (Input-Index $minPhase)) $spread
    Add-Formula "${Prefix}_balance_ratio" "safeDivide" @((Input-Index $spread), (Input-Index $PIndexes[3])) $ratio
    Add-Formula "${Prefix}_balance_scaled" "multiply" @((Input-Index $ratio), (Input-Value 300)) $scaled
    Add-Formula "${Prefix}_balance" "abs" @((Input-Index $scaled)) $BalanceOutput
}

function Add-DerivedLoadBranch(
    [string]$Prefix,
    [int[]]$PrimaryPIndexes,
    [int[]]$PrimaryQIndexes,
    [int[]]$SecondaryPIndexes,
    [int[]]$SecondaryQIndexes
) {
    $pOutputs = @(309, 310, 311, 312)
    $qOutputs = @(313, 314, 315, 316)
    for ($phase = 0; $phase -lt 4; $phase++) {
        Add-Formula "${Prefix}_p${phase}" "subtract" @(
            (Input-Index $PrimaryPIndexes[$phase]),
            (Input-Index $SecondaryPIndexes[$phase])
        ) $pOutputs[$phase]
        Add-Formula "${Prefix}_q${phase}" "subtract" @(
            (Input-Index $PrimaryQIndexes[$phase]),
            (Input-Index $SecondaryQIndexes[$phase])
        ) $qOutputs[$phase]
    }
    Add-PowerMetrics $Prefix $pOutputs $qOutputs @(317, 318, 319, 320) @(321, 322, 323, 324) 325
}

function Source-Node([string]$Id) {
    $node = $sourceGraph.nodes | Where-Object { $_.id -eq $Id } | Select-Object -First 1
    if ($null -eq $node) { throw "Source graph node not found: $Id" }
    $node
}

function Apply-GraphIndexRemap([object]$Value, [string]$PropertyName = "") {
    if ($null -eq $Value) { return }
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in @($Value.Keys)) {
            $child = $Value[$key]
            if ($key -match '(?i)index' -and $child -isnot [System.Collections.IEnumerable]) {
                $parsed = 0
                if ([int]::TryParse([string]$child, [ref]$parsed) -and $indexRemap.ContainsKey($parsed)) {
                    $Value[$key] = $indexRemap[$parsed]
                    continue
                }
            }
            if ($key -match '(?i)indexes' -and $child -is [System.Collections.IList]) {
                for ($i = 0; $i -lt $child.Count; $i++) {
                    $parsed = 0
                    if ([int]::TryParse([string]$child[$i], [ref]$parsed) -and $indexRemap.ContainsKey($parsed)) {
                        $child[$i] = $indexRemap[$parsed]
                    } else {
                        Apply-GraphIndexRemap $child[$i] $key
                    }
                }
                continue
            }
            Apply-GraphIndexRemap $child $key
        }
        return
    }
    if ($Value -is [System.Collections.IList]) {
        for ($i = 0; $i -lt $Value.Count; $i++) {
            Apply-GraphIndexRemap $Value[$i] $PropertyName
        }
        return
    }
    if ($Value -is [pscustomobject]) {
        foreach ($property in $Value.PSObject.Properties) {
            $child = $property.Value
            if ($property.Name -match '(?i)index') {
                $parsed = 0
                if ([int]::TryParse([string]$child, [ref]$parsed) -and $indexRemap.ContainsKey($parsed)) {
                    $property.Value = $indexRemap[$parsed]
                    continue
                }
            }
            Apply-GraphIndexRemap $child $property.Name
        }
    }
}

function Get-GraphMember([object]$Value, [string]$Name) {
    if ($Value -is [System.Collections.IDictionary]) {
        return $Value[$Name]
    }
    $property = $Value.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    $property.Value
}

function Escape-GraphPointerToken([string]$Value) {
    $Value.Replace("~", "~0").Replace("/", "~1")
}

function Get-V2PortDirection([string]$NodeType, [string]$PropertyName) {
    if (($NodeType -eq "controlWrite" -or $NodeType -eq "pcsWriteback") -and
        $PropertyName -ieq "targetIndex") {
        return "target"
    }
    if ($PropertyName -match '(?i)outputIndex(?:es)?$') { return "output" }
    "input"
}

function Get-V2PortValueType([string]$Path) {
    $value = $Path.ToLowerInvariant()
    if ($value.Contains("soc")) { return "soc" }
    if ($value.Contains("power") -or $value.Contains("active") -or $value.Contains("reactive")) {
        return "power"
    }
    if ($value.Contains("state")) { return "state" }
    if ($value.Contains("enable") -or $value.Contains("permit") -or $value.Contains("run")) {
        return "boolean"
    }
    "number"
}

function Get-V2PortId([string]$Path) {
    $value = $Path.Trim('/').Replace("~1", "_").Replace("~0", "_")
    $value = [regex]::Replace($value, '[^A-Za-z0-9]+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($value)) { return "port" }
    $value.ToLowerInvariant()
}

function Add-V2IndexReference(
    [string]$NodeType,
    [string]$PropertyName,
    [string]$Path,
    [object]$Value,
    [System.Collections.ArrayList]$References
) {
    $parsed = 0
    if (-not [int]::TryParse([string]$Value, [ref]$parsed) -or $parsed -le 0) { return }
    [void]$References.Add([pscustomobject][ordered]@{
        Path = $Path
        PropertyName = $PropertyName
        Direction = Get-V2PortDirection $NodeType $PropertyName
        ValueType = Get-V2PortValueType $Path
        Index = $parsed
    })
}

function Find-V2IndexReferences(
    [object]$Value,
    [string]$NodeType,
    [string]$Path,
    [string]$PropertyName,
    [System.Collections.ArrayList]$References
) {
    if ($null -eq $Value) { return }
    if ($Value -is [System.Collections.IDictionary] -or $Value -is [pscustomobject]) {
        $properties = if ($Value -is [System.Collections.IDictionary]) {
            @($Value.Keys | ForEach-Object { [pscustomobject]@{ Name = [string]$_; Value = $Value[$_] } })
        } else {
            @($Value.PSObject.Properties)
        }
        foreach ($property in $properties) {
            $childPath = "$Path/$(Escape-GraphPointerToken $property.Name)"
            if ($property.Name -match '(?i)Index$') {
                Add-V2IndexReference $NodeType $property.Name $childPath $property.Value $References
                continue
            }
            if ($property.Name -match '(?i)Indexes$' -and
                $property.Value -is [System.Collections.IEnumerable] -and
                $property.Value -isnot [string]) {
                $position = 0
                foreach ($item in $property.Value) {
                    Add-V2IndexReference $NodeType $property.Name "$childPath/$position" $item $References
                    $position++
                }
                continue
            }
            Find-V2IndexReferences $property.Value $NodeType $childPath $property.Name $References
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        $position = 0
        foreach ($item in $Value) {
            Find-V2IndexReferences $item $NodeType "$Path/$position" $PropertyName $References
            $position++
        }
    }
}

function Convert-LegacyGraphToV2([object]$LegacyGraph) {
    $sourceNodes = @(Get-GraphMember $LegacyGraph "nodes")
    $sourceEdges = @(Get-GraphMember $LegacyGraph "edges")
    $limits = Get-GraphMember $LegacyGraph "limits"
    $v2Nodes = [System.Collections.ArrayList]::new()
    $portsByNode = @{}
    for ($order = 0; $order -lt $sourceNodes.Count; $order++) {
        $sourceNode = $sourceNodes[$order]
        $nodeId = [string](Get-GraphMember $sourceNode "id")
        $nodeType = [string](Get-GraphMember $sourceNode "type")
        $parameters = Get-GraphMember $sourceNode "params"
        $references = [System.Collections.ArrayList]::new()
        Find-V2IndexReferences $parameters $nodeType "" "" $references
        $ports = [System.Collections.ArrayList]::new()
        $seenPaths = @{}
        foreach ($reference in @($references | Sort-Object Path)) {
            if ($seenPaths.ContainsKey($reference.Path)) { continue }
            $seenPaths[$reference.Path] = $true
            [void]$ports.Add([ordered]@{
                id = Get-V2PortId $reference.Path
                displayName = $reference.PropertyName
                direction = $reference.Direction
                valueType = $reference.ValueType
                unit = ""
                required = $true
                runtimePath = $reference.Path
                binding = [ordered]@{
                    kind = "point"
                    index = $reference.Index
                    pointCode = ""
                    semanticRole = ""
                    constant = $null
                }
            })
        }
        $portsByNode[$nodeId] = $ports
        [void]$v2Nodes.Add([ordered]@{
            id = $nodeId
            type = $nodeType
            typeVersion = "1.0.0"
            displayName = $nodeId
            enabled = [bool](Get-GraphMember $sourceNode "enabled")
            order = $order
            groupId = ""
            parameters = $parameters
            ports = $ports
            layout = [ordered]@{
                x = 80 + (($order % 4) * 360)
                y = 80 + ([math]::Floor($order / 4) * 220)
                width = 280
                height = 180
            }
        })
    }

    $links = [System.Collections.ArrayList]::new()
    for ($edgeIndex = 0; $edgeIndex -lt $sourceEdges.Count; $edgeIndex++) {
        $sourceEdge = $sourceEdges[$edgeIndex]
        $fromNodeId = [string](Get-GraphMember $sourceEdge "from")
        $toNodeId = [string](Get-GraphMember $sourceEdge "to")
        if (-not $portsByNode.ContainsKey($fromNodeId) -or -not $portsByNode.ContainsKey($toNodeId)) {
            throw "Generated edge references an unknown node: $fromNodeId -> $toNodeId"
        }
        $matches = @()
        foreach ($outputPort in $portsByNode[$fromNodeId]) {
            if ($outputPort.direction -ne "output") { continue }
            foreach ($inputPort in $portsByNode[$toNodeId]) {
                if ($inputPort.direction -eq "input" -and
                    [int]$inputPort.binding.index -eq [int]$outputPort.binding.index) {
                    $matches += ,@($outputPort, $inputPort)
                }
            }
        }
        if ($matches.Count -eq 0) {
            [void]$links.Add([ordered]@{
                id = "edge_{0:D4}" -f $edgeIndex
                kind = "dependency"
                fromNodeId = $fromNodeId
                fromPortId = ""
                toNodeId = $toNodeId
                toPortId = ""
                inferred = $false
            })
            continue
        }
        for ($matchIndex = 0; $matchIndex -lt $matches.Count; $matchIndex++) {
            [void]$links.Add([ordered]@{
                id = ("edge_{0:D4}_{1:D2}" -f $edgeIndex, $matchIndex)
                kind = "data"
                fromNodeId = $fromNodeId
                fromPortId = $matches[$matchIndex][0].id
                toNodeId = $toNodeId
                toPortId = $matches[$matchIndex][1].id
                inferred = $true
            })
        }
    }

    $maxNodes = [math]::Max([int](Get-GraphMember $limits "maxNodes"), $v2Nodes.Count)
    $maxEdges = [math]::Max([int](Get-GraphMember $limits "maxEdges"), [math]::Max(1, $links.Count))
    [ordered]@{
        schemaVersion = "2.0.0"
        graphCode = [string](Get-GraphMember $LegacyGraph "graphCode")
        displayName = [string](Get-GraphMember $LegacyGraph "graphCode")
        description = "由既有舜通策略迁移；同一文件用于编辑、发布和边端直接执行。"
        compile = [ordered]@{
            maxNodes = $maxNodes
            maxEdges = $maxEdges
            virtualIndexStart = 700000
            virtualIndexEnd = 799999
            requireKnownWriteTargets = $false
            preserveImportedBehavior = $true
        }
        nodes = $v2Nodes
        links = $links
        groups = @()
    }
}

function Param-Int([object]$Params, [string]$Name, [int]$DefaultValue) {
    $property = $Params.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $DefaultValue }
    [int]$property.Value
}

foreach ($sourceId in @("tq_average", "cn_average")) {
    $sourceNode = Source-Node $sourceId
    foreach ($mapping in $sourceNode.params.mappings) {
        $params = [ordered]@{
            operation = "average"
            inputIndex = [int]$mapping.input
            outputIndex = [int]$mapping.output
            windowSizeIndex = [int]$sourceNode.params.windowSizeIndex
        }
        $params.profileKey = if ($sourceId -eq "tq_average") { "Meter_TQ" } else { "Meter_CN" }
        Add-Node "${sourceId}_$($mapping.output)" "windowAggregate" $params
    }
}

$tqMetricsStart = $nodes.Count
Add-PowerMetrics "tq" @(209, 210, 211, 212) @(213, 214, 215, 216) @(217, 218, 219, 220) @(221, 222, 223, 224) 225
Set-NodeProfiles $tqMetricsStart -Required @("Meter_TQ")

$derived = Source-Node "fh_derived"
$derivedCnStart = $nodes.Count
Add-DerivedLoadBranch -Prefix "fh_cn" `
    -PrimaryPIndexes @([int]$derived.params.tqPaIndex, [int]$derived.params.tqPbIndex, [int]$derived.params.tqPcIndex, [int]$derived.params.tqP3Index) `
    -PrimaryQIndexes @([int]$derived.params.tqQaIndex, [int]$derived.params.tqQbIndex, [int]$derived.params.tqQcIndex, [int]$derived.params.tqQ3Index) `
    -SecondaryPIndexes @([int]$derived.params.cnPaIndex, [int]$derived.params.cnPbIndex, [int]$derived.params.cnPcIndex, [int]$derived.params.cnP3Index) `
    -SecondaryQIndexes @([int]$derived.params.cnQaIndex, [int]$derived.params.cnQbIndex, [int]$derived.params.cnQcIndex, [int]$derived.params.cnQ3Index)
Set-NodeProfiles $derivedCnStart -Required @("Meter_TQ", "Meter_CN") -Disabled @("Meter_BW", "Meter_FH")

$bwAverageStart = $nodes.Count
$bwInputs = @(4537, 4538, 4539, 4536, 4541, 4542, 4543, 4540)
$bwOutputs = @()
for ($i = 0; $i -lt $bwInputs.Count; $i++) {
    $bwOutput = New-Index
    $bwOutputs += $bwOutput
    Add-Node "bw_average_$i" "windowAggregate" ([ordered]@{
        operation = "average"
        inputIndex = $bwInputs[$i]
        outputIndex = $bwOutput
        windowSizeIndex = [int](Source-Node "tq_average").params.windowSizeIndex
    })
}
Set-NodeProfiles $bwAverageStart -Optional @("Meter_BW")

$derivedBwStart = $nodes.Count
Add-DerivedLoadBranch -Prefix "fh_bw" `
    -PrimaryPIndexes @([int]$derived.params.tqPaIndex, [int]$derived.params.tqPbIndex, [int]$derived.params.tqPcIndex, [int]$derived.params.tqP3Index) `
    -PrimaryQIndexes @([int]$derived.params.tqQaIndex, [int]$derived.params.tqQbIndex, [int]$derived.params.tqQcIndex, [int]$derived.params.tqQ3Index) `
    -SecondaryPIndexes @($bwOutputs[0], $bwOutputs[1], $bwOutputs[2], $bwOutputs[3]) `
    -SecondaryQIndexes @($bwOutputs[4], $bwOutputs[5], $bwOutputs[6], $bwOutputs[7])
Set-NodeProfiles $derivedBwStart -Required @("Meter_TQ") -Optional @("Meter_BW") -Disabled @("Meter_FH")

$bms = Source-Node "bms"
Add-Formula "bms_charge_kw_allow" "multiply" @(
    (Input-Index ([int]$bms.params.voltageIndex)),
    (Input-Index ([int]$bms.params.chargeCurrentAllowIndex)),
    (Input-Value 0.001)
) ([int]$bms.params.chargeKwAllowOutput)
Add-Formula "bms_discharge_kw_allow" "multiply" @(
    (Input-Index ([int]$bms.params.voltageIndex)),
    (Input-Index ([int]$bms.params.dischargeCurrentAllowIndex)),
    (Input-Value 0.001)
) ([int]$bms.params.dischargeKwAllowOutput)
Add-Formula "bms_charge_kwh_today" "subtract" @(
    (Input-Index ([int]$bms.params.chargeKwhSumIndex)),
    (Input-Index ([int]$bms.params.chargeKwhZeroIndex))
) ([int]$bms.params.chargeKwhTodayOutput) @{
    profileIntKey = "BMS_MODEL"; profileIntValue = 1; profileIntValue2 = 3
}
Add-Formula "bms_discharge_kwh_today" "subtract" @(
    (Input-Index ([int]$bms.params.dischargeKwhSumIndex)),
    (Input-Index ([int]$bms.params.dischargeKwhZeroIndex))
) ([int]$bms.params.dischargeKwhTodayOutput) @{
    profileIntKey = "BMS_MODEL"; profileIntValue = 1; profileIntValue2 = 3
}

$stationLimit = Source-Node "station_limit_v2"
$stationLimitStart = $nodes.Count
$positiveHourlyCurve = @()
$negativeHourlyCurve = @()
for ($hour = 0; $hour -lt 24; $hour++) {
    $positiveHourlyCurve += [ordered]@{
        hour = $hour
        powerIndex = [int]$stationLimit.params.hourlyPositiveValueStartIndex + $hour
    }
    $negativeHourlyCurve += [ordered]@{
        hour = $hour
        powerIndex = [int]$stationLimit.params.hourlyNegativeValueStartIndex + $hour
    }
}
$positiveHourlyValue = New-Index
$positiveHourlyEnable = New-Index
$negativeHourlyValue = New-Index
$negativeHourlyEnable = New-Index
Add-Node "station_limit_positive_schedule" "scheduleSelect" ([ordered]@{
    scheduleCurve = $positiveHourlyCurve
    powerOutputIndex = $positiveHourlyValue
    enableMaskIndexes = @($stationLimit.params.hourlyPositiveEnableMaskIndexes)
    enableOutputIndex = $positiveHourlyEnable
})
Add-Node "station_limit_negative_schedule" "scheduleSelect" ([ordered]@{
    scheduleCurve = $negativeHourlyCurve
    powerOutputIndex = $negativeHourlyValue
    enableMaskIndexes = @($stationLimit.params.hourlyNegativeEnableMaskIndexes)
    enableOutputIndex = $negativeHourlyEnable
})
$positiveSelectedValue = New-Index
$positiveSelectedEnable = New-Index
$negativeSelectedValue = New-Index
$negativeSelectedEnable = New-Index
Add-Switch "station_limit_positive_value_mode" ([ordered]@{
    leftIndex = [int]$stationLimit.params.hourlyModeIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $positiveHourlyValue }) ([ordered]@{ falseIndex = [int]$stationLimit.params.fixedPositiveValueIndex }) $positiveSelectedValue
Add-Switch "station_limit_positive_enable_mode" ([ordered]@{
    leftIndex = [int]$stationLimit.params.hourlyModeIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $positiveHourlyEnable }) ([ordered]@{ falseIndex = [int]$stationLimit.params.fixedPositiveEnableIndex }) $positiveSelectedEnable
Add-Switch "station_limit_negative_value_mode" ([ordered]@{
    leftIndex = [int]$stationLimit.params.hourlyModeIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $negativeHourlyValue }) ([ordered]@{ falseIndex = [int]$stationLimit.params.fixedNegativeValueIndex }) $negativeSelectedValue
Add-Switch "station_limit_negative_enable_mode" ([ordered]@{
    leftIndex = [int]$stationLimit.params.hourlyModeIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $negativeHourlyEnable }) ([ordered]@{ falseIndex = [int]$stationLimit.params.fixedNegativeEnableIndex }) $negativeSelectedEnable
Add-Switch "station_limit_positive_value" ([ordered]@{
    leftIndex = [int]$stationLimit.params.masterEnableIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $positiveSelectedValue }) ([ordered]@{ falseValue = 0 }) ([int]$stationLimit.params.positiveValueOutputIndex)
Add-Node "station_limit_positive_enable" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$stationLimit.params.masterEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = $positiveSelectedEnable; operator = "eq"; value = 1 }
    )
    outputIndex = [int]$stationLimit.params.positiveEnableOutputIndex
})
Add-Switch "station_limit_negative_value" ([ordered]@{
    leftIndex = [int]$stationLimit.params.masterEnableIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $negativeSelectedValue }) ([ordered]@{ falseValue = 0 }) ([int]$stationLimit.params.negativeValueOutputIndex)
Add-Node "station_limit_negative_enable" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$stationLimit.params.masterEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = $negativeSelectedEnable; operator = "eq"; value = 1 }
    )
    outputIndex = [int]$stationLimit.params.negativeEnableOutputIndex
})
Set-NodeProfiles $stationLimitStart -Optional @([string]$stationLimit.params.profileKey)

$cosStart = $nodes.Count
$cos = Source-Node "cos"
$targetAcos = New-Index
$targetTan = New-Index
Add-Formula "cos_target_acos" "acos" @((Input-Index ([int]$cos.params.targetCosIndex))) $targetAcos @{ invalidPolicy = "skip" }
Add-Formula "cos_target_tan" "tan" @((Input-Index $targetAcos)) $targetTan
$targetQ = @(505, 506, 507)
$outputQ = @(601, 602, 603)
$phaseP = @([int]$cos.params.tqPaIndex, [int]$cos.params.tqPbIndex, [int]$cos.params.tqPcIndex)
$phaseQ = @([int]$cos.params.tqQaIndex, [int]$cos.params.tqQbIndex, [int]$cos.params.tqQcIndex)
for ($phase = 0; $phase -lt 3; $phase++) {
    $targetSigned = New-Index
    $negativeTarget = New-Index
    $positiveRaw = New-Index
    $negativeRaw = New-Index
    $negativeSelected = New-Index
    Add-Formula "cos_target_q${phase}_signed" "multiply" @((Input-Index $targetTan), (Input-Index $phaseP[$phase])) $targetSigned
    Add-Formula "cos_target_q${phase}" "abs" @((Input-Index $targetSigned)) $targetQ[$phase]
    Add-Formula "cos_target_q${phase}_negative" "negate" @((Input-Index $targetQ[$phase])) $negativeTarget
    Add-Formula "cos_q${phase}_positive_raw" "subtract" @((Input-Index $phaseQ[$phase]), (Input-Index $targetQ[$phase])) $positiveRaw
    Add-Formula "cos_q${phase}_negative_raw" "add" @((Input-Index $phaseQ[$phase]), (Input-Index $targetQ[$phase])) $negativeRaw
    Add-Switch "cos_q${phase}_negative_select" ([ordered]@{
        leftIndex = $phaseQ[$phase]; operator = "lt"; rightIndex = $negativeTarget
    }) ([ordered]@{ trueIndex = $negativeRaw }) ([ordered]@{ falseValue = 0 }) $negativeSelected
    Add-Switch "cos_q${phase}_select" ([ordered]@{
        leftIndex = $phaseQ[$phase]; operator = "gt"; rightIndex = $targetQ[$phase]
    }) ([ordered]@{ trueIndex = $positiveRaw }) ([ordered]@{ falseIndex = $negativeSelected }) $outputQ[$phase]
}
Add-Formula "cos_target_q3" "add" @((Input-Index 505), (Input-Index 506), (Input-Index 507)) 508
$cosAbs = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $absolute = New-Index
    Add-Formula "cos_output_q${phase}_abs" "abs" @((Input-Index $outputQ[$phase])) $absolute
    $cosAbs += $absolute
}
Add-Formula "cos_output_q3" "add" @((Input-Index $cosAbs[0]), (Input-Index $cosAbs[1]), (Input-Index $cosAbs[2])) 604
Add-Switch "cos_run" ([ordered]@{ leftIndex = 604; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) 8
Set-NodeProfiles $cosStart -Required @("Meter_TQ")

$voltageStart = $nodes.Count
$voltage = Source-Node "lv_hv"
$negativePMax = New-Index
Add-Formula "voltage_negative_pmax" "negate" @((Input-Index ([int]$voltage.params.pMaxIndex))) $negativePMax
$voltageInputs = @([int]$voltage.params.cnUaIndex, [int]$voltage.params.cnUbIndex, [int]$voltage.params.cnUcIndex)
$voltageRealtimeInputs = @($voltage.params.cnRealtimeUIndexes | ForEach-Object { [int]$_ })
$powerRealtimeInputs = @($voltage.params.cnRealtimePIndexes | ForEach-Object { [int]$_ })
$lvSevereThreshold = New-Index
$hvSevereThreshold = New-Index
Add-Formula "lv_severe_threshold" "subtract" @((Input-Index ([int]$voltage.params.lvLowIndex)), (Input-Value 8)) $lvSevereThreshold
Add-Formula "hv_severe_threshold" "add" @((Input-Index ([int]$voltage.params.hvUpIndex)), (Input-Value 8)) $hvSevereThreshold
$lvOutputs = @(605, 606, 607)
$hvOutputs = @(609, 610, 611)
for ($phase = 0; $phase -lt 3; $phase++) {
    $lvLowTarget = New-Index
    $lvHighTarget = New-Index
    $lvHigh = New-Index
    $lvRaw = New-Index
    $lvClamped = New-Index
    $lvSevereScaled = New-Index
    $lvSevereSelected = New-Index
    Add-Formula "lv_phase${phase}_low_target" "subtract" @((Input-Index $powerRealtimeInputs[$phase]), (Input-Index ([int]$voltage.params.gradPIndex))) $lvLowTarget
    Add-Formula "lv_phase${phase}_high_target" "add" @((Input-Index $powerRealtimeInputs[$phase]), (Input-Index ([int]$voltage.params.gradPIndex))) $lvHighTarget
    Add-Switch "lv_phase${phase}_high" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "gt"; rightIndex = [int]$voltage.params.lvUpIndex
    }) ([ordered]@{ trueIndex = $lvHighTarget }) ([ordered]@{ falseValue = 0 }) $lvHigh
    Add-Switch "lv_phase${phase}_raw" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "lt"; rightIndex = [int]$voltage.params.lvLowIndex
    }) ([ordered]@{ trueIndex = $lvLowTarget }) ([ordered]@{ falseIndex = $lvHigh }) $lvRaw
    Add-Formula "lv_phase${phase}_clamp" "clamp" @((Input-Index $lvRaw)) $lvClamped @{
        lowerIndex = $negativePMax; upper = 0
    }
    Add-Formula "lv_phase${phase}_severe_scale" "multiply" @((Input-Index $lvClamped), (Input-Value 1.2)) $lvSevereScaled
    Add-Switch "lv_phase${phase}_severe" ([ordered]@{
        leftIndex = $voltageRealtimeInputs[$phase]; operator = "lt"; rightIndex = $lvSevereThreshold
    }) ([ordered]@{ trueIndex = $lvSevereScaled }) ([ordered]@{ falseIndex = $lvClamped }) $lvSevereSelected
    Add-Formula "lv_phase${phase}" "clamp" @((Input-Index $lvSevereSelected)) $lvOutputs[$phase] @{
        lowerIndex = $negativePMax; upper = 0
    }

    $hvHighTarget = New-Index
    $hvLowTarget = New-Index
    $hvLow = New-Index
    $hvRaw = New-Index
    $hvClamped = New-Index
    $hvSevereScaled = New-Index
    $hvSevereSelected = New-Index
    Add-Formula "hv_phase${phase}_high_target" "add" @((Input-Index $powerRealtimeInputs[$phase]), (Input-Index ([int]$voltage.params.gradPIndex))) $hvHighTarget
    Add-Formula "hv_phase${phase}_low_target" "subtract" @((Input-Index $powerRealtimeInputs[$phase]), (Input-Index ([int]$voltage.params.gradPIndex))) $hvLowTarget
    Add-Switch "hv_phase${phase}_low" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "lt"; rightIndex = [int]$voltage.params.hvLowIndex
    }) ([ordered]@{ trueIndex = $hvLowTarget }) ([ordered]@{ falseValue = 0 }) $hvLow
    Add-Switch "hv_phase${phase}_raw" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "gt"; rightIndex = [int]$voltage.params.hvUpIndex
    }) ([ordered]@{ trueIndex = $hvHighTarget }) ([ordered]@{ falseIndex = $hvLow }) $hvRaw
    Add-Formula "hv_phase${phase}_clamp" "clamp" @((Input-Index $hvRaw)) $hvClamped @{
        lower = 0; upperIndex = [int]$voltage.params.pMaxIndex
    }
    Add-Formula "hv_phase${phase}_severe_scale" "multiply" @((Input-Index $hvClamped), (Input-Value 1.2)) $hvSevereScaled
    Add-Switch "hv_phase${phase}_severe" ([ordered]@{
        leftIndex = $voltageRealtimeInputs[$phase]; operator = "gt"; rightIndex = $hvSevereThreshold
    }) ([ordered]@{ trueIndex = $hvSevereScaled }) ([ordered]@{ falseIndex = $hvClamped }) $hvSevereSelected
    Add-Formula "hv_phase${phase}" "clamp" @((Input-Index $hvSevereSelected)) $hvOutputs[$phase] @{
        lower = 0; upperIndex = [int]$voltage.params.pMaxIndex
    }
}
$lvAbs = @()
$hvAbs = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $lvAbsolute = New-Index
    $hvAbsolute = New-Index
    Add-Formula "lv_phase${phase}_abs" "abs" @((Input-Index $lvOutputs[$phase])) $lvAbsolute
    Add-Formula "hv_phase${phase}_abs" "abs" @((Input-Index $hvOutputs[$phase])) $hvAbsolute
    $lvAbs += $lvAbsolute
    $hvAbs += $hvAbsolute
}
Add-Formula "lv_total" "add" @((Input-Index $lvAbs[0]), (Input-Index $lvAbs[1]), (Input-Index $lvAbs[2])) 608
Add-Formula "hv_total" "add" @((Input-Index $hvAbs[0]), (Input-Index $hvAbs[1]), (Input-Index $hvAbs[2])) 612
Set-NodeProfiles $voltageStart -Required @("Meter_CN")

$chargeStart = $nodes.Count
$charge = Source-Node "cd_fd"
$chargeGate = 14
Add-Node "charge_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$charge.params.bmsSocIndex; operator = "lt"; valueIndex = [int]$charge.params.cdTargetSocIndex },
        [ordered]@{ index = [int]$charge.params.cdTargetPowerIndex; operator = "ne"; value = 0 }
    )
    outputIndex = $chargeGate
})
$chargeAvailable = New-Index
$chargeAvailablePositive = New-Index
$chargeLimited = New-Index
$chargeByLimitEnable = New-Index
Add-Formula "charge_available" "subtract" @((Input-Index ([int]$charge.params.positiveLimitIndex)), (Input-Index ([int]$charge.params.fhP3Index))) $chargeAvailable
Add-Formula "charge_available_positive" "max" @((Input-Index $chargeAvailable), (Input-Value 0)) $chargeAvailablePositive
Add-Formula "charge_limited" "min" @((Input-Index $chargeAvailablePositive), (Input-Index ([int]$charge.params.cdTargetPowerIndex))) $chargeLimited
Add-Switch "charge_limit_enable" ([ordered]@{
    leftIndex = [int]$charge.params.positiveLimitEnableIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $chargeLimited }) ([ordered]@{ falseIndex = [int]$charge.params.cdTargetPowerIndex }) $chargeByLimitEnable
Add-Switch "charge_output" ([ordered]@{ conditionIndex = $chargeGate }) ([ordered]@{ trueIndex = $chargeByLimitEnable }) ([ordered]@{ falseValue = 0 }) 613

$dischargeGate = 16
Add-Node "discharge_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$charge.params.bmsSocIndex; operator = "gt"; valueIndex = [int]$charge.params.fdTargetSocIndex },
        [ordered]@{ index = [int]$charge.params.fdTargetPowerIndex; operator = "ne"; value = 0 }
    )
    outputIndex = $dischargeGate
})
$dischargeAvailable = New-Index
$dischargeAvailablePositive = New-Index
$dischargeLimited = New-Index
$dischargeNegative = New-Index
$dischargeDefaultNegative = New-Index
$dischargeByLimitEnable = New-Index
Add-Formula "discharge_available" "subtract" @((Input-Index ([int]$charge.params.negativeLimitIndex)), (Input-Index ([int]$charge.params.fhP3Index))) $dischargeAvailable
Add-Formula "discharge_available_positive" "max" @((Input-Index $dischargeAvailable), (Input-Value 0)) $dischargeAvailablePositive
Add-Formula "discharge_limited" "min" @((Input-Index $dischargeAvailablePositive), (Input-Index ([int]$charge.params.fdTargetPowerIndex))) $dischargeLimited
Add-Formula "discharge_limited_negative" "negate" @((Input-Index $dischargeLimited)) $dischargeNegative
Add-Formula "discharge_default_negative" "negate" @((Input-Index ([int]$charge.params.fdTargetPowerIndex))) $dischargeDefaultNegative
Add-Switch "discharge_limit_enable" ([ordered]@{
    leftIndex = [int]$charge.params.negativeLimitEnableIndex; operator = "eq"; rightValue = 1
}) ([ordered]@{ trueIndex = $dischargeNegative }) ([ordered]@{ falseIndex = $dischargeDefaultNegative }) $dischargeByLimitEnable
Add-Switch "discharge_output" ([ordered]@{ conditionIndex = $dischargeGate }) ([ordered]@{ trueIndex = $dischargeByLimitEnable }) ([ordered]@{ falseValue = 0 }) 614
Set-NodeProfiles $chargeStart -Required @("Meter_TQ")

$scheduleStart = $nodes.Count
$schedule = Source-Node "ds"
$scheduleMode = New-Index
$scheduleCurve = @($schedule.params.scheduleCurve)
if ($null -ne $schedule.params.powerStartIndex -and
    $null -ne $schedule.params.targetSocStartIndex -and
    $null -ne $schedule.params.modeStartIndex) {
    $scheduleCurve = @()
    for ($hour = 0; $hour -lt 24; $hour++) {
        $scheduleCurve += [ordered]@{
            hour = $hour
            powerIndex = [int]$schedule.params.powerStartIndex + $hour
            targetSocIndex = [int]$schedule.params.targetSocStartIndex + $hour
            modeIndex = [int]$schedule.params.modeStartIndex + $hour
        }
    }
}
Add-Node "schedule_select" "scheduleSelect" ([ordered]@{
    scheduleCurve = $scheduleCurve
    powerOutputIndex = [int]$schedule.params.powerNowOutput
    socOutputIndex = [int]$schedule.params.socNowOutput
    modeOutputIndex = $scheduleMode
})
$schedulePowerAbs = New-Index
$scheduleLimitMinusOne = New-Index
$scheduleVMinMinusOne = New-Index
$scheduleVMaxPlusOne = New-Index
$doubleGrad = New-Index
Add-Formula "schedule_power_abs" "abs" @((Input-Index ([int]$schedule.params.powerNowOutput))) $schedulePowerAbs
Add-Formula "schedule_limit_minus_one" "subtract" @((Input-Index $schedulePowerAbs), (Input-Value 1)) $scheduleLimitMinusOne
Add-Formula "schedule_vmin_minus_one" "subtract" @((Input-Index ([int]$schedule.params.vMinIndex)), (Input-Value 1)) $scheduleVMinMinusOne
Add-Formula "schedule_vmax_plus_one" "add" @((Input-Index ([int]$schedule.params.vMaxIndex)), (Input-Value 1)) $scheduleVMaxPlusOne
Add-Formula "schedule_double_grad" "multiply" @((Input-Index ([int]$schedule.params.gradPIndex)), (Input-Value 2)) $doubleGrad

$scheduleChargeGate = New-Index
$scheduleDischargeGate = New-Index
Add-Node "schedule_charge_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$schedule.params.powerNowOutput; operator = "gt"; value = 0 },
        [ordered]@{ index = [int]$schedule.params.bmsSocIndex; operator = "lt"; valueIndex = [int]$schedule.params.socNowOutput }
    )
    outputIndex = $scheduleChargeGate
})
Add-Node "schedule_discharge_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$schedule.params.powerNowOutput; operator = "lt"; value = 0 },
        [ordered]@{ index = [int]$schedule.params.bmsSocIndex; operator = "gt"; valueIndex = [int]$schedule.params.socNowOutput }
    )
    outputIndex = $scheduleDischargeGate
})

$schedulePhaseOutputs = @([int]$schedule.params.paOutput, [int]$schedule.params.pbOutput, [int]$schedule.params.pcOutput)
$schedulePhaseRawOutputs = @((New-Index), (New-Index), (New-Index))
$scheduleVoltages = @([int]$schedule.params.cnUaIndex, [int]$schedule.params.cnUbIndex, [int]$schedule.params.cnUcIndex)
$scheduleRealtimeVoltages = @($schedule.params.cnRealtimeUIndexes | ForEach-Object { [int]$_ })
$scheduleOldOutputs = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $oldValue = New-Index
    Add-Formula "schedule_${phase}_previous" "add" @((Input-Index $schedulePhaseOutputs[$phase] 0)) $oldValue
    $scheduleOldOutputs += $oldValue
}
$scheduleOldP3 = New-Index
Add-Formula "schedule_total_previous" "add" @((Input-Index ([int]$schedule.params.p3Output) 0)) $scheduleOldP3
$chargeProvisional = @()
$dischargeProvisional = @()
$decayValues = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $oldOutput = $scheduleOldOutputs[$phase]
    $chargeIncrement = New-Index
    $chargeVoltageStep = New-Index
    $chargeRamp = New-Index
    Add-Formula "schedule_charge_${phase}_increment" "add" @((Input-Index $oldOutput 0), (Input-Index ([int]$schedule.params.gradPIndex))) $chargeIncrement
    Add-Switch "schedule_charge_${phase}_voltage" ([ordered]@{
        leftIndex = $scheduleVoltages[$phase]; operator = "gt"; rightIndex = [int]$schedule.params.vMinIndex
    }) ([ordered]@{ trueIndex = $chargeIncrement }) ([ordered]@{ falseIndex = $oldOutput }) $chargeVoltageStep
    Add-Switch "schedule_charge_${phase}_ramp" ([ordered]@{
        leftIndex = $scheduleOldP3; operator = "lt"; rightIndex = $scheduleLimitMinusOne
    }) ([ordered]@{ trueIndex = $chargeVoltageStep }) ([ordered]@{ falseIndex = $oldOutput }) $chargeRamp
    $chargeProvisional += $chargeRamp

    $dischargeIncrement = New-Index
    $dischargeVoltageStep = New-Index
    $dischargeRamp = New-Index
    Add-Formula "schedule_discharge_${phase}_increment" "subtract" @((Input-Index $oldOutput 0), (Input-Index ([int]$schedule.params.gradPIndex))) $dischargeIncrement
    Add-Switch "schedule_discharge_${phase}_voltage" ([ordered]@{
        leftIndex = $scheduleVoltages[$phase]; operator = "lt"; rightIndex = [int]$schedule.params.vMaxIndex
    }) ([ordered]@{ trueIndex = $dischargeIncrement }) ([ordered]@{ falseIndex = $oldOutput }) $dischargeVoltageStep
    Add-Switch "schedule_discharge_${phase}_ramp" ([ordered]@{
        leftIndex = $scheduleOldP3; operator = "lt"; rightIndex = $scheduleLimitMinusOne
    }) ([ordered]@{ trueIndex = $dischargeVoltageStep }) ([ordered]@{ falseIndex = $oldOutput }) $dischargeRamp
    $dischargeProvisional += $dischargeRamp

    $decayPositiveRaw = New-Index
    $decayPositive = New-Index
    $decayNegativeRaw = New-Index
    $decayNegative = New-Index
    $decayNegativeSelect = New-Index
    $decay = New-Index
    Add-Formula "schedule_decay_${phase}_positive_raw" "subtract" @((Input-Index $oldOutput 0), (Input-Index $doubleGrad)) $decayPositiveRaw
    Add-Formula "schedule_decay_${phase}_positive" "max" @((Input-Index $decayPositiveRaw), (Input-Value 0)) $decayPositive
    Add-Formula "schedule_decay_${phase}_negative_raw" "add" @((Input-Index $oldOutput 0), (Input-Index $doubleGrad)) $decayNegativeRaw
    Add-Formula "schedule_decay_${phase}_negative" "min" @((Input-Index $decayNegativeRaw), (Input-Value 0)) $decayNegative
    Add-Switch "schedule_decay_${phase}_negative_select" ([ordered]@{
        leftIndex = $oldOutput; operator = "lt"; rightValue = 0
    }) ([ordered]@{ trueIndex = $decayNegative }) ([ordered]@{ falseIndex = $oldOutput }) $decayNegativeSelect
    Add-Switch "schedule_decay_${phase}" ([ordered]@{
        leftIndex = $oldOutput; operator = "gt"; rightValue = 0
    }) ([ordered]@{ trueIndex = $decayPositive }) ([ordered]@{ falseIndex = $decayNegativeSelect }) $decay
    $decayValues += $decay
}

function Add-AbsoluteSum(
    [string]$Prefix,
    [int[]]$Indexes,
    [int]$OutputIndex,
    [bool]$DefaultMissingToZero = $false
) {
    $absoluteIndexes = @()
    for ($i = 0; $i -lt $Indexes.Count; $i++) {
        $absolute = New-Index
        $input = if ($DefaultMissingToZero) { Input-Index $Indexes[$i] 0 } else { Input-Index $Indexes[$i] }
        Add-Formula "${Prefix}_${i}_abs" "abs" @($input) $absolute
        $absoluteIndexes += $absolute
    }
    Add-Formula "${Prefix}_sum" "add" @((Input-Index $absoluteIndexes[0]), (Input-Index $absoluteIndexes[1]), (Input-Index $absoluteIndexes[2])) $OutputIndex
}

$chargeProvisionalSum = New-Index
$dischargeProvisionalSum = New-Index
Add-AbsoluteSum "schedule_charge_provisional" $chargeProvisional $chargeProvisionalSum
Add-AbsoluteSum "schedule_discharge_provisional" $dischargeProvisional $dischargeProvisionalSum
$scheduleThirdLimit = New-Index
$scheduleNegativeThirdLimit = New-Index
Add-Formula "schedule_third_limit" "multiply" @((Input-Index $schedulePowerAbs), (Input-Value 0.3333)) $scheduleThirdLimit
Add-Formula "schedule_negative_third_limit" "negate" @((Input-Index $scheduleThirdLimit)) $scheduleNegativeThirdLimit

for ($phase = 0; $phase -lt 3; $phase++) {
    $chargeCapped = New-Index
    $chargeLowAdjustedRaw = New-Index
    $chargeLowAdjusted = New-Index
    $chargeValue = New-Index
    Add-Switch "schedule_charge_${phase}_cap" ([ordered]@{
        leftIndex = $chargeProvisionalSum; operator = "gt"; rightIndex = $schedulePowerAbs
    }) ([ordered]@{ trueIndex = $scheduleThirdLimit }) ([ordered]@{ falseIndex = $chargeProvisional[$phase] }) $chargeCapped
    Add-Formula "schedule_charge_${phase}_low_raw" "subtract" @((Input-Index $chargeCapped), (Input-Index ([int]$schedule.params.gradPIndex))) $chargeLowAdjustedRaw
    Add-Formula "schedule_charge_${phase}_low_clamp" "max" @((Input-Index $chargeLowAdjustedRaw), (Input-Value 0)) $chargeLowAdjusted
    Add-Switch "schedule_charge_${phase}_low_select" ([ordered]@{
        leftIndex = $scheduleVoltages[$phase]; operator = "lt"; rightIndex = $scheduleVMinMinusOne
    }) ([ordered]@{ trueIndex = $chargeLowAdjusted }) ([ordered]@{ falseIndex = $chargeCapped }) $chargeValue

    $dischargeCapped = New-Index
    $dischargeHighAdjustedRaw = New-Index
    $dischargeHighAdjusted = New-Index
    $dischargeValue = New-Index
    Add-Switch "schedule_discharge_${phase}_cap" ([ordered]@{
        leftIndex = $dischargeProvisionalSum; operator = "gt"; rightIndex = $schedulePowerAbs
    }) ([ordered]@{ trueIndex = $scheduleNegativeThirdLimit }) ([ordered]@{ falseIndex = $dischargeProvisional[$phase] }) $dischargeCapped
    Add-Formula "schedule_discharge_${phase}_high_raw" "add" @((Input-Index $dischargeCapped), (Input-Index ([int]$schedule.params.gradPIndex))) $dischargeHighAdjustedRaw
    Add-Formula "schedule_discharge_${phase}_high_clamp" "min" @((Input-Index $dischargeHighAdjustedRaw), (Input-Value 0)) $dischargeHighAdjusted
    Add-Switch "schedule_discharge_${phase}_high_select" ([ordered]@{
        leftIndex = $scheduleVoltages[$phase]; operator = "gt"; rightIndex = $scheduleVMaxPlusOne
    }) ([ordered]@{ trueIndex = $dischargeHighAdjusted }) ([ordered]@{ falseIndex = $dischargeCapped }) $dischargeValue

    $activeValue = New-Index
    Add-Switch "schedule_${phase}_discharge_or_decay" ([ordered]@{ conditionIndex = $scheduleDischargeGate }) ([ordered]@{ trueIndex = $dischargeValue }) ([ordered]@{ falseIndex = $decayValues[$phase] }) $activeValue
    Add-Switch "schedule_${phase}_raw_output" ([ordered]@{ conditionIndex = $scheduleChargeGate }) ([ordered]@{ trueIndex = $chargeValue }) ([ordered]@{ falseIndex = $activeValue }) $schedulePhaseRawOutputs[$phase]
}
$scheduleNegativePMax = New-Index
Add-Formula "schedule_negative_pmax" "negate" @((Input-Index ([int]$schedule.params.pMaxIndex))) $scheduleNegativePMax
for ($phase = 0; $phase -lt 3; $phase++) {
    $derated = New-Index
    $chargeDerateGate = New-Index
    $dischargeDerateGate = New-Index
    $chargeSelected = New-Index
    $derateSelected = New-Index
    Add-Formula "schedule_${phase}_instant_derated" "multiply" @((Input-Index $schedulePhaseRawOutputs[$phase]), (Input-Value 0.8)) $derated
    Add-Node "schedule_${phase}_charge_derate_gate" "controlGate" ([ordered]@{
        combine = "all"
        conditions = @(
            [ordered]@{ index = $schedulePhaseRawOutputs[$phase]; operator = "gt"; value = 0 },
            [ordered]@{ index = $scheduleRealtimeVoltages[$phase]; operator = "lt"; valueIndex = [int]$schedule.params.vMinIndex }
        )
        outputIndex = $chargeDerateGate
    })
    Add-Node "schedule_${phase}_discharge_derate_gate" "controlGate" ([ordered]@{
        combine = "all"
        conditions = @(
            [ordered]@{ index = $schedulePhaseRawOutputs[$phase]; operator = "lt"; value = 0 },
            [ordered]@{ index = $scheduleRealtimeVoltages[$phase]; operator = "gt"; valueIndex = [int]$schedule.params.vMaxIndex }
        )
        outputIndex = $dischargeDerateGate
    })
    Add-Switch "schedule_${phase}_charge_derate" ([ordered]@{ conditionIndex = $chargeDerateGate }) ([ordered]@{ trueIndex = $derated }) ([ordered]@{ falseIndex = $schedulePhaseRawOutputs[$phase] }) $chargeSelected
    Add-Switch "schedule_${phase}_discharge_derate" ([ordered]@{ conditionIndex = $dischargeDerateGate }) ([ordered]@{ trueIndex = $derated }) ([ordered]@{ falseIndex = $chargeSelected }) $derateSelected
    Add-Formula "schedule_${phase}_output" "clamp" @((Input-Index $derateSelected)) $schedulePhaseOutputs[$phase] @{
        lowerIndex = $scheduleNegativePMax; upperIndex = [int]$schedule.params.pMaxIndex
    }
}
$scheduleP3Output = [int]$schedule.params.p3Output
$scheduleRunOutput = [int]$schedule.params.runOutput
Add-AbsoluteSum "schedule_output" $schedulePhaseOutputs $scheduleP3Output
Add-Switch "schedule_run" ([ordered]@{ leftIndex = $scheduleP3Output; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) $scheduleRunOutput
Set-NodeProfiles $scheduleStart -Required @("Meter_TQ", "Meter_CN")

$cycle = Source-Node "charge_discharge_test"
$cycleStart = $nodes.Count
$cyclePowerInputs = @()
$hasPerPhasePower = $true
foreach ($phaseName in @("A", "B", "C")) {
    $indexProperty = $cycle.params.PSObject.Properties["phasePower${phaseName}Index"]
    $valueProperty = $cycle.params.PSObject.Properties["phasePower${phaseName}"]
    if ($null -ne $indexProperty -and [int]$indexProperty.Value -gt 0) {
        $cyclePowerInputs += ,(Input-Index ([int]$indexProperty.Value))
    } elseif ($null -ne $valueProperty) {
        $cyclePowerInputs += ,(Input-Value ([double]$valueProperty.Value))
    } else {
        $hasPerPhasePower = $false
        break
    }
}
if (-not $hasPerPhasePower) {
    $totalIndexProperty = $cycle.params.PSObject.Properties["totalPowerIndex"]
    $totalValueProperty = $cycle.params.PSObject.Properties["totalPower"]
    if ($null -ne $totalIndexProperty -and [int]$totalIndexProperty.Value -gt 0) {
        $totalInput = Input-Index ([int]$totalIndexProperty.Value)
    } elseif ($null -ne $totalValueProperty) {
        $totalInput = Input-Value ([double]$totalValueProperty.Value)
    } else {
        throw "charge_discharge_test requires three phase powers or totalPower"
    }
    $totalAbsolute = New-Index
    $perPhase = New-Index
    Add-Formula "cycle_total_abs" "abs" @($totalInput) $totalAbsolute
    Add-Formula "cycle_per_phase" "safeDivide" @((Input-Index $totalAbsolute), (Input-Value 3)) $perPhase
    $cyclePowerInputs = @((Input-Index $perPhase), (Input-Index $perPhase), (Input-Index $perPhase))
}

$dischargeDepth = [double]$cycle.params.dischargeDepth
$chargeDepth = [double]$cycle.params.chargeDepth
if ($chargeDepth -le $dischargeDepth) {
    throw "charge_discharge_test chargeDepth must be greater than dischargeDepth"
}
$cycleStateOutput = [int]$cycle.params.phaseStateOutput
Add-Node "cycle_phase_sequence" "sequence" ([ordered]@{
    stateOutputIndex = $cycleStateOutput
    initialState = 1
    evaluateOnInitialize = $true
    states = @(
        [ordered]@{ id = 1; name = "放电" },
        [ordered]@{ id = 2; name = "充电" }
    )
    transitions = @(
        [ordered]@{
            from = 1; to = 2; name = "放电至下限"; minDurationMs = 0
            conditions = @([ordered]@{ index = [int]$cycle.params.bmsSocIndex; operator = "lte"; value = $dischargeDepth })
        },
        [ordered]@{
            from = 2; to = 1; name = "充电至上限"; minDurationMs = 0
            conditions = @([ordered]@{ index = [int]$cycle.params.bmsSocIndex; operator = "gte"; value = $chargeDepth })
        }
    )
})

$cycleOutputs = @([int]$cycle.params.paOutput, [int]$cycle.params.pbOutput, [int]$cycle.params.pcOutput)
for ($phase = 0; $phase -lt 3; $phase++) {
    $magnitude = New-Index
    $negative = New-Index
    Add-Formula "cycle_phase_${phase}_magnitude" "abs" @($cyclePowerInputs[$phase]) $magnitude
    Add-Formula "cycle_phase_${phase}_negative" "negate" @((Input-Index $magnitude)) $negative
    Add-Switch "cycle_phase_${phase}_output" ([ordered]@{ leftIndex = $cycleStateOutput; operator = "eq"; rightValue = 1 }) ([ordered]@{ trueIndex = $negative }) ([ordered]@{ falseIndex = $magnitude }) $cycleOutputs[$phase]
}
$cycleP3Output = [int]$cycle.params.p3Output
$cycleRunOutput = [int]$cycle.params.runOutput
Add-AbsoluteSum "cycle_output" $cycleOutputs $cycleP3Output
Add-Switch "cycle_run" ([ordered]@{ leftIndex = $cycleP3Output; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) $cycleRunOutput

$cycleProfileParam = if ($cycle.enabled) { "profileKey" } else { "optionalProfileKey" }
for ($i = $cycleStart; $i -lt $nodes.Count; $i++) {
    $nodes[$i].params[$cycleProfileParam] = "CHARGE_DISCHARGE_TEST"
}

$solarStart = $nodes.Count
$solar = Source-Node "gf"
$hourIndex = New-Index
$solarTimeGate = New-Index
Add-Node "local_hour" "timeSource" ([ordered]@{ component = "hour"; outputIndex = $hourIndex })
Add-Node "solar_time_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = $hourIndex; operator = "gte"; valueIndex = [int]$solar.params.startHourIndex },
        [ordered]@{ index = $hourIndex; operator = "lte"; valueIndex = [int]$solar.params.endHourIndex }
    )
    outputIndex = $solarTimeGate
})
$solarLoads = @([int]$solar.params.fhPaIndex, [int]$solar.params.fhPbIndex, [int]$solar.params.fhPcIndex)
$solarOutputs = @([int]$solar.params.paOutput, [int]$solar.params.pbOutput, [int]$solar.params.pcOutput)
$solarTrackingPerPhase = New-Index
Add-Formula "solar_tracking_per_phase" "safeDivide" @((Input-Index ([int]$solar.params.trackingPowerIndex)), (Input-Value 3)) $solarTrackingPerPhase
for ($phase = 0; $phase -lt 3; $phase++) {
    $surplus = New-Index
    $phaseAvailable = New-Index
    Add-Formula "solar_${phase}_surplus" "subtract" @((Input-Index $solarTrackingPerPhase), (Input-Index $solarLoads[$phase])) $surplus
    Add-Switch "solar_${phase}_available" ([ordered]@{
        leftIndex = $solarLoads[$phase]; operator = "lte"; rightIndex = $solarTrackingPerPhase
    }) ([ordered]@{ trueIndex = $surplus }) ([ordered]@{ falseValue = 0 }) $phaseAvailable
    Add-Switch "solar_${phase}_output" ([ordered]@{ conditionIndex = $solarTimeGate }) ([ordered]@{ trueIndex = $phaseAvailable }) ([ordered]@{ falseValue = 0 }) $solarOutputs[$phase]
}
Add-AbsoluteSum "solar_output" $solarOutputs ([int]$solar.params.p3Output)
Add-Switch "solar_run" ([ordered]@{ leftIndex = [int]$solar.params.p3Output; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$solar.params.runOutput)
Set-NodeProfiles $solarStart -Required @("Meter_TQ", "Meter_CN")

$balanceStart = $nodes.Count
$balance = Source-Node "ph"
$balanceValues = @([int]$balance.params.tqCnPaOutput, [int]$balance.params.tqCnPbOutput, [int]$balance.params.tqCnPcOutput)
$balanceTq = @([int]$balance.params.tqPaIndex, [int]$balance.params.tqPbIndex, [int]$balance.params.tqPcIndex)
$balanceLoads = @([int]$balance.params.fhPaIndex, [int]$balance.params.fhPbIndex, [int]$balance.params.fhPcIndex)
$balanceOutputs = @([int]$balance.params.paOutput, [int]$balance.params.pbOutput, [int]$balance.params.pcOutput)
for ($phase = 0; $phase -lt 3; $phase++) {
    Add-Formula "balance_phase_${phase}_net" "subtract" @((Input-Index $balanceTq[$phase]), (Input-Index $balanceOutputs[$phase] 0)) $balanceValues[$phase]
}
$balanceAllowed = New-Index
$balanceLoadAbs = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $absolute = New-Index
    Add-Formula "balance_load_${phase}_abs" "abs" @((Input-Index $balanceLoads[$phase])) $absolute
    $balanceLoadAbs += $absolute
}
$balanceAverage = New-Index
$balanceMax = New-Index
$balanceMin = New-Index
$balanceMaxAbs = New-Index
$balanceSpread = New-Index
$balanceRatio = New-Index
Add-Formula "balance_load_average" "average" @((Input-Index $balanceLoads[0]), (Input-Index $balanceLoads[1]), (Input-Index $balanceLoads[2])) $balanceAverage
Add-Formula "balance_max" "max" @((Input-Index $balanceLoads[0]), (Input-Index $balanceLoads[1]), (Input-Index $balanceLoads[2])) $balanceMax
Add-Formula "balance_min" "min" @((Input-Index $balanceLoads[0]), (Input-Index $balanceLoads[1]), (Input-Index $balanceLoads[2])) $balanceMin
Add-Formula "balance_max_abs" "max" @((Input-Index $balanceLoadAbs[0]), (Input-Index $balanceLoadAbs[1]), (Input-Index $balanceLoadAbs[2])) $balanceMaxAbs
Add-Formula "balance_allowed" "multiply" @((Input-Index ([int]$balance.params.balancePercentIndex)), (Input-Index $balanceMaxAbs), (Input-Value 0.01)) $balanceAllowed
Add-Formula "balance_spread" "subtract" @((Input-Index $balanceMax), (Input-Index $balanceMin)) $balanceSpread
Add-Formula "balance_ratio" "safeDivide" @((Input-Index $balanceSpread), (Input-Index $balanceMaxAbs)) $balanceRatio
Add-Formula "balance_percent" "multiply" @((Input-Index $balanceRatio), (Input-Value 100)) ([int]$balance.params.balanceOutput)
$balanceEnabled = New-Index
Add-Switch "balance_enabled" ([ordered]@{ leftIndex = $balanceSpread; operator = "gt"; rightIndex = $balanceAllowed }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) $balanceEnabled
for ($phase = 0; $phase -lt 3; $phase++) {
    $correction = New-Index
    Add-Formula "balance_phase_${phase}_correction" "subtract" @((Input-Index $balanceAverage), (Input-Index $balanceLoads[$phase])) $correction
    Add-Switch "balance_phase_${phase}_output" ([ordered]@{ conditionIndex = $balanceEnabled }) ([ordered]@{ trueIndex = $correction }) ([ordered]@{ falseValue = 0 }) $balanceOutputs[$phase]
}
$balanceTotal = New-Index
Add-AbsoluteSum "balance_output" $balanceOutputs $balanceTotal
Add-Switch "balance_run" ([ordered]@{ leftIndex = $balanceTotal; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$balance.params.runOutput)
Set-NodeProfiles $balanceStart -Required @("Meter_TQ", "Meter_CN")

$skStart = $nodes.Count
$sk = Source-Node "sk"
$skPAbs = New-Index
$skQAbs = New-Index
$skTotal = New-Index
Add-Formula "override_active_abs" "abs" @((Input-Index ([int]$sk.params.skP3Index))) $skPAbs
Add-Formula "override_reactive_abs" "abs" @((Input-Index ([int]$sk.params.skQ3Index))) $skQAbs
Add-Formula "override_total" "add" @((Input-Index $skPAbs), (Input-Index $skQAbs)) $skTotal
Add-Switch "override_run" ([ordered]@{ leftIndex = $skTotal; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$sk.params.runOutput)
Set-NodeProfiles $skStart -Required @("Meter_TQ")

$force = Source-Node "force_full_charge"
$forceDayOfMonth = New-Index
$forceScheduledDate = New-Index
$forceArmPermit = New-Index
$forceState = New-Index
$forcePhasePower = New-Index
Add-Node "force_full_day_of_month" "timeSource" ([ordered]@{
    component = "dayOfMonth"
    outputIndex = $forceDayOfMonth
})
Add-Node "force_full_scheduled_date" "controlGate" ([ordered]@{
    combine = "any"
    conditions = @(
        [ordered]@{ index = $forceDayOfMonth; operator = "eq"; valueIndex = [int]$force.params.date1Index },
        [ordered]@{ index = $forceDayOfMonth; operator = "eq"; valueIndex = [int]$force.params.date2Index }
    )
    outputIndex = $forceScheduledDate
})
$forceArmConditions = @(
    [ordered]@{ index = [int]$force.params.enableIndex; operator = "eq"; value = 1 },
    [ordered]@{ index = [int]$force.params.masterAutoIndex; operator = "eq"; value = 1 },
    [ordered]@{ index = $forceScheduledDate; operator = "eq"; value = 1 },
    [ordered]@{ index = [int]$force.params.bmsSocIndex; operator = "lt"; value = [double]$force.params.targetSoc }
)
foreach ($runIndex in $force.params.conflictRunIndexes) {
    $forceArmConditions += [ordered]@{ index = [int]$runIndex; operator = "eq"; value = 0 }
}
Add-Node "force_full_arm_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = $forceArmConditions
    outputIndex = $forceArmPermit
})
Add-Node "force_full_sequence" "sequence" ([ordered]@{
    stateOutputIndex = $forceState
    initialState = 0
    evaluateOnInitialize = $true
    states = @(
        [ordered]@{ id = 0; name = "等待指定日期" },
        [ordered]@{ id = 1; name = "强制满充中" },
        [ordered]@{ id = 2; name = "本次满充已完成" }
    )
    transitions = @(
        [ordered]@{
            from = 0; to = 1; name = "指定日期满足启动条件"; minDurationMs = 0
            conditions = @([ordered]@{ index = $forceArmPermit; operator = "eq"; value = 1 })
        },
        [ordered]@{
            from = 1; to = 2; name = "SOC 达到 100% 或操作员停用"; minDurationMs = 0; combine = "any"
            conditions = @(
                [ordered]@{ index = [int]$force.params.bmsSocIndex; operator = "gte"; value = [double]$force.params.targetSoc },
                [ordered]@{ index = [int]$force.params.enableIndex; operator = "eq"; value = 0 }
            )
        },
        [ordered]@{
            from = 2; to = 0; name = "离开指定日期后允许下一次触发"; minDurationMs = 0
            conditions = @([ordered]@{ index = $forceScheduledDate; operator = "eq"; value = 0 })
        }
    )
})
Add-Switch "force_full_run" ([ordered]@{ leftIndex = $forceState; operator = "eq"; rightValue = 1 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$force.params.runOutput)
Add-Formula "force_full_phase_power" "multiply" @(
    (Input-Index ([int]$force.params.pMaxIndex)),
    (Input-Value ([double]$force.params.phasePowerFactor))
) $forcePhasePower

$solve = Source-Node "power_solve"
$arbiterActive = @((New-Index), (New-Index), (New-Index))
$arbiterReactive = @((New-Index), (New-Index), (New-Index))
Add-Node "power_arbiter" "phaseArbiter" ([ordered]@{
    activeBaseIndexes = @([int]$solve.params.outPaDsIndex, [int]$solve.params.outPbDsIndex, [int]$solve.params.outPcDsIndex)
    reactiveBaseIndexes = @([int]$solve.params.outQaCosIndex, [int]$solve.params.outQbCosIndex, [int]$solve.params.outQcCosIndex)
    activeOutputIndexes = $arbiterActive
    reactiveOutputIndexes = $arbiterReactive
    candidates = @(
        [ordered]@{ name = "charge"; target = "active"; merge = "stronger"; direction = "positive"; totalIndex = (Param-Int $solve.params "outP3CdIndex" 613) },
        [ordered]@{ name = "discharge"; target = "active"; merge = "stronger"; direction = "negative"; totalIndex = (Param-Int $solve.params "outP3FdIndex" 614) },
        [ordered]@{ name = "low_voltage"; target = "active"; merge = "min"; direction = "negative"; indexes = @([int]$solve.params.outPaLvIndex, [int]$solve.params.outPbLvIndex, [int]$solve.params.outPcLvIndex); runOutputIndex = (Param-Int $solve.params "lvRunOutput" 10) },
        [ordered]@{ name = "high_voltage"; target = "active"; merge = "max"; direction = "positive"; indexes = @([int]$solve.params.outPaHvIndex, [int]$solve.params.outPbHvIndex, [int]$solve.params.outPcHvIndex); runOutputIndex = (Param-Int $solve.params "hvRunOutput" 12) },
        [ordered]@{ name = "solar"; target = "active"; merge = "max"; direction = "positive"; indexes = @([int]$solve.params.outPaGfIndex, [int]$solve.params.outPbGfIndex, [int]$solve.params.outPcGfIndex) },
        [ordered]@{ name = "phase_balance"; target = "active"; merge = "add"; direction = "any"; indexes = @([int]$solve.params.outPaPhIndex, [int]$solve.params.outPbPhIndex, [int]$solve.params.outPcPhIndex) }
    )
    override = [ordered]@{
        enableIndex = [int]$solve.params.skRunIndex
        enableValue = 1
        activeTotalIndex = [int]$solve.params.skP3Index
        reactiveTotalIndex = [int]$solve.params.skQ3Index
    }
})
$constraintActive = @()
$constraintReactive = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $selectedActive = New-Index
    $selectedReactive = New-Index
    Add-Switch "force_full_active_${phase}" ([ordered]@{ conditionIndex = [int]$force.params.runOutput }) ([ordered]@{ trueIndex = $forcePhasePower }) ([ordered]@{ falseIndex = $arbiterActive[$phase] }) $selectedActive
    Add-Switch "force_full_reactive_${phase}" ([ordered]@{ conditionIndex = [int]$force.params.runOutput }) ([ordered]@{ trueValue = 0 }) ([ordered]@{ falseIndex = $arbiterReactive[$phase] }) $selectedReactive
    $constraintActive += $selectedActive
    $constraintReactive += $selectedReactive
}
$effectiveSocUpper = New-Index
Add-Switch "force_full_soc_upper" ([ordered]@{ conditionIndex = [int]$force.params.runOutput }) ([ordered]@{ trueValue = [double]$force.params.targetSoc }) ([ordered]@{ falseIndex = [int]$solve.params.bmsSocMaxIndex }) $effectiveSocUpper
Add-Node "power_constraints" "powerConstraint" ([ordered]@{
    activeInputIndexes = $constraintActive
    reactiveInputIndexes = $constraintReactive
    activeOutputIndexes = @([int]$solve.params.paOutput, [int]$solve.params.pbOutput, [int]$solve.params.pcOutput)
    reactiveOutputIndexes = @([int]$solve.params.qaOutput, [int]$solve.params.qbOutput, [int]$solve.params.qcOutput)
    loadIndexes = @([int]$solve.params.fhPaIndex, [int]$solve.params.fhPbIndex, [int]$solve.params.fhPcIndex)
    positiveLimitEnableIndex = [int]$solve.params.positiveLimitEnableIndex
    positiveLimitIndex = [int]$solve.params.positiveLimitIndex
    negativeLimitEnableIndex = [int]$solve.params.negativeLimitEnableIndex
    negativeLimitIndex = [int]$solve.params.negativeLimitIndex
    reserveEnableIndex = [int]$solve.params.zrEnableIndex
    reserveMarginIndex = [int]$solve.params.zrP1Index
    reserveRunOutputIndex = (Param-Int $solve.params "zrRunOutput" 24)
    activeAbsLimitIndex = [int]$solve.params.pMaxIndex
    reactiveAbsLimitIndex = [int]$solve.params.qMaxIndex
    apparentTotalLimitIndex = [int]$solve.params.s3MaxIndex
    positiveTotalLimitIndex = [int]$solve.params.chargeKwAllowIndex
    negativeTotalLimitIndex = [int]$solve.params.dischargeKwAllowIndex
    stateIndex = [int]$solve.params.bmsSocIndex
    stateUpperIndex = $effectiveSocUpper
    stateLowerIndex = [int]$solve.params.bmsSocMinIndex
    lowStateClearReactive = $true
    lowStateClearIndexes = @((Param-Int $solve.params "cosRunOutput" 8), (Param-Int $solve.params "lvRunOutput" 10))
    highStateClearIndexes = @((Param-Int $solve.params "hvRunOutput" 12), (Param-Int $solve.params "gfRunOutput" 22))
})

$energy = Source-Node "energy_saving"
$energyActiveTotal = New-Index
$energyReactiveTotal = New-Index
$energyDemandTotal = New-Index
Add-AbsoluteSum "energy_active_output" @([int]$solve.params.paOutput, [int]$solve.params.pbOutput, [int]$solve.params.pcOutput) $energyActiveTotal $true
Add-AbsoluteSum "energy_reactive_output" @([int]$solve.params.qaOutput, [int]$solve.params.qbOutput, [int]$solve.params.qcOutput) $energyReactiveTotal $true
Add-Formula "energy_output_total" "add" @((Input-Index $energyActiveTotal), (Input-Index $energyReactiveTotal)) $energyDemandTotal
Add-Switch "energy_strategy_run" ([ordered]@{ leftIndex = $energyDemandTotal; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$energy.params.strategyRunOutputIndex)
$energyCommandOne = New-Index
$energyCommandZero = New-Index
Add-Formula "energy_command_one" "add" @((Input-Value 1), (Input-Value 0)) $energyCommandOne
Add-Formula "energy_command_zero" "add" @((Input-Value 0), (Input-Value 0)) $energyCommandZero

$pcsEnergyStart = $nodes.Count
$pcsStartPermit = New-Index
Add-Node "pcs_energy_start_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$energy.params.pcsEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.pcsComStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.strategyRunOutputIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.pcsStopStatusIndex; operator = "eq"; value = [double]$energy.params.pcsStoppedValue },
        [ordered]@{ index = [int]$energy.params.pcsFaultIndex; operator = "eq"; value = 0 }
    )
    outputIndex = $pcsStartPermit
})
Add-Node "pcs_energy_start" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $energyCommandOne
    targetIndex = [int]$energy.params.pcsStartIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $pcsStartPermit
    permitValue = 1
    deadband = 0
})
$pcsStopPermit = New-Index
Add-Node "pcs_energy_stop_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$energy.params.pcsEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.pcsComStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.strategyRunOutputIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$energy.params.pcsRunStatusIndex; operator = "eq"; value = [double]$energy.params.pcsRunningValue }
    )
    outputIndex = $pcsStopPermit
})
Add-Node "pcs_energy_stop" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $energyCommandOne
    targetIndex = [int]$energy.params.pcsStopIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $pcsStopPermit
    permitValue = 1
    deadband = 0
})
Set-NodeProfiles $pcsEnergyStart -Optional @([string]$energy.params.pcsProfileKey) -Disabled @("CHARGE_DISCHARGE_TEST")

$pcsResetStart = $nodes.Count
$pcsResetPermit = New-Index
Add-Node "pcs_auto_reset_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$energy.params.pcsComStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.strategyRunOutputIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.pcsFaultIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.pcsResetIndex; operator = "eq"; value = 0 }
    )
    outputIndex = $pcsResetPermit
})
Add-Node "pcs_auto_reset" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $energyCommandOne
    targetIndex = [int]$energy.params.pcsResetIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $pcsResetPermit
    permitValue = 1
    deadband = 0
})
Set-NodeProfiles $pcsResetStart -Optional @([string]$energy.params.pcsAutoResetProfileKey)

$liquidEnergyStart = $nodes.Count
$liquidStartPermit = New-Index
Add-Node "liquid_energy_start_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$energy.params.liquidEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.liquidComStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.strategyRunOutputIndex; operator = "eq"; value = 1 }
    )
    outputIndex = $liquidStartPermit
})
Add-Node "liquid_energy_start" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $energyCommandOne
    targetIndex = [int]$energy.params.liquidRemoteControlIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $liquidStartPermit
    permitValue = 1
    deadband = 0
})
$liquidScaledTemperature = New-Index
Add-Formula "liquid_energy_temperature_scaled" "multiply" @((Input-Index ([int]$energy.params.liquidTargetTemperatureIndex)), (Input-Value ([double]$energy.params.liquidTemperatureScale))) $liquidScaledTemperature
Add-Node "liquid_energy_temperature" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $liquidScaledTemperature
    targetIndex = [int]$energy.params.liquidTemperatureSetpointIndex
    valueMode = "round"
    minValue = -1000
    maxValue = 2000
    permitIndex = $liquidStartPermit
    permitValue = 1
    deadband = 0
})
$liquidStopTemperature = New-Index
Add-Formula "liquid_energy_stop_temperature" "subtract" @((Input-Index ([int]$energy.params.liquidTargetTemperatureIndex)), (Input-Value ([double]$energy.params.liquidStopTemperatureDelta))) $liquidStopTemperature
$liquidStopPermit = New-Index
Add-Node "liquid_energy_stop_permit" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$energy.params.liquidEnableIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.liquidComStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$energy.params.strategyRunOutputIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$energy.params.liquidActualReturnTemperatureIndex; operator = "lt"; valueIndex = $liquidStopTemperature },
        [ordered]@{ index = [int]$energy.params.liquidRemoteControlIndex; operator = "eq"; value = 1 }
    )
    outputIndex = $liquidStopPermit
})
Add-Node "liquid_energy_stop" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $energyCommandZero
    targetIndex = [int]$energy.params.liquidRemoteControlIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $liquidStopPermit
    permitValue = 1
    deadband = 0
})
Set-NodeProfiles $liquidEnergyStart -Optional @([string]$energy.params.liquidProfileKey)

$cycleSafetyStart = $nodes.Count
$safety = $cycle.params.safety
$cycleBasicSafety = New-Index
Add-Node "cycle_basic_safety_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = [int]$safety.comStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.pcsFaultIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$safety.bmsFaultIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$safety.remoteModeIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.gridConnectedIndex; operator = "eq"; value = 1 }
    )
    outputIndex = $cycleBasicSafety
})
$chargeCommandAllowed = New-Index
$chargeCommandConditions = @()
foreach ($value in $safety.chargeCommandValues) {
    $chargeCommandConditions += [ordered]@{ index = [int]$safety.bmsCommandIndex; operator = "eq"; value = [double]$value }
}
Add-Node "cycle_charge_command_allowed" "controlGate" ([ordered]@{
    combine = "any"; conditions = $chargeCommandConditions; outputIndex = $chargeCommandAllowed
})
$chargeDirectionAllowed = New-Index
Add-Node "cycle_charge_direction_allowed" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = $cycleBasicSafety; operator = "eq"; value = 1 },
        [ordered]@{ index = $cycleStateOutput; operator = "eq"; value = 2 },
        [ordered]@{ index = [int]$safety.chargeAllowIndex; operator = "gt"; value = 0 },
        [ordered]@{ index = [int]$safety.bmsAlarmIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$safety.cloudEmergencyStopIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = $chargeCommandAllowed; operator = "eq"; value = 1 }
    )
    outputIndex = $chargeDirectionAllowed
})
$dischargeCommandAllowed = New-Index
$dischargeCommandConditions = @()
foreach ($value in $safety.dischargeCommandValues) {
    $dischargeCommandConditions += [ordered]@{ index = [int]$safety.bmsCommandIndex; operator = "eq"; value = [double]$value }
}
Add-Node "cycle_discharge_command_allowed" "controlGate" ([ordered]@{
    combine = "any"; conditions = $dischargeCommandConditions; outputIndex = $dischargeCommandAllowed
})
$dischargeDirectionAllowed = New-Index
Add-Node "cycle_discharge_direction_allowed" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = $cycleBasicSafety; operator = "eq"; value = 1 },
        [ordered]@{ index = $cycleStateOutput; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.dischargeAllowIndex; operator = "gt"; value = 0 },
        [ordered]@{ index = [int]$safety.bmsAlarmIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = [int]$safety.cloudEmergencyStopIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = $dischargeCommandAllowed; operator = "eq"; value = 1 }
    )
    outputIndex = $dischargeDirectionAllowed
})
$cycleDirectionAllowed = New-Index
Add-Node "cycle_direction_allowed" "controlGate" ([ordered]@{
    combine = "any"
    conditions = @(
        [ordered]@{ index = $chargeDirectionAllowed; operator = "eq"; value = 1 },
        [ordered]@{ index = $dischargeDirectionAllowed; operator = "eq"; value = 1 }
    )
    outputIndex = $cycleDirectionAllowed
})
$cycleStartState = New-Index
Add-Node "cycle_start_hold_sequence" "sequence" ([ordered]@{
    stateOutputIndex = $cycleStartState
    initialState = 1
    evaluateOnInitialize = $true
    states = @(
        [ordered]@{ id = 1; name = "启动前安全等待" },
        [ordered]@{ id = 2; name = "保持启动请求" },
        [ordered]@{ id = 3; name = "启动请求结束" }
    )
    transitions = @(
        [ordered]@{
            from = 1; to = 2; name = "安全条件满足，发出启动请求"; minDurationMs = 5000
            conditions = @(
                [ordered]@{ index = $cycleDirectionAllowed; operator = "eq"; value = 1 },
                [ordered]@{ index = [int]$safety.pcsRunStatusIndex; operator = "eq"; value = 0 }
            )
        },
        [ordered]@{
            from = 2; to = 3; name = "运行成功或启动超时后撤销请求"; minDurationMs = 30000
            conditions = @([ordered]@{ index = $cycleStartState; operator = "eq"; value = 2 })
        },
        [ordered]@{
            from = 3; to = 1; name = "安全条件恢复且 PCS 未运行，重新准备启动"; minDurationMs = 10000
            conditions = @(
                [ordered]@{ index = $cycleDirectionAllowed; operator = "eq"; value = 1 },
                [ordered]@{ index = [int]$safety.pcsRunStatusIndex; operator = "eq"; value = 0 }
            )
        }
    )
})
$cycleStartValue = New-Index
Add-Node "cycle_start_hold_value" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = $cycleStartState; operator = "eq"; value = 2 },
        [ordered]@{ index = $cycleDirectionAllowed; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.pcsRunStatusIndex; operator = "eq"; value = 0 }
    )
    outputIndex = $cycleStartValue
})
$cyclePhaseModeZero = New-Index
Add-Formula "cycle_phase_mode_zero" "add" @((Input-Value 0), (Input-Value 0)) $cyclePhaseModeZero
Add-Node "cycle_phase_control_mode" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $cyclePhaseModeZero
    targetIndex = [int]$safety.phaseControlModeIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $cycleDirectionAllowed
    permitValue = 1
    deadband = 0
})
$cycleOperableState = New-Index
Add-Node "cycle_operable_state_gate" "controlGate" ([ordered]@{
    combine = "any"
    conditions = @(
        [ordered]@{ index = [int]$safety.pcsStandbyStatusIndex; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.pcsRunStatusIndex; operator = "eq"; value = 1 }
    )
    outputIndex = $cycleOperableState
})
$cycleRuntimeSafety = New-Index
Add-Node "cycle_runtime_safety_gate" "controlGate" ([ordered]@{
    combine = "all"
    conditions = @(
        [ordered]@{ index = $cycleBasicSafety; operator = "eq"; value = 1 },
        [ordered]@{ index = $cycleDirectionAllowed; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.pcsStopStatusIndex; operator = "eq"; value = 0 },
        [ordered]@{ index = $cycleOperableState; operator = "eq"; value = 1 },
        [ordered]@{ index = [int]$safety.phaseControlModeIndex; operator = "eq"; value = 0 }
    )
    outputIndex = $cycleRuntimeSafety
})
$cycleZero = New-Index
Add-Formula "cycle_zero" "add" @((Input-Value 0), (Input-Value 0)) $cycleZero
Add-Node "cycle_stop_clear" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $cycleZero
    targetIndex = [int]$safety.pcsStopIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $cycleDirectionAllowed
    permitValue = 1
    deadband = 0
})
Add-Node "cycle_start" "controlWrite" ([ordered]@{
    submitWrites = $true
    inputIndex = $cycleStartValue
    targetIndex = [int]$safety.pcsStartIndex
    valueMode = "round"
    minValue = 0
    maxValue = 1
    permitIndex = $cycleDirectionAllowed
    permitValue = 1
    deadband = 0
})
$cycleSafeWriteInputs = @()
$constraintOutputs = @([int]$solve.params.paOutput, [int]$solve.params.pbOutput, [int]$solve.params.pcOutput, [int]$solve.params.qaOutput, [int]$solve.params.qbOutput, [int]$solve.params.qcOutput)
for ($i = 0; $i -lt 6; $i++) {
    $safeOutput = New-Index
    $axis = if ($i -lt 3) { "p" } else { "q" }
    $phase = $i % 3
    Add-Switch "cycle_safe_${axis}${phase}" ([ordered]@{ conditionIndex = $cycleRuntimeSafety }) ([ordered]@{ trueIndex = $constraintOutputs[$i] }) ([ordered]@{ falseValue = 0 }) $safeOutput
    $cycleSafeWriteInputs += $safeOutput
}
Set-NodeProfiles $cycleSafetyStart -Optional @("CHARGE_DISCHARGE_TEST")

$writeback = Source-Node "pcs_writeback"
$writeInputs = @([int]$writeback.params.paInput, [int]$writeback.params.pbInput, [int]$writeback.params.pcInput, [int]$writeback.params.qaInput, [int]$writeback.params.qbInput, [int]$writeback.params.qcInput)
$writeTargets = @([int]$writeback.params.pControlAIndex, [int]$writeback.params.pControlBIndex, [int]$writeback.params.pControlCIndex, [int]$writeback.params.qControlAIndex, [int]$writeback.params.qControlBIndex, [int]$writeback.params.qControlCIndex)
for ($i = 0; $i -lt 6; $i++) {
    Add-Node "pcs_control_write_${i}" "controlWrite" ([ordered]@{
        submitWrites = [bool]$writeback.params.submitWrites
        inputIndex = $writeInputs[$i]
        targetIndex = $writeTargets[$i]
        valueMode = "none"
        minValue = -1000000000
        maxValue = 1000000000
        permitIndex = [int]$writeback.params.comStatusIndex
        permitValue = 1
        profileDisabledKey = "CHARGE_DISCHARGE_TEST"
    })
    Add-Node "cycle_pcs_control_write_${i}" "controlWrite" ([ordered]@{
        submitWrites = $true
        inputIndex = $cycleSafeWriteInputs[$i]
        targetIndex = $writeTargets[$i]
        valueMode = "none"
        minValue = [double]$safety.powerMin
        maxValue = [double]$safety.powerMax
        permitIndex = [int]$safety.comStatusIndex
        permitValue = 1
        deadband = [double]$safety.deadband
        optionalProfileKey = "CHARGE_DISCHARGE_TEST"
    })
}

$legacyResult = [ordered]@{
    schemaVersion = "1.2.0"
    graphCode = "$($sourceGraph.graphCode)-modular"
    limits = [ordered]@{ maxNodes = 512; maxEdges = 1024 }
    nodes = $nodes
    edges = $edges
}
if ($indexRemap.Count -gt 0) {
    Apply-GraphIndexRemap $legacyResult
}
$invalidOutputs = @($nodes | Where-Object {
    $_.params.Contains("outputIndex") -and ([int64]$_.params.outputIndex -le 0)
})
if ($invalidOutputs.Count -gt 0) {
    throw "Generated graph contains invalid output indexes: $($invalidOutputs.id -join ', ')"
}
$legacyTypes = @(
    "meterAverage", "derivedLoad", "bmsDerived", "cosCompensation", "voltageCompensation",
    "chargeDischarge", "timedChargeDischarge", "photovoltaicCharge", "phaseBalance",
    "reserveCapacity", "skOverride", "pcsPowerSolve", "pcsWriteback"
)
$legacyNodes = @($nodes | Where-Object { $_.type -in $legacyTypes })
if ($legacyNodes.Count -gt 0) {
    throw "Generated graph still contains legacy nodes: $($legacyNodes.id -join ', ')"
}
$result = Convert-LegacyGraphToV2 $legacyResult
$duplicateOutputs = @(
    $result.nodes |
        ForEach-Object { $_.ports | Where-Object direction -eq "output" } |
        Group-Object { [int]$_.binding.index } |
        Where-Object Count -gt 1
)
if ($duplicateOutputs.Count -gt 0) {
    if (-not [bool]$result.compile.preserveImportedBehavior) {
        throw "Generated V2 graph contains duplicate executable output indexes: $($duplicateOutputs.Name -join ', ')"
    }
    Write-Warning "Preserving imported duplicate executable output indexes: $($duplicateOutputs.Name -join ', ')"
}
$json = $result | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $Output), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $RuntimeOutput), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

$virtualConfig = Get-Content -Raw -LiteralPath $VirtualSource | ConvertFrom-Json
$virtualMeter = @($virtualConfig.meters)[0]
$existingIndexes = @{}
foreach ($meter in $virtualConfig.meters) {
    foreach ($point in $meter.points) {
        $originalIndex = [int]$point.index
        if ($indexRemap.ContainsKey($originalIndex)) {
            $point.index = $indexRemap[$originalIndex]
            $point.address = $indexRemap[$originalIndex]
            $point.desc = "$($point.desc) / 原 Index $originalIndex"
        }
        $existingIndexes[[int]$point.index] = $true
    }
}

function New-SemanticVirtualPoint(
    [int]$Index,
    [string]$PointCode,
    [string]$Name,
    [string]$PointRole,
    [string]$LegacyVarName,
    [string]$Unit = "",
    [string]$Category = "setting",
    [bool]$StoreHistory = $true
) {
    [ordered]@{
        index = $Index
        pointCode = $PointCode
        name = $Name
        desc = "EMS 本体虚拟点 / $Name"
        category = $Category
        address = $Index
        enabled = $true
        isStore = $StoreHistory
        fullUpload = $true
        reportOnChange = $true
        persistIntervalSec = $(if ($StoreHistory) { 60 } else { 0 })
        deviceType = "ems"
        deviceCode = "EMS_CORE"
        pointRole = $PointRole
        legacyVarName = $LegacyVarName
        read = [ordered]@{
            enable = $false
            function = 0
            length = 1
            dataType = "float64"
            scale = 1
            offset = 0
            byteOrder = "AB"
            signed = $true
            unit = $Unit
            intervalMs = 1000
            cachePolicy = [ordered]@{
                storeLatest = $true
                storeHistory = $StoreHistory
                historySize = $(if ($StoreHistory) { 100 } else { 1 })
                ttlMs = 600000
            }
        }
        write = [ordered]@{ enable = $false }
        alarms = @()
        tags = @("ems_virtual", $Category, "change", "full_upload")
    }
}

$semanticPoints = @(
    @(6, "ems_master_auto_mode", "EMS 整柜自动模式", "master_auto_mode", "H_BOX_All_Auto", "", "setting", $true),
    @(27, "ems_station_limit_enable", "台区功率限制使能", "station_limit_enable", "H_CL_TQXZ_EN", "", "setting", $true),
    @(28, "ems_station_limit_hourly_mode", "台区功率限制分时模式", "station_limit_hourly_mode", "H_CL_TQXZ_per", "", "setting", $true),
    @(29, "ems_force_full_charge_enable", "强制满充使能", "force_full_charge_enable", "H_CL_Charge2Full_EN", "", "setting", $true),
    @(30, "ems_force_full_charge_run", "强制满充运行", "force_full_charge_run", "H_CL_Charge2Full_RUN", "", "status", $false),
    @(100, "ems_strategy_run", "EMS 策略总运行反馈", "strategy_run", "H_CL_RUN", "", "status", $false),
    @(165, "ems_pcs_energy_saving_enable", "PCS 无功率停机节能使能", "pcs_energy_saving_enable", "H_JN_PCS", "", "setting", $true),
    @(166, "ems_liquid_energy_saving_enable", "液冷随策略停机节能使能", "liquid_energy_saving_enable", "H_JN_YL", "", "setting", $true),
    @(167, "ems_liquid_target_temperature", "液冷目标温度", "liquid_target_temperature", "H_JN_YL_TEM", "℃", "setting", $true),
    @(168, "ems_force_full_charge_date_1", "强制满充日期一", "force_full_charge_date_1", "H_CL_Charge2Full_Date1", "日", "setting", $true),
    @(169, "ems_force_full_charge_date_2", "强制满充日期二", "force_full_charge_date_2", "H_CL_Charge2Full_Date2", "日", "setting", $true),
    @(471, "ems_station_fixed_positive_enable", "台区固定正向限制使能", "station_fixed_positive_enable", "H_CL_TQ_PXZ_pos_EN", "", "setting", $true),
    @(472, "ems_station_fixed_positive_power", "台区固定正向限制功率", "station_fixed_positive_power", "H_CL_TQ_PXZ_pos_Value", "kW", "setting", $true),
    @(473, "ems_station_fixed_negative_enable", "台区固定反向限制使能", "station_fixed_negative_enable", "H_CL_TQ_PXZ_neg_EN", "", "setting", $true),
    @(474, "ems_station_fixed_negative_power", "台区固定反向限制功率", "station_fixed_negative_power", "H_CL_TQ_PXZ_neg_Value", "kW", "setting", $true),
    @(475, "ems_station_hourly_positive_mask_low", "台区分时正向限制使能 0-15 时", "station_hourly_positive_mask_low", "H_CL_TQXZ_pos_EN_0_15", "", "setting", $true),
    @(476, "ems_station_hourly_positive_mask_high", "台区分时正向限制使能 16-23 时", "station_hourly_positive_mask_high", "H_CL_TQXZ_pos_EN_16_23", "", "setting", $true),
    @(477, "ems_station_hourly_negative_mask_low", "台区分时反向限制使能 0-15 时", "station_hourly_negative_mask_low", "H_CL_TQXZ_neg_EN_0_15", "", "setting", $true),
    @(478, "ems_station_hourly_negative_mask_high", "台区分时反向限制使能 16-23 时", "station_hourly_negative_mask_high", "H_CL_TQXZ_neg_EN_16_23", "", "setting", $true),
    @(582, "ems_pv_tracking_power", "光伏跟踪目标功率", "pv_tracking_power", "H_CL_GF_Tracking_Power", "kW", "setting", $true),
    @(595, "ems_reserve_capacity_power_v2", "动态增容单相储备功率", "reserve_capacity_power_v2", "H_CL_ZR_P1", "kW", "setting", $true)
)

for ($hour = 0; $hour -lt 24; $hour++) {
    $semanticPoints += ,@(
        (400 + $hour),
        "ems_schedule_power_$hour",
        "计划曲线 $hour 时功率",
        "schedule_power_$hour",
        "H_CL_DS_Power_$hour",
        "kW",
        "setting",
        $true
    )
    $semanticPoints += ,@(
        (424 + $hour),
        "ems_schedule_soc_$hour",
        "计划曲线 $hour 时目标 SOC",
        "schedule_soc_$hour",
        "H_CL_DS_SOC_$hour",
        "%",
        "setting",
        $true
    )
    $semanticPoints += ,@(
        (700 + $hour),
        "ems_station_hourly_positive_power_$hour",
        "台区 $hour 时正向限制功率",
        "station_hourly_positive_power_$hour",
        "H_CL_TQXZ_pos_$hour",
        "kW",
        "setting",
        $true
    )
    $semanticPoints += ,@(
        (724 + $hour),
        "ems_station_hourly_negative_power_$hour",
        "台区 $hour 时反向限制功率",
        "station_hourly_negative_power_$hour",
        "H_CL_TQXZ_neg_$hour",
        "kW",
        "setting",
        $true
    )
    $semanticPoints += ,@(
        (760 + $hour),
        "ems_schedule_mode_$hour",
        "计划曲线 $hour 时模式",
        "schedule_mode_$hour",
        "H_CL_DS_Mode_$hour",
        "",
        "setting",
        $true
    )
}

foreach ($spec in $semanticPoints) {
    $sourceIndex = [int]$spec[0]
    $index = if ($indexRemap.ContainsKey($sourceIndex)) { $indexRemap[$sourceIndex] } else { $sourceIndex }
    if ($existingIndexes.ContainsKey($index)) { continue }
    $effectiveSpec = @($spec)
    $effectiveSpec[0] = $index
    $point = New-SemanticVirtualPoint @effectiveSpec
    if ($index -ne $sourceIndex) {
        $point.desc = "$($point.desc) / 原 Index $sourceIndex"
    }
    $virtualMeter.points += [pscustomobject]$point
    $existingIndexes[$index] = $true
}

for ($index = 700000; $index -lt $nextIndex; $index++) {
    if ($existingIndexes.ContainsKey($index)) { continue }
    $point = [ordered]@{
        index = $index
        pointCode = "ems_modular_$index"
        name = "模块化策略中间值 $index"
        desc = "模块化 EMS 计算图内部路由点，不参与全量上传"
        category = "calculation"
        address = $index
        enabled = $true
        isStore = $false
        fullUpload = $false
        reportOnChange = $false
        persistIntervalSec = 60
        deviceType = "ems"
        deviceCode = "EMS_CORE"
        pointRole = "modular_intermediate"
        read = [ordered]@{
            enable = $false
            function = 0
            length = 1
            dataType = "float64"
            scale = 1
            offset = 0
            byteOrder = "AB"
            signed = $true
            unit = ""
            intervalMs = 1000
            cachePolicy = [ordered]@{
                storeLatest = $true
                storeHistory = $false
                historySize = 1
                ttlMs = 600000
            }
        }
        write = [ordered]@{ enable = $false }
        alarms = @()
        tags = @("ems_virtual", "modular", "internal")
    }
    $virtualMeter.points += [pscustomobject]$point
}
$virtualJson = $virtualConfig | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $VirtualOutput), $virtualJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $RuntimeVirtualOutput), $virtualJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
Write-Host "Generated $Output with $($result.nodes.Count) nodes and $($result.links.Count) links."
Write-Host "Generated $RuntimeOutput with $($result.nodes.Count) nodes and $($result.links.Count) links."
Write-Host "Generated $VirtualOutput with $($virtualMeter.points.Count) virtual points."
Write-Host "Generated $RuntimeVirtualOutput with $($virtualMeter.points.Count) virtual points."
