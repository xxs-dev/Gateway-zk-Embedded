param(
    [Parameter(Mandatory = $true)]
    [string]$SourceProjectDirectory,

    [string]$BaselineProjectDirectory = "",

    [Parameter(Mandatory = $true)]
    [string]$DeviceConfigDirectory,

    [string]$OverlayDeviceConfigDirectory = "",

    [Parameter(Mandatory = $true)]
    [string]$OutputProjectDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputPackage,

    [Parameter(Mandatory = $true)]
    [string]$MachineCode,

    [string]$PackageVersion = "1.0.0",
    [string]$NodeId = "edge-1",
    [string]$CreatedBy = "Gateway SCADA remap tool"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ExistingDirectory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-Json([string]$Path, [object]$Value, [switch]$Compress) {
    $json = if ($Compress) {
        ConvertTo-Json -InputObject $Value -Depth 100 -Compress
    } else {
        ConvertTo-Json -InputObject $Value -Depth 100
    }
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Has-Property([object]$Value, [string]$Name) {
    return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name]
}

function Property-Value([object]$Value, [string]$Name, [object]$Default = $null) {
    if (Has-Property $Value $Name) {
        return $Value.PSObject.Properties[$Name].Value
    }
    return $Default
}

function Add-Bucket([hashtable]$Table, [string]$Key, [object]$Value) {
    if ([string]::IsNullOrWhiteSpace($Key)) {
        return
    }
    if (-not $Table.ContainsKey($Key)) {
        $Table[$Key] = [System.Collections.Generic.List[object]]::new()
    }
    $Table[$Key].Add($Value)
}

function Unique-BucketValue([hashtable]$Table, [string]$Key) {
    if ([string]::IsNullOrWhiteSpace($Key) -or -not $Table.ContainsKey($Key)) {
        return $null
    }
    $values = $Table[$Key]
    if ($values.Count -ne 1) {
        return $null
    }
    return $values[0]
}

function As-UInt32OrZero([object]$Value) {
    if ($null -eq $Value) {
        return [uint32]0
    }
    $parsed = [uint32]0
    if ([uint32]::TryParse([string]$Value, [ref]$parsed)) {
        return $parsed
    }
    return [uint32]0
}

function Add-Or-SetProperty([object]$Object, [string]$Name, [object]$Value) {
    if (Has-Property $Object $Name) {
        $Object.PSObject.Properties[$Name].Value = $Value
    } else {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function Replace-MachineCode([string]$Value) {
    if ([string]::IsNullOrEmpty($Value)) {
        return $Value
    }
    return $Value -replace 'COMM\d{9}', $MachineCode
}

function Reference-Index([object]$Value) {
    foreach ($name in @("indexFallback", "index")) {
        if (Has-Property $Value $name) {
            $index = As-UInt32OrZero (Property-Value $Value $name 0)
            if ($index -gt 0) {
                return $index
            }
        }
    }
    return [uint32]0
}

function Resolve-ProjectReferenceTarget([object]$Value) {
    if (Has-Property $Value "tagId") {
        $tagId = [string]$Value.tagId
        if (-not [string]::IsNullOrWhiteSpace($tagId) -and $ReferenceTargetByTagId.ContainsKey($tagId)) {
            return $ReferenceTargetByTagId[$tagId]
        }
    }

    if ($UseBaselineProject) {
        $index = Reference-Index $Value
        if ($index -gt 0) {
            return Unique-BucketValue $TargetByIndex ([string]$index)
        }
    }
    return $null
}

function Apply-TargetReference([object]$Value, [object]$Target) {
    $tag = $Target.Tag
    $runtime = $Target.Runtime

    if (Has-Property $Value "tagId") { $Value.tagId = [string]$tag.tagId }
    if (Has-Property $Value "nodeId") { $Value.nodeId = $NodeId }
    foreach ($name in @("deviceId", "meterCode", "pointCode", "displayName", "semanticRole", "access")) {
        if ((Has-Property $Value $name) -and (Has-Property $tag $name)) {
            $Value.PSObject.Properties[$name].Value = $tag.PSObject.Properties[$name].Value
        }
    }
    if (Has-Property $Value "pointName") {
        $Value.pointName = [string](Property-Value $tag "displayName" (Property-Value $tag "pointCode" ""))
    }
    if (Has-Property $Value "index") { $Value.index = [uint32]$runtime.index }
    if (Has-Property $Value "indexFallback") { $Value.indexFallback = [uint32]$runtime.index }
    if (Has-Property $Value "unit") { $Value.unit = [string](Property-Value $runtime "unit" "") }
    if (Has-Property $Value "dataType") { $Value.dataType = [string](Property-Value $runtime "dataType" "float64") }
    if (Has-Property $Value "writable") { $Value.writable = [bool](Property-Value $runtime "writable" $false) }
}

function Update-EmbeddedJsonString([string]$Text) {
    $trimmed = $Text.Trim()
    if (($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) -or
        ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
        try {
            $embedded = $trimmed | ConvertFrom-Json
            Update-ProjectJsonValue $embedded
            return ConvertTo-Json -InputObject $embedded -Depth 100
        } catch {
            # Designer metadata may contain arbitrary text that only resembles JSON.
        }
    }
    return Replace-MachineCode $Text
}

function Update-ProjectJsonValue([object]$Value) {
    if ($null -eq $Value) {
        return
    }

    if ($Value -is [System.Collections.IList]) {
        for ($index = 0; $index -lt $Value.Count; $index++) {
            $item = $Value[$index]
            if ($item -is [string]) {
                $Value[$index] = Update-EmbeddedJsonString $item
            } else {
                Update-ProjectJsonValue $item
            }
        }
        return
    }

    if ($Value -isnot [pscustomobject]) {
        return
    }

    $target = Resolve-ProjectReferenceTarget $Value
    if ($null -ne $target) {
        Apply-TargetReference $Value $target
    } else {
        if (Has-Property $Value "nodeId") {
            $Value.nodeId = $NodeId
        }
    }
    if ($null -eq $target -and (Has-Property $Value "tagId")) {
        $oldTagId = [string]$Value.tagId
        if ($TagIdMap.ContainsKey($oldTagId)) {
            $Value.tagId = $TagIdMap[$oldTagId]
        } else {
            $Value.tagId = Replace-MachineCode $oldTagId
        }
    }

    if ($null -eq $target -and -not $UseBaselineProject -and (Has-Property $Value "pointCode")) {
        $pointCode = [string]$Value.pointCode
        $record = Unique-BucketValue $MatchedByPointCode $pointCode
        if ($null -ne $record) {
            if (Has-Property $Value "meterCode") { $Value.meterCode = $record.MeterCode }
            if (Has-Property $Value "deviceId") { $Value.deviceId = $record.MeterCode }
            if (Has-Property $Value "pointName") { $Value.pointName = $record.Name }
            if (Has-Property $Value "displayName") { $Value.displayName = $record.Name }
            if (Has-Property $Value "index") { $Value.index = $record.Index }
            if (Has-Property $Value "indexFallback") { $Value.indexFallback = $record.Index }
            if (Has-Property $Value "unit") { $Value.unit = $record.Unit }
        }
    }

    foreach ($property in @($Value.PSObject.Properties)) {
        if ($property.Value -is [string]) {
            $property.Value = Update-EmbeddedJsonString ([string]$property.Value)
        } elseif ($null -ne $property.Value) {
            Update-ProjectJsonValue $property.Value
        }
    }
}

function Collect-ReferencedTagIds([object]$Value, [System.Collections.Generic.HashSet[string]]$Result) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [string]) {
        $trimmed = $Value.Trim()
        if (($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) -or
            ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
            try {
                Collect-ReferencedTagIds ($trimmed | ConvertFrom-Json) $Result
            } catch {
                # Non-JSON designer text is not a tag reference container.
            }
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string] -and $Value -isnot [pscustomobject]) {
        foreach ($item in $Value) {
            Collect-ReferencedTagIds $item $Result
        }
        return
    }
    if ($Value -isnot [pscustomobject]) {
        return
    }
    if ((Has-Property $Value "tagId") -and -not [string]::IsNullOrWhiteSpace([string]$Value.tagId)) {
        [void]$Result.Add([string]$Value.tagId)
    }
    foreach ($property in $Value.PSObject.Properties) {
        Collect-ReferencedTagIds $property.Value $Result
    }
}

function Test-InvalidProjectReference([object]$Value) {
    if ($null -eq $Value -or $Value -isnot [pscustomobject]) {
        return $false
    }

    if (Has-Property $Value "tagId") {
        $tagId = [string]$Value.tagId
        if (-not [string]::IsNullOrWhiteSpace($tagId) -and $InvalidReferenceTagIds.Contains($tagId)) {
            return $true
        }
    }

    $looksLikeReference = $false
    foreach ($name in @("tagId", "indexFallback", "nodeId", "meterCode", "pointCode", "semanticRole", "operator", "comparison", "slot")) {
        if (Has-Property $Value $name) {
            $looksLikeReference = $true
            break
        }
    }
    if (-not $looksLikeReference) {
        return $false
    }

    $index = Reference-Index $Value
    return $index -gt 0 -and $InvalidReferenceIndexes.Contains($index)
}

function Test-ContainsInvalidProjectReference([object]$Value) {
    if ($null -eq $Value) {
        return $false
    }
    if ($Value -is [string]) {
        $trimmed = $Value.Trim()
        if (($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) -or
            ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
            try {
                return Test-ContainsInvalidProjectReference ($trimmed | ConvertFrom-Json)
            } catch {
                return $false
            }
        }
        return $false
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string] -and $Value -isnot [pscustomobject]) {
        foreach ($item in $Value) {
            if (Test-ContainsInvalidProjectReference $item) {
                return $true
            }
        }
        return $false
    }
    if ($Value -isnot [pscustomobject]) {
        return $false
    }
    if (Test-InvalidProjectReference $Value) {
        return $true
    }
    foreach ($property in $Value.PSObject.Properties) {
        if (Test-ContainsInvalidProjectReference $property.Value) {
            return $true
        }
    }
    return $false
}

