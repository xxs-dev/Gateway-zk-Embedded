param(
    [string]$Source = "config/examples/shuntong_ems_graph.json",
    [string]$Output = "config/examples/shuntong_ems_modular_graph.json",
    [string]$VirtualSource = "config/factory/runtime/devices/device_ems_virtual.json",
    [string]$VirtualOutput = "config/examples/device_ems_modular_virtual.json",
    [string]$IndexRemapFile = ""
)

$ErrorActionPreference = "Stop"
$sourceGraph = Get-Content -Raw -LiteralPath $Source | ConvertFrom-Json
$nodes = [System.Collections.ArrayList]::new()
$edges = [System.Collections.ArrayList]::new()
$previousNode = $null
$nextIndex = 700000
$indexRemap = @{}
if (-not [string]::IsNullOrWhiteSpace($IndexRemapFile)) {
    $remapObject = Get-Content -Raw -LiteralPath $IndexRemapFile | ConvertFrom-Json
    foreach ($property in $remapObject.PSObject.Properties) {
        $sourceIndex = [int]$property.Name
        $targetIndex = [int]$property.Value
        if ($sourceIndex -le 0 -or $targetIndex -le 0) {
            throw "Index remap values must be positive: $($property.Name)=$($property.Value)"
        }
        $indexRemap[$sourceIndex] = $targetIndex
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

function Add-Formula(
    [string]$Id,
    [string]$Operation,
    [object[]]$Inputs,
    [int]$OutputIndex,
    [hashtable]$Extra = @{}
) {
    $params = [ordered]@{ operation = $Operation; inputs = $Inputs; outputIndex = $OutputIndex }
    foreach ($entry in $Extra.GetEnumerator()) { $params[$entry.Key] = $entry.Value }
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
    foreach ($entry in $Condition.GetEnumerator()) { $params[$entry.Key] = $entry.Value }
    foreach ($entry in $WhenTrue.GetEnumerator()) { $params[$entry.Key] = $entry.Value }
    foreach ($entry in $WhenFalse.GetEnumerator()) { $params[$entry.Key] = $entry.Value }
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
Add-DerivedLoadBranch "fh_cn" `
    @([int]$derived.params.tqPaIndex, [int]$derived.params.tqPbIndex, [int]$derived.params.tqPcIndex, [int]$derived.params.tqP3Index) `
    @([int]$derived.params.tqQaIndex, [int]$derived.params.tqQbIndex, [int]$derived.params.tqQcIndex, [int]$derived.params.tqQ3Index) `
    @([int]$derived.params.cnPaIndex, [int]$derived.params.cnPbIndex, [int]$derived.params.cnPcIndex, [int]$derived.params.cnP3Index) `
    @([int]$derived.params.cnQaIndex, [int]$derived.params.cnQbIndex, [int]$derived.params.cnQcIndex, [int]$derived.params.cnQ3Index)
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
Add-DerivedLoadBranch "fh_bw" `
    @([int]$derived.params.tqPaIndex, [int]$derived.params.tqPbIndex, [int]$derived.params.tqPcIndex, [int]$derived.params.tqP3Index) `
    @([int]$derived.params.tqQaIndex, [int]$derived.params.tqQbIndex, [int]$derived.params.tqQcIndex, [int]$derived.params.tqQ3Index) `
    @($bwOutputs[0], $bwOutputs[1], $bwOutputs[2], $bwOutputs[3]) `
    @($bwOutputs[4], $bwOutputs[5], $bwOutputs[6], $bwOutputs[7])
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
$negativeGrad = New-Index
$negativePMax = New-Index
Add-Formula "voltage_negative_grad" "negate" @((Input-Index ([int]$voltage.params.gradPIndex))) $negativeGrad
Add-Formula "voltage_negative_pmax" "negate" @((Input-Index ([int]$voltage.params.pMaxIndex))) $negativePMax
$voltageInputs = @([int]$voltage.params.cnUaIndex, [int]$voltage.params.cnUbIndex, [int]$voltage.params.cnUcIndex)
$lvOutputs = @(605, 606, 607)
$hvOutputs = @(609, 610, 611)
for ($phase = 0; $phase -lt 3; $phase++) {
    $lvHigh = New-Index
    $lvRaw = New-Index
    Add-Switch "lv_phase${phase}_high" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "gt"; rightIndex = [int]$voltage.params.lvUpIndex
    }) ([ordered]@{ trueIndex = [int]$voltage.params.gradPIndex }) ([ordered]@{ falseValue = 0 }) $lvHigh
    Add-Switch "lv_phase${phase}_raw" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "lt"; rightIndex = [int]$voltage.params.lvLowIndex
    }) ([ordered]@{ trueIndex = $negativeGrad }) ([ordered]@{ falseIndex = $lvHigh }) $lvRaw
    Add-Formula "lv_phase${phase}" "clamp" @((Input-Index $lvRaw)) $lvOutputs[$phase] @{
        lowerIndex = $negativePMax; upper = 0
    }

    $hvLow = New-Index
    $hvRaw = New-Index
    Add-Switch "hv_phase${phase}_low" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "lt"; rightIndex = [int]$voltage.params.hvLowIndex
    }) ([ordered]@{ trueIndex = $negativeGrad }) ([ordered]@{ falseValue = 0 }) $hvLow
    Add-Switch "hv_phase${phase}_raw" ([ordered]@{
        leftIndex = $voltageInputs[$phase]; operator = "gt"; rightIndex = [int]$voltage.params.hvUpIndex
    }) ([ordered]@{ trueIndex = [int]$voltage.params.gradPIndex }) ([ordered]@{ falseIndex = $hvLow }) $hvRaw
    Add-Formula "hv_phase${phase}" "clamp" @((Input-Index $hvRaw)) $hvOutputs[$phase] @{
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
Add-Switch "lv_run" ([ordered]@{ leftIndex = 608; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) 10
Add-Switch "hv_run" ([ordered]@{ leftIndex = 612; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) 12
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
Add-Node "schedule_select" "scheduleSelect" ([ordered]@{
    scheduleCurve = @($schedule.params.scheduleCurve)
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
$scheduleVoltages = @([int]$schedule.params.cnUaIndex, [int]$schedule.params.cnUbIndex, [int]$schedule.params.cnUcIndex)
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

function Add-AbsoluteSum([string]$Prefix, [int[]]$Indexes, [int]$OutputIndex) {
    $absoluteIndexes = @()
    for ($i = 0; $i -lt $Indexes.Count; $i++) {
        $absolute = New-Index
        Add-Formula "${Prefix}_${i}_abs" "abs" @((Input-Index $Indexes[$i])) $absolute
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
    Add-Switch "schedule_${phase}_output" ([ordered]@{ conditionIndex = $scheduleChargeGate }) ([ordered]@{ trueIndex = $chargeValue }) ([ordered]@{ falseIndex = $activeValue }) $schedulePhaseOutputs[$phase]
}
Add-AbsoluteSum "schedule_output" $schedulePhaseOutputs ([int]$schedule.params.p3Output)
Add-Switch "schedule_run" ([ordered]@{ leftIndex = [int]$schedule.params.p3Output; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$schedule.params.runOutput)
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
Add-AbsoluteSum "cycle_output" $cycleOutputs ([int]$cycle.params.p3Output)
Add-Switch "cycle_run" ([ordered]@{ leftIndex = [int]$cycle.params.p3Output; operator = "ne"; rightValue = 0 }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) ([int]$cycle.params.runOutput)

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
for ($phase = 0; $phase -lt 3; $phase++) {
    $surplus = New-Index
    $phaseAvailable = New-Index
    Add-Formula "solar_${phase}_surplus" "subtract" @((Input-Index ([int]$solar.params.negativeLimitIndex)), (Input-Index $solarLoads[$phase])) $surplus
    Add-Switch "solar_${phase}_available" ([ordered]@{
        leftIndex = $solarLoads[$phase]; operator = "lte"; rightIndex = [int]$solar.params.negativeLimitIndex
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
$balanceCn = @([int]$balance.params.cnPaIndex, [int]$balance.params.cnPbIndex, [int]$balance.params.cnPcIndex)
for ($phase = 0; $phase -lt 3; $phase++) {
    Add-Formula "balance_phase_${phase}_combined" "add" @((Input-Index $balanceTq[$phase]), (Input-Index $balanceCn[$phase])) $balanceValues[$phase]
}
$balanceAllowedNumerator = New-Index
$balanceAllowed = New-Index
$balanceMax = New-Index
$balanceMin = New-Index
$balanceSpread = New-Index
$balanceRatio = New-Index
$balanceScaled = New-Index
$balanceExcess = New-Index
$balanceSetValue = New-Index
$balanceNegativeSetValue = New-Index
Add-Formula "balance_allowed_numerator" "multiply" @((Input-Index ([int]$balance.params.balancePercentIndex)), (Input-Index ([int]$balance.params.tqP3Index))) $balanceAllowedNumerator
Add-Formula "balance_allowed" "safeDivide" @((Input-Index $balanceAllowedNumerator), (Input-Value 300)) $balanceAllowed
Add-Formula "balance_max" "max" @((Input-Index $balanceValues[0]), (Input-Index $balanceValues[1]), (Input-Index $balanceValues[2])) $balanceMax
Add-Formula "balance_min" "min" @((Input-Index $balanceValues[0]), (Input-Index $balanceValues[1]), (Input-Index $balanceValues[2])) $balanceMin
Add-Formula "balance_spread" "subtract" @((Input-Index $balanceMax), (Input-Index $balanceMin)) $balanceSpread
Add-Formula "balance_ratio" "safeDivide" @((Input-Index $balanceSpread), (Input-Index ([int]$balance.params.tqP3Index))) $balanceRatio
Add-Formula "balance_scaled" "multiply" @((Input-Index $balanceRatio), (Input-Value 300)) $balanceScaled
Add-Formula "balance_percent" "abs" @((Input-Index $balanceScaled)) ([int]$balance.params.balanceOutput)
Add-Formula "balance_excess" "subtract" @((Input-Index $balanceSpread), (Input-Index $balanceAllowed)) $balanceExcess
Add-Formula "balance_set_value" "safeDivide" @((Input-Index $balanceExcess), (Input-Value 2)) $balanceSetValue
Add-Formula "balance_negative_set_value" "negate" @((Input-Index $balanceSetValue)) $balanceNegativeSetValue

$maxFlags = @()
$minFlags = @()
for ($phase = 0; $phase -lt 3; $phase++) {
    $maxFlag = New-Index
    $minFlag = New-Index
    $maxConditions = @([ordered]@{ index = $balanceValues[$phase]; operator = "eq"; valueIndex = $balanceMax })
    $minConditions = @([ordered]@{ index = $balanceValues[$phase]; operator = "eq"; valueIndex = $balanceMin })
    for ($prior = 0; $prior -lt $phase; $prior++) {
        $maxConditions += [ordered]@{ index = $balanceValues[$prior]; operator = "ne"; valueIndex = $balanceMax }
        $minConditions += [ordered]@{ index = $balanceValues[$prior]; operator = "ne"; valueIndex = $balanceMin }
    }
    Add-Node "balance_max_flag_${phase}" "controlGate" ([ordered]@{ combine = "all"; conditions = $maxConditions; outputIndex = $maxFlag })
    Add-Node "balance_min_flag_${phase}" "controlGate" ([ordered]@{ combine = "all"; conditions = $minConditions; outputIndex = $minFlag })
    $maxFlags += $maxFlag
    $minFlags += $minFlag
}
$balanceEnabled = New-Index
Add-Switch "balance_enabled" ([ordered]@{ leftIndex = $balanceSpread; operator = "gt"; rightIndex = $balanceAllowed }) ([ordered]@{ trueValue = 1 }) ([ordered]@{ falseValue = 0 }) $balanceEnabled
$balanceOutputs = @([int]$balance.params.paOutput, [int]$balance.params.pbOutput, [int]$balance.params.pcOutput)
for ($phase = 0; $phase -lt 3; $phase++) {
    $maxSelected = New-Index
    $minSelected = New-Index
    Add-Switch "balance_phase_${phase}_max_select" ([ordered]@{ conditionIndex = $maxFlags[$phase] }) ([ordered]@{ trueIndex = $balanceSetValue }) ([ordered]@{ falseValue = 0 }) $maxSelected
    Add-Switch "balance_phase_${phase}_min_select" ([ordered]@{ conditionIndex = $minFlags[$phase] }) ([ordered]@{ trueIndex = $balanceNegativeSetValue }) ([ordered]@{ falseIndex = $maxSelected }) $minSelected
    Add-Switch "balance_phase_${phase}_output" ([ordered]@{ conditionIndex = $balanceEnabled }) ([ordered]@{ trueIndex = $minSelected }) ([ordered]@{ falseValue = 0 }) $balanceOutputs[$phase]
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
        [ordered]@{ name = "low_voltage"; target = "active"; merge = "min"; direction = "negative"; indexes = @([int]$solve.params.outPaLvIndex, [int]$solve.params.outPbLvIndex, [int]$solve.params.outPcLvIndex) },
        [ordered]@{ name = "high_voltage"; target = "active"; merge = "max"; direction = "positive"; indexes = @([int]$solve.params.outPaHvIndex, [int]$solve.params.outPbHvIndex, [int]$solve.params.outPcHvIndex) },
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
Add-Node "power_constraints" "powerConstraint" ([ordered]@{
    activeInputIndexes = $arbiterActive
    reactiveInputIndexes = $arbiterReactive
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
    stateUpperIndex = [int]$solve.params.bmsSocMaxIndex
    stateLowerIndex = [int]$solve.params.bmsSocMinIndex
    lowStateClearReactive = $true
    lowStateClearIndexes = @((Param-Int $solve.params "cosRunOutput" 8), (Param-Int $solve.params "lvRunOutput" 10))
    highStateClearIndexes = @((Param-Int $solve.params "hvRunOutput" 12), (Param-Int $solve.params "gfRunOutput" 22))
})

$writeback = Source-Node "pcs_writeback"
$writeInputs = @([int]$writeback.params.paInput, [int]$writeback.params.pbInput, [int]$writeback.params.pcInput, [int]$writeback.params.qaInput, [int]$writeback.params.qbInput, [int]$writeback.params.qcInput)
$writeTargets = @([int]$writeback.params.pControlAIndex, [int]$writeback.params.pControlBIndex, [int]$writeback.params.pControlCIndex, [int]$writeback.params.qControlAIndex, [int]$writeback.params.qControlBIndex, [int]$writeback.params.qControlCIndex)
for ($i = 0; $i -lt 6; $i++) {
    Add-Node "pcs_control_write_${i}" "controlWrite" ([ordered]@{
        submitWrites = $false
        inputIndex = $writeInputs[$i]
        targetIndex = $writeTargets[$i]
        valueMode = "truncate"
        minValue = -1000000000
        maxValue = 1000000000
        permitIndex = [int]$writeback.params.comStatusIndex
        permitValue = 1
    })
}

$result = [ordered]@{
    schemaVersion = "1.2.0"
    graphCode = "$($sourceGraph.graphCode)-modular"
    limits = [ordered]@{ maxNodes = 512; maxEdges = 1024 }
    nodes = $nodes
    edges = $edges
}
if ($indexRemap.Count -gt 0) {
    Apply-GraphIndexRemap $result
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
$json = $result | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $Output), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

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
Write-Host "Generated $Output with $($nodes.Count) nodes and $($edges.Count) edges."
Write-Host "Generated $VirtualOutput with $($virtualMeter.points.Count) virtual points."