function Remove-InvalidScreenReferences([object]$Value) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [System.Collections.IList]) {
        foreach ($item in $Value) {
            Remove-InvalidScreenReferences $item
        }
        return
    }
    if ($Value -isnot [pscustomobject]) {
        return
    }

    if (Has-Property $Value "bindings") {
        $bindings = @(Property-Value $Value "bindings" @())
        $keptBindings = @($bindings | Where-Object { -not (Test-ContainsInvalidProjectReference $_) })
        $script:RemovedInvalidBindingCount += $bindings.Count - $keptBindings.Count
        Add-Or-SetProperty $Value "bindings" ([object[]]$keptBindings)
    }
    if (Has-Property $Value "stateRules") {
        $stateRules = @(Property-Value $Value "stateRules" @())
        $keptStateRules = @($stateRules | Where-Object { -not (Test-ContainsInvalidProjectReference $_) })
        $script:RemovedInvalidStateRuleCount += $stateRules.Count - $keptStateRules.Count
        Add-Or-SetProperty $Value "stateRules" ([object[]]$keptStateRules)
    }
    if (Has-Property $Value "action") {
        $action = Property-Value $Value "action" $null
        if ($null -ne $action -and (Test-ContainsInvalidProjectReference $action)) {
            Add-Or-SetProperty $Value "action" $null
            $script:RemovedInvalidActionCount++
        }
    }
    if (Has-Property $Value "actions") {
        $actions = @(Property-Value $Value "actions" @())
        $keptActions = @($actions | Where-Object { -not (Test-ContainsInvalidProjectReference $_) })
        $script:RemovedInvalidActionCount += $actions.Count - $keptActions.Count
        Add-Or-SetProperty $Value "actions" ([object[]]$keptActions)
    }

    foreach ($property in @($Value.PSObject.Properties)) {
        if ($property.Value -is [string]) {
            $trimmed = ([string]$property.Value).Trim()
            if (($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) -or
                ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
                try {
                    $embedded = $trimmed | ConvertFrom-Json
                    if ($property.Name -ieq "stateBindingJson" -and
                        (Has-Property $embedded "states") -and
                        (Test-ContainsInvalidProjectReference (Property-Value $embedded "states" @()))) {
                        Add-Or-SetProperty $embedded "states" ([object[]]@())
                        $defaultState = Property-Value $embedded "defaultState" $null
                        if ($null -eq $defaultState) {
                            $defaultState = [pscustomobject]@{}
                            Add-Or-SetProperty $embedded "defaultState" $defaultState
                        }
                        Add-Or-SetProperty $defaultState "code" "UNKNOWN"
                        $script:ClearedInvalidStateBindingCount++
                    }
                    Remove-InvalidScreenReferences $embedded
                    $property.Value = ConvertTo-Json -InputObject $embedded -Depth 100
                } catch {
                    # Designer metadata may contain arbitrary text that only resembles JSON.
                }
            }
        } else {
            Remove-InvalidScreenReferences $property.Value
        }
    }
}

function Test-ExplicitFalse([object]$Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [bool]) { return -not [bool]$Value }
    return [string]$Value -in @("0", "false", "False", "FALSE")
}

function Test-WidgetHasAction([object]$Widget) {
    $action = Property-Value $Widget "action" $null
    if ($null -ne $action) {
        foreach ($name in @("type", "tagId", "screenId", "targetScreenId", "pageId", "targetPageId")) {
            if (-not [string]::IsNullOrWhiteSpace([string](Property-Value $action $name ""))) {
                return $true
            }
        }
    }
    return @(Property-Value $Widget "actions" @()).Count -gt 0
}

function Test-ReferenceWritable([object]$Reference) {
    if ($null -eq $Reference -or -not (Has-Property $Reference "tagId")) {
        return $false
    }
    $tagId = [string]$Reference.tagId
    if ([string]::IsNullOrWhiteSpace($tagId) -or -not $ReferenceTargetByTagId.ContainsKey($tagId)) {
        return $false
    }
    return [bool](Property-Value $ReferenceTargetByTagId[$tagId].Runtime "writable" $false)
}

function Test-WidgetHasWritableTarget([object]$Widget) {
    foreach ($binding in @(Property-Value $Widget "bindings" @())) {
        if (Test-ReferenceWritable $binding) { return $true }
    }
    $action = Property-Value $Widget "action" $null
    if (Test-ReferenceWritable $action) { return $true }
    foreach ($candidate in @(Property-Value $Widget "actions" @())) {
        if (Test-ReferenceWritable $candidate) { return $true }
    }
    return $false
}

function Normalize-ScreenWidgetSafety([object]$Value) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [System.Collections.IList]) {
        foreach ($item in $Value) {
            Normalize-ScreenWidgetSafety $item
        }
        return
    }
    if ($Value -isnot [pscustomobject]) {
        return
    }

    $widgetType = [string](Property-Value $Value "type" (Property-Value $Value "widgetType" ""))
    if ($widgetType -iin @("qtButton", "QPushButton")) {
        $properties = Property-Value $Value "properties" $null
        if ($null -eq $properties) {
            $properties = [pscustomobject]@{}
            Add-Or-SetProperty $Value "properties" $properties
        }
        $visible = -not (Test-ExplicitFalse (Property-Value $Value "visible" $true)) -and
            -not (Test-ExplicitFalse (Property-Value $properties "visible" $true))
        if ($visible -and -not (Test-WidgetHasAction $Value)) {
            Add-Or-SetProperty $properties "enabled" $false
            $script:NormalizedActionlessButtonCount++
        }
    } elseif ($widgetType -iin @("qtInput", "QLineEdit")) {
        if (-not (Test-WidgetHasWritableTarget $Value)) {
            $properties = Property-Value $Value "properties" $null
            if ($null -eq $properties) {
                $properties = [pscustomobject]@{}
                Add-Or-SetProperty $Value "properties" $properties
            }
            Add-Or-SetProperty $properties "readOnly" $true
            Add-Or-SetProperty $properties "enabled" $false
            $script:NormalizedUnboundInputCount++
        }
    }

    foreach ($property in @($Value.PSObject.Properties)) {
        if ($property.Name -ne "properties") {
            Normalize-ScreenWidgetSafety $property.Value
        }
    }
}

$SourceProjectDirectory = Resolve-ExistingDirectory $SourceProjectDirectory "source SCADA project directory"
$UseBaselineProject = -not [string]::IsNullOrWhiteSpace($BaselineProjectDirectory)
if ($UseBaselineProject) {
    $BaselineProjectDirectory = Resolve-ExistingDirectory $BaselineProjectDirectory "baseline SCADA project directory"
}
$DeviceConfigDirectory = Resolve-ExistingDirectory $DeviceConfigDirectory "device config directory"
if (-not [string]::IsNullOrWhiteSpace($OverlayDeviceConfigDirectory)) {
    $OverlayDeviceConfigDirectory = Resolve-ExistingDirectory $OverlayDeviceConfigDirectory "overlay device config directory"
}
$OutputProjectDirectory = [System.IO.Path]::GetFullPath($OutputProjectDirectory)
$OutputPackage = [System.IO.Path]::GetFullPath($OutputPackage)
$OutputReport = $OutputPackage + ".remap-report.json"

if (Test-Path -LiteralPath $OutputProjectDirectory) {
    throw "output project directory already exists: $OutputProjectDirectory"
}
if (Test-Path -LiteralPath $OutputPackage) {
    throw "output package already exists: $OutputPackage"
}
if (Test-Path -LiteralPath $OutputReport) {
    throw "output report already exists: $OutputReport"
}
if ($OutputProjectDirectory -eq $SourceProjectDirectory -or
    $OutputProjectDirectory -eq $BaselineProjectDirectory -or
    $OutputProjectDirectory -eq $DeviceConfigDirectory -or
    $OutputProjectDirectory -eq $OverlayDeviceConfigDirectory) {
    throw "output project directory must be separate from all input directories"
}
if ($MachineCode -notmatch '^COMM\d{9}$') {
    throw "invalid machineCode: $MachineCode"
}
if ($PackageVersion -notmatch '^[A-Za-z0-9._-]+$') {
    throw "invalid packageVersion: $PackageVersion"
}

$configPaths = @{}
foreach ($file in Get-ChildItem -LiteralPath $DeviceConfigDirectory -Filter "*.json" -File) {
    $configPaths[$file.Name] = $file.FullName
}
if (-not [string]::IsNullOrWhiteSpace($OverlayDeviceConfigDirectory)) {
    foreach ($file in Get-ChildItem -LiteralPath $OverlayDeviceConfigDirectory -Filter "*.json" -File) {
        if ($file.Name -like "device*.json") {
            $configPaths[$file.Name] = $file.FullName
        }
    }
}

$points = [System.Collections.Generic.List[object]]::new()
foreach ($entry in $configPaths.GetEnumerator() | Sort-Object Key) {
    $config = Read-Json $entry.Value
    $sharedMemoryName = [string](Property-Value $config.memoryStore "sharedMemoryName" "")
    if ([string]::IsNullOrWhiteSpace($sharedMemoryName)) {
        continue
    }
    foreach ($meter in @(Property-Value $config "meters" @())) {
        $meterCode = [string](Property-Value $meter "meterCode" "")
        foreach ($point in @(Property-Value $meter "points" @())) {
            $index = As-UInt32OrZero (Property-Value $point "index" 0)
            $pointCode = [string](Property-Value $point "pointCode" "")
            if ($index -eq 0 -or [string]::IsNullOrWhiteSpace($pointCode)) {
                continue
            }
            $read = Property-Value $point "read" $null
            $write = Property-Value $point "write" $null
            $writable = $null -ne $write -and [bool](Property-Value $write "enable" $false)
            $dataType = [string](Property-Value $read "dataType" (Property-Value $write "dataType" "float64"))
            $unit = [string](Property-Value $read "unit" (Property-Value $write "unit" ""))
            $points.Add([pscustomobject]@{
                ConfigFile = $entry.Key
                SharedMemoryName = $sharedMemoryName
                MeterCode = $meterCode
                DeviceName = [string](Property-Value $meter "deviceName" "")
                PointCode = $pointCode
                Name = [string](Property-Value $point "name" $pointCode)
                Index = $index
                LegacyIndex = As-UInt32OrZero (Property-Value $point "legacyIndex" 0)
                DataType = if ([string]::IsNullOrWhiteSpace($dataType)) { "float64" } else { $dataType }
                Unit = $unit
                Writable = $writable
            })
        }
    }
}
if ($points.Count -eq 0) {
    throw "no indexed points were found in the device configs"
}

$byMeterPoint = @{}
$byPointCode = @{}
$byIndex = @{}
$byLegacyIndex = @{}
$byName = @{}
foreach ($point in $points) {
    Add-Bucket $byMeterPoint ($point.MeterCode + "`u{1f}" + $point.PointCode) $point
    Add-Bucket $byPointCode $point.PointCode $point
    Add-Bucket $byIndex ([string]$point.Index) $point
    if ($point.LegacyIndex -gt 0) { Add-Bucket $byLegacyIndex ([string]$point.LegacyIndex) $point }
    Add-Bucket $byName $point.Name $point
}
if ($UseBaselineProject) {
    $duplicateDeviceIndexes = @($byIndex.GetEnumerator() | Where-Object { $_.Value.Count -ne 1 })
    if ($duplicateDeviceIndexes.Count -gt 0) {
        $indexes = @($duplicateDeviceIndexes | ForEach-Object { $_.Key } | Sort-Object) -join ", "
        throw "device configs contain duplicate indexes: $indexes"
    }
}

$sourceTagsPath = Join-Path $SourceProjectDirectory "tags.json"
$sourceTags = @(Read-Json $sourceTagsPath)
$sourceRuntimePath = Join-Path $SourceProjectDirectory "runtime-map.json"
$sourceRuntimeMappings = if (Test-Path -LiteralPath $sourceRuntimePath -PathType Leaf) {
    @(Read-Json $sourceRuntimePath)
} else {
    @()
}
$newTags = [System.Collections.Generic.List[object]]::new()
$runtimeMappings = [System.Collections.Generic.List[object]]::new()
$reportRows = [System.Collections.Generic.List[object]]::new()
$TagIdMap = @{}
$MatchedByPointCode = @{}
$ReferenceTargetByTagId = @{}
$TargetByIndex = @{}
$usedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$usedRoutes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$reservedFallback = [uint32]900000000
$mappedSourceTagCount = 0
$ignoredSourceOnlyTagCount = 0
$appendedSourceOnlyTagCount = 0
$appendedDeviceOnlyTagCount = 0
$excludedBaselinePlaceholderCount = 0
$excludedBaselineWithoutDeviceCount = 0
$InvalidReferenceTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$InvalidReferenceIndexes = [System.Collections.Generic.HashSet[uint32]]::new()

$sourceTagsByIndex = @{}
$sourceRuntimeByIndex = @{}
$sourceRuntimeByTagId = @{}
foreach ($sourceTag in $sourceTags) {
    $sourceIndex = Reference-Index $sourceTag
    if ($sourceIndex -gt 0) { Add-Bucket $sourceTagsByIndex ([string]$sourceIndex) $sourceTag }
}
foreach ($sourceRuntime in $sourceRuntimeMappings) {
    $sourceIndex = As-UInt32OrZero (Property-Value $sourceRuntime "index" 0)
    if ($sourceIndex -gt 0) { Add-Bucket $sourceRuntimeByIndex ([string]$sourceIndex) $sourceRuntime }
    Add-Bucket $sourceRuntimeByTagId ([string](Property-Value $sourceRuntime "tagId" "")) $sourceRuntime
}
$sourceReferencedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$sourceScreensDirectory = Join-Path $SourceProjectDirectory "screens"
if (Test-Path -LiteralPath $sourceScreensDirectory -PathType Container) {
    foreach ($screenFile in Get-ChildItem -LiteralPath $sourceScreensDirectory -Filter "*.json" -File) {
        Collect-ReferencedTagIds (Read-Json $screenFile.FullName) $sourceReferencedTagIds
    }
}

if ($UseBaselineProject) {
    $baselineTagsPath = Join-Path $BaselineProjectDirectory "tags.json"
    $baselineRuntimePath = Join-Path $BaselineProjectDirectory "runtime-map.json"
    if (-not (Test-Path -LiteralPath $baselineTagsPath -PathType Leaf)) {
        throw "baseline tags.json does not exist: $baselineTagsPath"
    }
    if (-not (Test-Path -LiteralPath $baselineRuntimePath -PathType Leaf)) {
        throw "baseline runtime-map.json does not exist: $baselineRuntimePath"
    }

    $baselineTags = @(Read-Json $baselineTagsPath)
    $baselineRuntimeMappings = @(Read-Json $baselineRuntimePath)
    $baselineTagCountForReport = $baselineTags.Count
    $baselineTagsByIndex = @{}
    $baselineRuntimeByIndex = @{}
    foreach ($baselineTag in $baselineTags) {
        $baselineIndex = Reference-Index $baselineTag
        if ($baselineIndex -eq 0) {
            throw "baseline tag has no valid index: $([string](Property-Value $baselineTag 'tagId' ''))"
        }
        Add-Bucket $baselineTagsByIndex ([string]$baselineIndex) $baselineTag
    }
    foreach ($baselineRuntime in $baselineRuntimeMappings) {
        $baselineIndex = As-UInt32OrZero (Property-Value $baselineRuntime "index" 0)
        if ($baselineIndex -eq 0) {
            throw "baseline runtime route has no valid index: $([string](Property-Value $baselineRuntime 'tagId' ''))"
        }
        Add-Bucket $baselineRuntimeByIndex ([string]$baselineIndex) $baselineRuntime
    }

    foreach ($baselineTag in $baselineTags) {
        $baselineIndex = Reference-Index $baselineTag
        $baselineRuntime = Unique-BucketValue $baselineRuntimeByIndex ([string]$baselineIndex)
        if ($null -eq $baselineRuntime) {
            throw "baseline tag index does not have exactly one runtime route: $baselineIndex"
        }

        $baselineTagId = [string](Property-Value $baselineTag "tagId" "")
        if ($baselineTagId -like "DISPLAY_RUNTIME*") {
            $excludedBaselinePlaceholderCount++
            continue
        }
        $devicePoint = Unique-BucketValue $byIndex ([string]$baselineIndex)
        if ($null -eq $devicePoint) {
            $excludedBaselineWithoutDeviceCount++
            continue
        }

        $sourceTag = Unique-BucketValue $sourceTagsByIndex ([string]$baselineIndex)
        $sourceRuntime = $null
        if ($null -ne $sourceTag) {
            $sourceRuntime = Unique-BucketValue $sourceRuntimeByTagId ([string]$sourceTag.tagId)
        }
        if ($null -eq $sourceRuntime) {
            $sourceRuntime = Unique-BucketValue $sourceRuntimeByIndex ([string]$baselineIndex)
        }

        $newTag = $baselineTag.PSObject.Copy()
        $newTagId = Replace-MachineCode $baselineTagId
        $newRuntime = $baselineRuntime.PSObject.Copy()
        Add-Or-SetProperty $newTag "tagId" $newTagId
        Add-Or-SetProperty $newTag "nodeId" $NodeId
        foreach ($name in @("deviceId", "meterCode")) {
            if (Has-Property $newTag $name) {
                $newTag.PSObject.Properties[$name].Value = Replace-MachineCode ([string]$newTag.PSObject.Properties[$name].Value)
            }
        }
        Add-Or-SetProperty $newTag "indexFallback" $baselineIndex
        Add-Or-SetProperty $newRuntime "nodeId" $NodeId
        Add-Or-SetProperty $newRuntime "tagId" $newTagId
        Add-Or-SetProperty $newRuntime "index" $baselineIndex

        $sourceWritableKnown = $false
        $sourceWritable = $false
        if ($null -ne $sourceTag -and (Has-Property $sourceTag "access")) {
            $sourceWritableKnown = $true
            $sourceWritable = [string]$sourceTag.access -ieq "readWrite"
        }
        if ($null -ne $sourceRuntime -and (Has-Property $sourceRuntime "writable")) {
            $sourceWritableKnown = $true
            $sourceWritable = $sourceWritable -or [bool]$sourceRuntime.writable
        }

        $effectiveWritable = [bool](Property-Value $newRuntime "writable" $false)
        if ($null -ne $devicePoint) {
            $effectiveWritable = [bool]$devicePoint.Writable
        } elseif ($sourceWritableKnown) {
            $effectiveWritable = $sourceWritable
        }
        $effectiveDataType = [string](Property-Value $newRuntime "dataType" (Property-Value $newTag "dataType" "float64"))
        $effectiveUnit = [string](Property-Value $newRuntime "unit" (Property-Value $newTag "unit" ""))
        if ($null -ne $devicePoint) {
            $effectiveDataType = [string]$devicePoint.DataType
            $effectiveUnit = [string]$devicePoint.Unit
        }
        if ([string]::IsNullOrWhiteSpace($effectiveDataType)) { $effectiveDataType = "float64" }

        $effectiveAccess = if ($effectiveWritable) { "readWrite" } else { "read" }
        Add-Or-SetProperty $newTag "access" $effectiveAccess
        Add-Or-SetProperty $newTag "dataType" $effectiveDataType
        Add-Or-SetProperty $newTag "unit" $effectiveUnit
        Add-Or-SetProperty $newRuntime "writable" $effectiveWritable
        Add-Or-SetProperty $newRuntime "dataType" $effectiveDataType
        Add-Or-SetProperty $newRuntime "unit" $effectiveUnit

        if (-not $usedTagIds.Add($newTagId)) {
            throw "duplicate target SCADA tagId: $newTagId"
        }
        $routeKey = [string]$newRuntime.sharedMemoryName + "`u{1f}" + [string]$baselineIndex
        if (-not $usedRoutes.Add($routeKey)) {
            throw "baseline contains duplicate runtime route: $routeKey"
        }

        $target = [pscustomobject]@{ Tag = $newTag; Runtime = $newRuntime }
        Add-Bucket $TargetByIndex ([string]$baselineIndex) $target
        $ReferenceTargetByTagId[$baselineTagId] = $target
        $ReferenceTargetByTagId[$newTagId] = $target
        $newTags.Add($newTag)
        $runtimeMappings.Add($newRuntime)
    }

    foreach ($sourceTag in $sourceTags) {
        $sourceTagId = [string]$sourceTag.tagId
        $sourceIndex = Reference-Index $sourceTag
        $existingTarget = if ($sourceIndex -gt 0) { Unique-BucketValue $TargetByIndex ([string]$sourceIndex) } else { $null }
        if ($null -ne $existingTarget) {
            $newTagId = [string]$existingTarget.Tag.tagId
            $TagIdMap[$sourceTagId] = $newTagId
            $ReferenceTargetByTagId[$sourceTagId] = $existingTarget
            $mappedSourceTagCount++
            $reportRows.Add([pscustomobject]@{
                sourceTagId = $sourceTagId
                targetTagId = $newTagId
                matchedBy = "baselineIndex"
                sourceIndex = $sourceIndex
                targetIndex = [uint32]$existingTarget.Runtime.index
                sharedMemoryName = [string]$existingTarget.Runtime.sharedMemoryName
                targetMeterCode = [string](Property-Value $existingTarget.Tag "meterCode" "")
                displayName = [string](Property-Value $existingTarget.Tag "displayName" "")
            })
            continue
        }
        $authoritativeDevicePoint = if ($sourceIndex -gt 0) { Unique-BucketValue $byIndex ([string]$sourceIndex) } else { $null }
        if ($null -eq $authoritativeDevicePoint) {
            $newTagId = Replace-MachineCode $sourceTagId
            $TagIdMap[$sourceTagId] = $newTagId
            [void]$InvalidReferenceTagIds.Add($sourceTagId)
            [void]$InvalidReferenceTagIds.Add($newTagId)
            if ($sourceIndex -gt 0) { [void]$InvalidReferenceIndexes.Add($sourceIndex) }
            $ignoredSourceOnlyTagCount++
            $reportRows.Add([pscustomobject]@{
                sourceTagId = $sourceTagId
                targetTagId = $newTagId
                matchedBy = "excludedByDeviceIndex"
                sourceIndex = $sourceIndex
                targetIndex = 0
                sharedMemoryName = ""
                targetMeterCode = ""
                displayName = [string](Property-Value $sourceTag "displayName" "")
            })
            continue
        }
        $baselineTag = if ($sourceIndex -gt 0) { Unique-BucketValue $baselineTagsByIndex ([string]$sourceIndex) } else { $null }
        $baselineRuntime = if ($sourceIndex -gt 0) { Unique-BucketValue $baselineRuntimeByIndex ([string]$sourceIndex) } else { $null }
        $sourceRuntime = Unique-BucketValue $sourceRuntimeByTagId $sourceTagId
        if ($null -eq $sourceRuntime -and $sourceIndex -gt 0) {
            $sourceRuntime = Unique-BucketValue $sourceRuntimeByIndex ([string]$sourceIndex)
        }
        $devicePoint = if ($sourceIndex -gt 0) { Unique-BucketValue $byIndex ([string]$sourceIndex) } else { $null }

        $useBaselineTag = $null -ne $baselineTag -and
            ([string](Property-Value $baselineTag "tagId" "") -notlike "DISPLAY_RUNTIME*")
        if ($useBaselineTag) {
            $newTag = $baselineTag.PSObject.Copy()
            $newTagId = Replace-MachineCode ([string]$baselineTag.tagId)
        } else {
            $newTag = $sourceTag.PSObject.Copy()
            $newTagId = Replace-MachineCode $sourceTagId
        }

        if ($null -ne $baselineRuntime) {
            $newRuntime = $baselineRuntime.PSObject.Copy()
            $matchedBy = "baselineIndex"
        } elseif ($null -ne $devicePoint) {
            $newRuntime = [pscustomobject]@{
                nodeId = $NodeId
                tagId = $newTagId
                sharedMemoryName = [string]$devicePoint.SharedMemoryName
                index = $sourceIndex
                writable = [bool]$devicePoint.Writable
                dataType = [string]$devicePoint.DataType
                unit = [string]$devicePoint.Unit
            }
            $matchedBy = "deviceIndex"
        } elseif ($null -ne $sourceRuntime) {
            $newRuntime = $sourceRuntime.PSObject.Copy()
            $matchedBy = "sourceRuntimeIndex"
        } else {
            $TagIdMap[$sourceTagId] = $newTagId
            $reportRows.Add([pscustomobject]@{
                sourceTagId = $sourceTagId
                targetTagId = $newTagId
                matchedBy = "unmatched"
                sourceIndex = $sourceIndex
                targetIndex = 0
                sharedMemoryName = ""
                targetMeterCode = ""
                displayName = ""
            })
            continue
        }

        Add-Or-SetProperty $newTag "tagId" $newTagId
        Add-Or-SetProperty $newTag "nodeId" $NodeId
        foreach ($name in @("deviceId", "meterCode")) {
            if (Has-Property $newTag $name) {
                $newTag.PSObject.Properties[$name].Value = Replace-MachineCode ([string]$newTag.PSObject.Properties[$name].Value)
            }
        }
        Add-Or-SetProperty $newTag "indexFallback" $sourceIndex
        Add-Or-SetProperty $newRuntime "nodeId" $NodeId
        Add-Or-SetProperty $newRuntime "tagId" $newTagId
        Add-Or-SetProperty $newRuntime "index" $sourceIndex

        $sourceWritableKnown = $false
        $sourceWritable = $false
        if ($null -ne $sourceTag -and (Has-Property $sourceTag "access")) {
            $sourceWritableKnown = $true
            $sourceWritable = [string]$sourceTag.access -ieq "readWrite"
        }
        if ($null -ne $sourceRuntime -and (Has-Property $sourceRuntime "writable")) {
            $sourceWritableKnown = $true
            $sourceWritable = $sourceWritable -or [bool]$sourceRuntime.writable
        }

        $effectiveWritable = [bool](Property-Value $newRuntime "writable" $false)
        if ($null -ne $devicePoint) {
            $effectiveWritable = [bool]$devicePoint.Writable
        } elseif ($sourceWritableKnown) {
            $effectiveWritable = $sourceWritable
        }
        $effectiveDataType = [string](Property-Value $newRuntime "dataType" (Property-Value $newTag "dataType" "float64"))
        $effectiveUnit = [string](Property-Value $newRuntime "unit" (Property-Value $newTag "unit" ""))
        if ($null -ne $devicePoint) {
            $effectiveDataType = [string]$devicePoint.DataType
            $effectiveUnit = [string]$devicePoint.Unit
        }
        if ([string]::IsNullOrWhiteSpace($effectiveDataType)) { $effectiveDataType = "float64" }

        $effectiveAccess = if ($effectiveWritable) { "readWrite" } else { "read" }
        Add-Or-SetProperty $newTag "access" $effectiveAccess
        Add-Or-SetProperty $newTag "dataType" $effectiveDataType
        Add-Or-SetProperty $newTag "unit" $effectiveUnit
        Add-Or-SetProperty $newRuntime "writable" $effectiveWritable
        Add-Or-SetProperty $newRuntime "dataType" $effectiveDataType
        Add-Or-SetProperty $newRuntime "unit" $effectiveUnit

        if (-not $usedTagIds.Add($newTagId)) {
            throw "duplicate target SCADA tagId: $newTagId"
        }
        $routeKey = [string]$newRuntime.sharedMemoryName + "`u{1f}" + [string]$sourceIndex
        if (-not $usedRoutes.Add($routeKey)) {
            throw "baseline contains duplicate runtime route: $routeKey"
        }

        $target = [pscustomobject]@{ Tag = $newTag; Runtime = $newRuntime }
        Add-Bucket $TargetByIndex ([string]$sourceIndex) $target
        if ($null -ne $baselineTag) {
            $ReferenceTargetByTagId[[string]$baselineTag.tagId] = $target
        }
        $ReferenceTargetByTagId[$newTagId] = $target
        $ReferenceTargetByTagId[$sourceTagId] = $target
        $TagIdMap[$sourceTagId] = $newTagId
        $newTags.Add($newTag)
        $runtimeMappings.Add($newRuntime)
        $mappedSourceTagCount++
        $appendedSourceOnlyTagCount++
        $reportRows.Add([pscustomobject]@{
            sourceTagId = $sourceTagId
            targetTagId = $newTagId
            matchedBy = $matchedBy
            sourceIndex = $sourceIndex
            targetIndex = [uint32]$newRuntime.index
            sharedMemoryName = [string]$newRuntime.sharedMemoryName
            targetMeterCode = [string](Property-Value $newTag "meterCode" "")
            displayName = [string](Property-Value $newTag "displayName" "")
        })
    }

    foreach ($indexKey in @($byIndex.Keys | Sort-Object { [uint32]$_ })) {
        if ($null -ne (Unique-BucketValue $TargetByIndex $indexKey)) {
            continue
        }
        $devicePoint = Unique-BucketValue $byIndex $indexKey
        $index = [uint32]$devicePoint.Index
        $meterCode = Replace-MachineCode ([string]$devicePoint.MeterCode)
        $tagId = $meterCode + "." + [string]$devicePoint.PointCode
        $tag = [pscustomobject]@{
            tagId = $tagId
            nodeId = $NodeId
            deviceId = $meterCode
            meterCode = $meterCode
            pointCode = [string]$devicePoint.PointCode
            semanticRole = ""
            displayName = [string]$devicePoint.Name
            unit = [string]$devicePoint.Unit
            dataType = [string]$devicePoint.DataType
            access = if ([bool]$devicePoint.Writable) { "readWrite" } else { "read" }
            indexFallback = $index
        }
        $runtime = [pscustomobject]@{
            nodeId = $NodeId
            tagId = $tagId
            sharedMemoryName = [string]$devicePoint.SharedMemoryName
            index = $index
            writable = [bool]$devicePoint.Writable
            dataType = [string]$devicePoint.DataType
            unit = [string]$devicePoint.Unit
        }
        if (-not $usedTagIds.Add($tagId)) {
            throw "duplicate target SCADA tagId: $tagId"
        }
        $routeKey = [string]$runtime.sharedMemoryName + "`u{1f}" + [string]$index
        if (-not $usedRoutes.Add($routeKey)) {
            throw "duplicate device runtime route: $routeKey"
        }
        $target = [pscustomobject]@{ Tag = $tag; Runtime = $runtime }
        Add-Bucket $TargetByIndex ([string]$index) $target
        $ReferenceTargetByTagId[$tagId] = $target
        $newTags.Add($tag)
        $runtimeMappings.Add($runtime)
        $appendedDeviceOnlyTagCount++
    }

    if ($newTags.Count -ne $points.Count -or $runtimeMappings.Count -ne $points.Count) {
        throw "baseline remap output must equal the authoritative device index set: device=$($points.Count), tags=$($newTags.Count), routes=$($runtimeMappings.Count)"
    }
    $displayRuntimeTags = @($newTags | Where-Object { [string]$_.tagId -like "DISPLAY_RUNTIME*" })
    if ($displayRuntimeTags.Count -gt 0) {
        throw "baseline remap output still contains DISPLAY_RUNTIME tags"
    }
} else {
    $baselineTagCountForReport = 0
    foreach ($sourceTag in $sourceTags) {
        $sourceTagId = [string]$sourceTag.tagId
        $sourceMeterCode = [string]$sourceTag.meterCode
        $sourcePointCode = [string]$sourceTag.pointCode
        $sourceIndex = As-UInt32OrZero $sourceTag.indexFallback
        $match = Unique-BucketValue $byMeterPoint ($sourceMeterCode + "`u{1f}" + $sourcePointCode)
        $matchedBy = "meter+pointCode"
        if ($null -eq $match) {
            $match = Unique-BucketValue $byPointCode $sourcePointCode
            $matchedBy = "pointCode"
        }
        if ($null -eq $match -and $sourceIndex -gt 0) {
            $match = Unique-BucketValue $byIndex ([string]$sourceIndex)
            $matchedBy = "index"
        }
        if ($null -eq $match -and $sourceIndex -gt 0) {
            $match = Unique-BucketValue $byLegacyIndex ([string]$sourceIndex)
            $matchedBy = "legacyIndex"
        }
        if ($null -eq $match) {
            $match = Unique-BucketValue $byName ([string]$sourceTag.displayName)
            $matchedBy = "name"
        }

        $newTag = $sourceTag.PSObject.Copy()
        $newTag.nodeId = $NodeId
        $newRuntime = $null
        if ($null -ne $match) {
            $newTagId = $match.MeterCode + "." + $match.PointCode
            $newTag.tagId = $newTagId
            $newTag.deviceId = $match.MeterCode
            $newTag.meterCode = $match.MeterCode
            $newTag.pointCode = $match.PointCode
            $newTag.displayName = $match.Name
            $newTag.unit = $match.Unit
            $newTag.dataType = $match.DataType
            $newTag.access = if ($match.Writable) { "readWrite" } else { "read" }
            $newTag.indexFallback = $match.Index
            Add-Bucket $MatchedByPointCode $sourcePointCode $match

            $route = $match.SharedMemoryName + "`u{1f}" + [string]$match.Index
            if (-not $usedRoutes.Add($route)) {
                throw "multiple SCADA tags resolved to the same runtime route: $route"
            }
            $newRuntime = [pscustomobject]@{
                nodeId = $NodeId
                tagId = $newTagId
                sharedMemoryName = $match.SharedMemoryName
                index = $match.Index
                writable = $match.Writable
                dataType = $match.DataType
                unit = $match.Unit
            }
            $runtimeMappings.Add($newRuntime)
            $mappedSourceTagCount++
        } else {
            $newTagId = Replace-MachineCode $sourceTagId
            $newTag.tagId = $newTagId
            $newTag.deviceId = Replace-MachineCode ([string]$newTag.deviceId)
            $newTag.meterCode = Replace-MachineCode ([string]$newTag.meterCode)
            $newTag.indexFallback = $reservedFallback
            $reservedFallback++
            $matchedBy = "unmatched"
        }

        if (-not $usedTagIds.Add($newTagId)) {
            throw "duplicate target SCADA tagId: $newTagId"
        }
        $TagIdMap[$sourceTagId] = $newTagId
        $newTags.Add($newTag)
        if ($null -ne $newRuntime) {
            $target = [pscustomobject]@{ Tag = $newTag; Runtime = $newRuntime }
            $ReferenceTargetByTagId[$sourceTagId] = $target
            $ReferenceTargetByTagId[$newTagId] = $target
            Add-Bucket $TargetByIndex ([string]$newRuntime.index) $target
        }
        $reportRows.Add([pscustomobject]@{
            sourceTagId = $sourceTagId
            targetTagId = $newTagId
            matchedBy = $matchedBy
            sourceIndex = $sourceIndex
            targetIndex = if ($null -ne $match) { $match.Index } else { $newTag.indexFallback }
            sharedMemoryName = if ($null -ne $match) { $match.SharedMemoryName } else { "" }
            targetMeterCode = [string]$newTag.meterCode
            displayName = [string]$newTag.displayName
        })
    }
}

[void](New-Item -ItemType Directory -Path $OutputProjectDirectory)
Copy-Item -Path (Join-Path $SourceProjectDirectory "*") -Destination $OutputProjectDirectory -Recurse -Force

$manifest = Read-Json (Join-Path $OutputProjectDirectory "manifest.json")
$manifest.projectId = "ky-ems-$MachineCode"
$manifest.projectName = "KY-EMS SCADA 工程 - $MachineCode"
$manifest.packageVersion = $PackageVersion
$manifest.createdAt = [DateTimeOffset]::UtcNow.ToString("o")
$manifest.createdBy = $CreatedBy
Write-Json (Join-Path $OutputProjectDirectory "manifest.json") $manifest

$nodes = @(Read-Json (Join-Path $OutputProjectDirectory "nodes.json"))
if ($nodes.Count -ne 1) {
    throw "integrated SCADA project must contain exactly one node"
}
$nodes[0].nodeId = $NodeId
$nodes[0].machineCode = $MachineCode
$nodes[0].displayName = $MachineCode
Write-Json (Join-Path $OutputProjectDirectory "nodes.json") $nodes
Write-Json (Join-Path $OutputProjectDirectory "tags.json") $newTags
Write-Json (Join-Path $OutputProjectDirectory "runtime-map.json") $runtimeMappings

$script:NormalizedActionlessButtonCount = 0
$script:NormalizedUnboundInputCount = 0
$script:RemovedInvalidBindingCount = 0
$script:RemovedInvalidStateRuleCount = 0
$script:RemovedInvalidActionCount = 0
$script:ClearedInvalidStateBindingCount = 0
$outputScreensDirectory = Join-Path $OutputProjectDirectory "screens"
$projectJsonFiles = Get-ChildItem -LiteralPath $OutputProjectDirectory -Recurse -Filter "*.json" -File |
    Where-Object { $_.Name -notin @("manifest.json", "nodes.json", "tags.json", "runtime-map.json", "checksums.json") }
foreach ($file in $projectJsonFiles) {
    $rawJson = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8
    if ($rawJson.Trim() -in @("[]", "{}")) {
        continue
    }
    $json = $rawJson | ConvertFrom-Json
    if ($file.FullName.StartsWith($outputScreensDirectory + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-InvalidScreenReferences $json
    }
    Update-ProjectJsonValue $json
    if ($file.FullName.StartsWith($outputScreensDirectory + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        Normalize-ScreenWidgetSafety $json
    }
    Write-Json $file.FullName $json
}

$referencedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($screenFile in Get-ChildItem -LiteralPath (Join-Path $OutputProjectDirectory "screens") -Filter "*.json" -File) {
    Collect-ReferencedTagIds (Read-Json $screenFile.FullName) $referencedTagIds
}
$mappedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($mapping in $runtimeMappings) { [void]$mappedTagIds.Add([string]$mapping.tagId) }
$unmappedReferenced = @($referencedTagIds | Where-Object { -not $mappedTagIds.Contains($_) } | Sort-Object)

$report = [pscustomobject]@{
    machineCode = $MachineCode
    packageVersion = $PackageVersion
    baselineProjectDirectory = if ($UseBaselineProject) { $BaselineProjectDirectory } else { "" }
    sourceTagCount = $sourceTags.Count
    baselineTagCount = $baselineTagCountForReport
    discoveredPointCount = $points.Count
    mappedTagCount = $mappedSourceTagCount
    unmatchedTagCount = $sourceTags.Count - $mappedSourceTagCount
    ignoredSourceOnlyTagCount = $ignoredSourceOnlyTagCount
    appendedSourceOnlyTagCount = $appendedSourceOnlyTagCount
    appendedDeviceOnlyTagCount = $appendedDeviceOnlyTagCount
    excludedBaselinePlaceholderCount = $excludedBaselinePlaceholderCount
    excludedBaselineWithoutDeviceCount = $excludedBaselineWithoutDeviceCount
    outputTagCount = $newTags.Count
    runtimeMappingCount = $runtimeMappings.Count
    removedInvalidBindingCount = $script:RemovedInvalidBindingCount
    removedInvalidStateRuleCount = $script:RemovedInvalidStateRuleCount
    removedInvalidActionCount = $script:RemovedInvalidActionCount
    clearedInvalidStateBindingCount = $script:ClearedInvalidStateBindingCount
    normalizedActionlessButtonCount = $script:NormalizedActionlessButtonCount
    normalizedUnboundInputCount = $script:NormalizedUnboundInputCount
    referencedTagCount = $referencedTagIds.Count
    mappedReferencedTagCount = $referencedTagIds.Count - $unmappedReferenced.Count
    unmatchedReferencedTagIds = $unmappedReferenced
    matches = $reportRows
}
$packageDirectory = Split-Path -Parent $OutputPackage
if (-not (Test-Path -LiteralPath $packageDirectory)) {
    [void](New-Item -ItemType Directory -Path $packageDirectory -Force)
}
Write-Json $OutputReport $report

$checksumTable = [ordered]@{}
$filesToChecksum = Get-ChildItem -LiteralPath $OutputProjectDirectory -Recurse -File |
    Where-Object { $_.Name -ne "checksums.json" } |
    Sort-Object FullName
foreach ($file in $filesToChecksum) {
    $relative = [System.IO.Path]::GetRelativePath($OutputProjectDirectory, $file.FullName).Replace('\', '/')
    $checksumTable[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
Write-Json (Join-Path $OutputProjectDirectory "checksums.json") ([pscustomobject]$checksumTable)

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $OutputProjectDirectory,
    $OutputPackage,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)

Write-Host "SCADA remap completed"
Write-Host "  machineCode: $MachineCode"
Write-Host "  discovered points: $($points.Count)"
Write-Host "  mapped source tags: $mappedSourceTagCount/$($sourceTags.Count)"
Write-Host "  output tags/routes: $($newTags.Count)/$($runtimeMappings.Count)"
Write-Host "  mapped screen tags: $($referencedTagIds.Count - $unmappedReferenced.Count)/$($referencedTagIds.Count)"
if ($unmappedReferenced.Count -gt 0) {
    Write-Host "  unmatched screen tags:"
    foreach ($tagId in $unmappedReferenced) { Write-Host "    $tagId" }
}
Write-Host "  project: $OutputProjectDirectory"
Write-Host "  package: $OutputPackage"
Write-Host "  report: $OutputReport"
