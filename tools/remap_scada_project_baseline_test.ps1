param(
    [string]$AuditSourcePackage = "",
    [string]$AuditBaselineProjectDirectory = "",
    [string]$AuditDeviceConfigDirectory = "",
    [string]$AuditOverlayDeviceConfigDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-TestJson([string]$Path, [object]$Value) {
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    $json = ConvertTo-Json -InputObject $Value -Depth 100
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw "assertion failed: $Message"
    }
}

function Require-Equal([object]$Expected, [object]$Actual, [string]$Message) {
    if ([string]$Expected -cne [string]$Actual) {
        throw "assertion failed: $Message (expected='$Expected', actual='$Actual')"
    }
}

function Has-TestProperty([object]$Value, [string]$Name) {
    return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name]
}

function Get-TestProperty([object]$Value, [string]$Name, [object]$Default = $null) {
    if (Has-TestProperty $Value $Name) {
        return $Value.PSObject.Properties[$Name].Value
    }
    return $Default
}

function Get-TestReferenceIndex([object]$Value) {
    if ($null -eq $Value -or $Value -isnot [pscustomobject]) {
        return [uint32]0
    }
    foreach ($name in @("indexFallback", "index")) {
        if (Has-TestProperty $Value $name) {
            $parsed = [uint32]0
            if ([uint32]::TryParse([string](Get-TestProperty $Value $name 0), [ref]$parsed) -and $parsed -gt 0) {
                return $parsed
            }
        }
    }
    return [uint32]0
}

function Test-LooksLikeReference([object]$Value) {
    if ($null -eq $Value -or $Value -isnot [pscustomobject]) {
        return $false
    }
    foreach ($name in @("tagId", "indexFallback", "nodeId", "meterCode", "pointCode", "semanticRole", "operator", "comparison", "slot")) {
        if (Has-TestProperty $Value $name) {
            return $true
        }
    }
    return $false
}

function Collect-TestReferences(
    [object]$Value,
    [System.Collections.Generic.HashSet[string]]$TagIds,
    [System.Collections.Generic.HashSet[uint32]]$Indexes
) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [string]) {
        $trimmed = $Value.Trim()
        if (($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) -or
            ($trimmed.StartsWith("[") -and $trimmed.EndsWith("]"))) {
            try {
                Collect-TestReferences ($trimmed | ConvertFrom-Json) $TagIds $Indexes
            } catch {
                # Designer text can resemble JSON without being a reference container.
            }
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string] -and $Value -isnot [pscustomobject]) {
        foreach ($item in $Value) {
            Collect-TestReferences $item $TagIds $Indexes
        }
        return
    }
    if ($Value -isnot [pscustomobject]) {
        return
    }
    if ((Has-TestProperty $Value "tagId") -and -not [string]::IsNullOrWhiteSpace([string]$Value.tagId)) {
        [void]$TagIds.Add([string]$Value.tagId)
    }
    if (Test-LooksLikeReference $Value) {
        $index = Get-TestReferenceIndex $Value
        if ($index -gt 0) {
            [void]$Indexes.Add($index)
        }
    }
    foreach ($property in $Value.PSObject.Properties) {
        Collect-TestReferences $property.Value $TagIds $Indexes
    }
}

function Add-TestWidgets([object]$Value, [System.Collections.Generic.List[object]]$Result) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string] -and $Value -isnot [pscustomobject]) {
        foreach ($item in $Value) {
            Add-TestWidgets $item $Result
        }
        return
    }
    if ($Value -isnot [pscustomobject]) {
        return
    }
    if (Has-TestProperty $Value "widgetId") {
        $Result.Add($Value)
    }
    foreach ($property in $Value.PSObject.Properties) {
        Add-TestWidgets $property.Value $Result
    }
}

function Get-TestWidgets([object[]]$ScreenDocuments) {
    $result = [System.Collections.Generic.List[object]]::new()
    foreach ($screen in $ScreenDocuments) {
        Add-TestWidgets $screen $result
    }
    return $result.ToArray()
}

function Test-ExplicitFalse([object]$Value) {
    if ($null -eq $Value) { return $false }
    if ($Value -is [bool]) { return -not [bool]$Value }
    return [string]$Value -in @("0", "false", "False", "FALSE")
}

function Test-ExplicitTrue([object]$Value) {
    if ($Value -is [bool]) { return [bool]$Value }
    return [string]$Value -in @("1", "true", "True", "TRUE")
}

function Test-TestWidgetHasAction([object]$Widget) {
    $action = Get-TestProperty $Widget "action" $null
    if ($null -ne $action) {
        foreach ($name in @("type", "tagId", "screenId", "targetScreenId", "pageId", "targetPageId")) {
            if (-not [string]::IsNullOrWhiteSpace([string](Get-TestProperty $action $name ""))) {
                return $true
            }
        }
    }
    return @(Get-TestProperty $Widget "actions" @()).Count -gt 0
}

function Test-TestReferenceWritable([object]$Reference, [hashtable]$RouteByTagId) {
    if ($null -eq $Reference -or -not (Has-TestProperty $Reference "tagId")) {
        return $false
    }
    $tagId = [string]$Reference.tagId
    return $RouteByTagId.ContainsKey($tagId) -and [bool]$RouteByTagId[$tagId].writable
}

function Test-TestWidgetHasWritableTarget([object]$Widget, [hashtable]$RouteByTagId) {
    foreach ($binding in @(Get-TestProperty $Widget "bindings" @())) {
        if (Test-TestReferenceWritable $binding $RouteByTagId) { return $true }
    }
    if (Test-TestReferenceWritable (Get-TestProperty $Widget "action" $null) $RouteByTagId) {
        return $true
    }
    foreach ($action in @(Get-TestProperty $Widget "actions" @())) {
        if (Test-TestReferenceWritable $action $RouteByTagId) { return $true }
    }
    return $false
}

function Get-ScreenSafetyAudit([object[]]$ScreenDocuments, [hashtable]$RouteByTagId) {
    $unboundEditableInputs = 0
    $actionlessActionableButtons = 0
    $disabledActionWidgets = 0
    $navigationActions = 0
    $controlActions = 0
    foreach ($widget in @(Get-TestWidgets $ScreenDocuments)) {
        $type = [string](Get-TestProperty $widget "type" (Get-TestProperty $widget "widgetType" ""))
        $properties = Get-TestProperty $widget "properties" $null
        $visible = -not (Test-ExplicitFalse (Get-TestProperty $widget "visible" $true)) -and
            -not (Test-ExplicitFalse (Get-TestProperty $properties "visible" $true))
        $hasAction = Test-TestWidgetHasAction $widget

        if ($type -iin @("qtButton", "QPushButton")) {
            if ($visible -and -not $hasAction -and -not (Test-ExplicitFalse (Get-TestProperty $properties "enabled" $true))) {
                $actionlessActionableButtons++
            }
            if ($hasAction -and (Test-ExplicitFalse (Get-TestProperty $properties "enabled" $true))) {
                $disabledActionWidgets++
            }
            $action = Get-TestProperty $widget "action" $null
            $actionType = [string](Get-TestProperty $action "type" "")
            if ($actionType -ieq "navigate") { $navigationActions++ }
            if ($actionType -ieq "writeSetpoint") { $controlActions++ }
        } elseif ($type -iin @("qtInput", "QLineEdit")) {
            if (-not (Test-TestWidgetHasWritableTarget $widget $RouteByTagId)) {
                $readOnly = Test-ExplicitTrue (Get-TestProperty $properties "readOnly" $false)
                $disabled = Test-ExplicitFalse (Get-TestProperty $properties "enabled" $true)
                if (-not $readOnly -or -not $disabled) {
                    $unboundEditableInputs++
                }
            }
        }
    }
    return [pscustomobject]@{
        unboundEditableInputs = $unboundEditableInputs
        actionlessActionableButtons = $actionlessActionableButtons
        disabledActionWidgets = $disabledActionWidgets
        navigationActions = $navigationActions
        controlActions = $controlActions
    }
}

function New-RouteByTagId([object[]]$Routes) {
    $result = @{}
    foreach ($route in $Routes) {
        $tagId = [string]$route.tagId
        Require (-not $result.ContainsKey($tagId)) "duplicate runtime tagId: $tagId"
        $result[$tagId] = $route
    }
    return $result
}

function Assert-NoProjectDuplicates([object[]]$Tags, [object[]]$Routes, [string]$Context) {
    Require-Equal 0 @($Tags | Group-Object { [uint32]$_.indexFallback } | Where-Object Count -gt 1).Count "$Context duplicate tag index"
    Require-Equal 0 @($Tags | Group-Object tagId | Where-Object Count -gt 1).Count "$Context duplicate tagId"
    Require-Equal 0 @($Routes | Group-Object { [uint32]$_.index } | Where-Object Count -gt 1).Count "$Context duplicate runtime index"
    $routeKeys = @($Routes | ForEach-Object { [string]$_.sharedMemoryName + "`u{1f}" + [string][uint32]$_.index })
    Require-Equal 0 @($routeKeys | Group-Object | Where-Object Count -gt 1).Count "$Context duplicate shared-memory route"
}

function Assert-FakeReferencesRemoved(
    [object[]]$ScreenDocuments,
    [string[]]$FakeTagIds,
    [string[]]$WidgetIds,
    [string]$Context
) {
    $tagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $indexes = [System.Collections.Generic.HashSet[uint32]]::new()
    foreach ($screen in $ScreenDocuments) {
        Collect-TestReferences $screen $tagIds $indexes
    }
    foreach ($tagId in $FakeTagIds) {
        Require (-not $tagIds.Contains($tagId)) "$Context still contains fake direct or embedded tagId $tagId"
    }
    foreach ($index in @(209, 210, 211)) {
        Require (-not $indexes.Contains([uint32]$index)) "$Context still contains fake direct or embedded index $index"
    }

    $widgets = @(Get-TestWidgets $ScreenDocuments)
    foreach ($widgetId in $WidgetIds) {
        $matches = @($widgets | Where-Object { [string]$_.widgetId -eq $widgetId })
        Require-Equal 1 $matches.Count "$Context fake FireFight widget $widgetId"
        $widget = $matches[0]
        Require-Equal 0 @(Get-TestProperty $widget "bindings" @()).Count "$context $widgetId bindings must be cleared"
        Require-Equal 0 @(Get-TestProperty $widget "stateRules" @()).Count "$context $widgetId stateRules must be cleared"
        $properties = Get-TestProperty $widget "properties" $null
        Require-Equal "--" (Get-TestProperty $properties "qtText" "") "$context $widgetId placeholder must be preserved"
        $stateBinding = [string](Get-TestProperty $properties "stateBindingJson" "") | ConvertFrom-Json
        Require-Equal 0 @(Get-TestProperty $stateBinding "states" @()).Count "$context $widgetId embedded states must be cleared"
        Require-Equal "UNKNOWN" (Get-TestProperty (Get-TestProperty $stateBinding "defaultState" $null) "code" "") "$context $widgetId default state must remain UNKNOWN"
    }
}

function Read-ScreenDocuments([string]$ProjectDirectory) {
    return @(
        Get-ChildItem -LiteralPath (Join-Path $ProjectDirectory "screens") -Filter "*.json" -File -Recurse |
            ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json }
    )
}

function New-FixtureDeviceConfig([object[]]$PointSpecs, [bool]$EnableEmsWrite) {
    return [ordered]@{
        memoryStore = [ordered]@{ sharedMemoryName = "gateway_point_store" }
        meters = @([ordered]@{
            meterCode = "CURRENT_DEVICE"
            deviceName = "Current device"
            points = @(
                foreach ($spec in $PointSpecs) {
                    $writable = [bool]$spec.DeviceWritable -or ($EnableEmsWrite -and [uint32]$spec.Index -eq 151)
                    [ordered]@{
                        index = [uint32]$spec.Index
                        pointCode = [string]$spec.PointCode
                        name = "Current metadata $($spec.Index)"
                        read = [ordered]@{ enable = $true; dataType = [string]$spec.DataType; unit = [string]$spec.Unit }
                        write = [ordered]@{ enable = $writable; dataType = [string]$spec.DataType; unit = [string]$spec.Unit }
                    }
                }
            )
        })
    }
}

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gateway-scada-remap-test-" + [guid]::NewGuid().ToString("N"))
$source = Join-Path $fixtureRoot "source"
$baseline = Join-Path $fixtureRoot "baseline"
$deviceConfigs = Join-Path $fixtureRoot "device-configs"
$overlayConfigs = Join-Path $fixtureRoot "overlay-configs"
$output = Join-Path $fixtureRoot "output"
$package = Join-Path $fixtureRoot "package\baseline-remap.kyscada"
$overlayOutput = Join-Path $fixtureRoot "overlay-output"
$overlayPackage = Join-Path $fixtureRoot "package\baseline-remap-overlay.kyscada"

try {
    foreach ($project in @($source, $baseline)) {
        Write-TestJson (Join-Path $project "manifest.json") ([ordered]@{
            projectId = "fixture"
            projectName = "fixture"
            packageVersion = "0.0.0"
            createdAt = "2026-01-01T00:00:00Z"
            createdBy = "test"
        })
        Write-TestJson (Join-Path $project "nodes.json") @(
            [ordered]@{ nodeId = "source-node"; machineCode = "COMM202600001"; displayName = "source" }
        )
    }

    $pointSpecs = @(
        [pscustomobject]@{ Index = 42; Route = "gateway_point_store_ttySP3"; PointCode = "power"; DataType = "float32"; Unit = "kW"; DeviceWritable = $false; InSource = $true },
        [pscustomobject]@{ Index = 151; Route = "gateway_point_store_ems_virtual"; PointCode = "ems_setpoint"; DataType = "float64"; Unit = "kW"; DeviceWritable = $false; InSource = $true },
        [pscustomobject]@{ Index = 985; Route = "gateway_point_store_dio"; PointCode = "dio_985"; DataType = "digital_output"; Unit = ""; DeviceWritable = $true; InSource = $true },
        [pscustomobject]@{ Index = 986; Route = "gateway_point_store_dio"; PointCode = "dio_986"; DataType = "digital_output"; Unit = ""; DeviceWritable = $true; InSource = $true },
        [pscustomobject]@{ Index = 987; Route = "gateway_point_store_dio"; PointCode = "dio_987"; DataType = "digital_output"; Unit = ""; DeviceWritable = $true; InSource = $true },
        [pscustomobject]@{ Index = 988; Route = "gateway_point_store_dio"; PointCode = "dio_988"; DataType = "digital_output"; Unit = ""; DeviceWritable = $true; InSource = $true },
        [pscustomobject]@{ Index = 991; Route = "gateway_point_store_dio"; PointCode = "dio_991"; DataType = "digital_output"; Unit = ""; DeviceWritable = $true; InSource = $true },
        [pscustomobject]@{ Index = 500; Route = "gateway_point_store_ttySP1"; PointCode = "baseline_only"; DataType = "uint16"; Unit = "V"; DeviceWritable = $false; InSource = $false }
    )
    $sourceTags = [System.Collections.Generic.List[object]]::new()
    $sourceRoutes = [System.Collections.Generic.List[object]]::new()
    $baselineTags = [System.Collections.Generic.List[object]]::new()
    $baselineRoutes = [System.Collections.Generic.List[object]]::new()
    $sourceTagIdByIndex = @{}
    $targetTagIdByIndex = @{}

    foreach ($spec in $pointSpecs) {
        $index = [uint32]$spec.Index
        $baselineTagId = "COMM202600999_DEVICE.point_$index"
        $targetTagIdByIndex[[string]$index] = $baselineTagId -replace 'COMM\d{9}', 'COMM202600104'
        $baselineTags.Add([pscustomobject][ordered]@{
            tagId = $baselineTagId; nodeId = "old-node"; deviceId = "COMM202600999_DEVICE"; meterCode = "COMM202600999_DEVICE"
            pointCode = "point_$index"; displayName = "Baseline point $index"; unit = "old"; dataType = "int16"; access = "read"; indexFallback = $index
        })
        $baselineRoutes.Add([pscustomobject][ordered]@{
            nodeId = "old-node"; tagId = $baselineTagId; sharedMemoryName = [string]$spec.Route; index = $index
            writable = $false; dataType = "int16"; unit = "old"
        })
        if ([bool]$spec.InSource) {
            $sourceTagId = "SOURCE_DEVICE.source_$index"
            $sourceTagIdByIndex[[string]$index] = $sourceTagId
            $sourceWritable = $index -eq 151 -or $index -in @(985, 986, 987, 988, 991)
            $sourceTags.Add([pscustomobject][ordered]@{
                tagId = $sourceTagId; nodeId = "source-node"; deviceId = "SOURCE_DEVICE"; meterCode = "SOURCE_DEVICE"
                pointCode = "source_$index"; displayName = "Source point $index"; unit = "W"; dataType = "float64"
                access = if ($sourceWritable) { "readWrite" } else { "read" }; indexFallback = $index
            })
            $sourceRoutes.Add([pscustomobject][ordered]@{
                nodeId = "source-node"; tagId = $sourceTagId; sharedMemoryName = "gateway_point_store"; index = $index
                writable = $sourceWritable; dataType = "float64"; unit = "W"
            })
        }
    }

    $baselineTags.Add([pscustomobject][ordered]@{
        tagId = "DISPLAY_RUNTIME.placeholder_900"; nodeId = "old-node"; deviceId = "DISPLAY_RUNTIME"; meterCode = "DISPLAY_RUNTIME"
        pointCode = "placeholder_900"; displayName = "Placeholder"; unit = ""; dataType = "float64"; access = "read"; indexFallback = 900
    })
    $baselineRoutes.Add([pscustomobject][ordered]@{
        nodeId = "old-node"; tagId = "DISPLAY_RUNTIME.placeholder_900"; sharedMemoryName = "gateway_point_store"; index = 900
        writable = $false; dataType = "float64"; unit = ""
    })

    $fakeTagIds = @()
    foreach ($index in @(209, 210, 211)) {
        $tagId = "SOURCE_FIRE.fake_$index"
        $fakeTagIds += $tagId
        $sourceTagIdByIndex[[string]$index] = $tagId
        $sourceTags.Add([pscustomobject][ordered]@{
            tagId = $tagId; nodeId = "source-node"; deviceId = "SOURCE_FIRE"; meterCode = "SOURCE_FIRE"
            pointCode = "fake_$index"; displayName = "Fake $index"; unit = ""; dataType = "bool"; access = "read"; indexFallback = $index
        })
        $sourceRoutes.Add([pscustomobject][ordered]@{
            nodeId = "source-node"; tagId = $tagId; sharedMemoryName = "gateway_point_store"; index = $index
            writable = $false; dataType = "bool"; unit = ""
        })
    }

    Write-TestJson (Join-Path $source "tags.json") $sourceTags
    Write-TestJson (Join-Path $source "runtime-map.json") $sourceRoutes
    Write-TestJson (Join-Path $baseline "tags.json") $baselineTags
    Write-TestJson (Join-Path $baseline "runtime-map.json") $baselineRoutes

    $embeddedDesignerJson = ConvertTo-Json -Compress -Depth 20 -InputObject ([ordered]@{
        binding = [ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex["42"] }
        action = [ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex["151"]; index = 151 }
    })
    $widgets = [System.Collections.Generic.List[object]]::new()
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "normal-value"; type = "qtValue"; bindings = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex["42"]; slot = "value" })
        stateRules = @([ordered]@{ conditions = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex["42"]; comparison = "gt"; value = "0" }) })
        action = $null; properties = [ordered]@{ qtText = "--" }
    })
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "embedded-valid"; type = "qtLabel"; bindings = @(); stateRules = @(); action = $null
        properties = [ordered]@{ qtText = "--"; designerJson = $embeddedDesignerJson }
    })
    foreach ($index in @(209, 210, 211)) {
        $stateBindingJson = ConvertTo-Json -Depth 20 -InputObject ([ordered]@{
            matchMode = "first"
            defaultState = [ordered]@{ code = "UNKNOWN"; label = "--"; color = "#71808A"; image = "" }
            states = @(
                [ordered]@{ code = "TRIGGERED"; conditions = @([ordered]@{ pointCode = "fake_$index"; index = $index; operator = "ne"; value = "0" }) },
                [ordered]@{ code = "NORMAL"; conditions = @([ordered]@{ pointCode = "fake_$index"; index = $index; operator = "eq"; value = "0" }) }
            )
        })
        $widgets.Add([pscustomobject][ordered]@{
            widgetId = "fake-$index"; type = "qtValue"; visible = $true
            bindings = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex[[string]$index]; slot = "value" })
            stateRules = @(
                [ordered]@{ code = "TRIGGERED"; conditions = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex[[string]$index]; comparison = "ne"; value = "0" }) },
                [ordered]@{ code = "NORMAL"; conditions = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex[[string]$index]; comparison = "eq"; value = "0" }) }
            )
            action = $null; properties = [ordered]@{ qtText = "--"; stateBindingJson = $stateBindingJson }
        })
    }
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "actionless"; type = "qtButton"; visible = $true; bindings = @(); action = $null
        properties = [ordered]@{ enabled = $true; qtText = "No action" }
    })
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "unbound-input"; type = "qtInput"; visible = $true; bindings = @(); action = $null
        properties = [ordered]@{ enabled = $true; readOnly = $false; qtText = "" }
    })
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "navigation"; type = "qtButton"; visible = $true; bindings = @()
        action = [ordered]@{ type = "navigate"; targetScreenId = "other" }; properties = [ordered]@{ enabled = $true }
    })
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "control"; type = "qtButton"; visible = $true; bindings = @()
        action = [ordered]@{ type = "writeSetpoint"; nodeId = "source-node"; tagId = $sourceTagIdByIndex["985"]; indexFallback = 985 }
        properties = [ordered]@{ enabled = $true }
    })
    $widgets.Add([pscustomobject][ordered]@{
        widgetId = "writable-input"; type = "qtInput"; visible = $true
        bindings = @([ordered]@{ nodeId = "source-node"; tagId = $sourceTagIdByIndex["985"]; slot = "value" })
        action = $null; properties = [ordered]@{ enabled = $true; readOnly = $false }
    })
    Write-TestJson (Join-Path $source "screens\Main.json") ([ordered]@{ screenId = "main"; widgets = $widgets })

    Write-TestJson (Join-Path $deviceConfigs "device_all.json") (New-FixtureDeviceConfig $pointSpecs $false)
    Write-TestJson (Join-Path $overlayConfigs "device_all.json") (New-FixtureDeviceConfig $pointSpecs $true)

    & (Join-Path $PSScriptRoot "remap_scada_project.ps1") `
        -SourceProjectDirectory $source `
        -BaselineProjectDirectory $baseline `
        -DeviceConfigDirectory $deviceConfigs `
        -OutputProjectDirectory $output `
        -OutputPackage $package `
        -MachineCode "COMM202600104" `
        -PackageVersion "test-baseline"

    $outputTags = @(Get-Content -LiteralPath (Join-Path $output "tags.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
    $outputRoutes = @(Get-Content -LiteralPath (Join-Path $output "runtime-map.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
    $screenDocuments = @(Read-ScreenDocuments $output)
    $screen = $screenDocuments[0]
    $report = Get-Content -LiteralPath ($package + ".remap-report.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $routeByTagId = New-RouteByTagId $outputRoutes

    Require-Equal 8 $outputTags.Count "fixture output must equal authoritative device indexes"
    Require-Equal 8 $outputRoutes.Count "fixture runtime routes must equal authoritative device indexes"
    Assert-NoProjectDuplicates $outputTags $outputRoutes "fixture"
    Require-Equal 0 @($outputTags | Where-Object { [string]$_.tagId -like "DISPLAY_RUNTIME*" }).Count "fixture DISPLAY_RUNTIME tags"
    Require-Equal 0 @($outputRoutes | Where-Object { [string]$_.tagId -like "DISPLAY_RUNTIME*" }).Count "fixture DISPLAY_RUNTIME routes"
    Require-Equal 1 @($outputTags | Where-Object { [uint32]$_.indexFallback -eq 500 }).Count "baseline-only device tag must survive"

    $ttyRoute = @($outputRoutes | Where-Object { [uint32]$_.index -eq 42 })[0]
    Require-Equal "gateway_point_store_ttySP3" $ttyRoute.sharedMemoryName "ttySP route must come from baseline"
    Require ($ttyRoute.sharedMemoryName -cne "gateway_point_store") "ttySP route must not fall back to gateway_point_store"
    Require-Equal "float32" $ttyRoute.dataType "device dataType must overlay baseline"
    Require-Equal "kW" $ttyRoute.unit "device unit must overlay baseline"
    $ttyTag = @($outputTags | Where-Object { [uint32]$_.indexFallback -eq 42 })[0]
    Require-Equal $targetTagIdByIndex["42"] $ttyTag.tagId "baseline tag identity must be preserved"
    Require-Equal "Baseline point 42" $ttyTag.displayName "baseline display name must be preserved"

    $emsRoute = @($outputRoutes | Where-Object { [uint32]$_.index -eq 151 })[0]
    $emsTag = @($outputTags | Where-Object { [uint32]$_.indexFallback -eq 151 })[0]
    Require (-not [bool]$emsRoute.writable) "source writable must not override authoritative device read-only"
    Require-Equal "read" $emsTag.access "source access must not override authoritative device read-only"
    foreach ($index in @(985, 986, 987, 988, 991)) {
        $dioRoute = @($outputRoutes | Where-Object { [uint32]$_.index -eq $index })[0]
        $dioTag = @($outputTags | Where-Object { [uint32]$_.indexFallback -eq $index })[0]
        Require-Equal "gateway_point_store_dio" $dioRoute.sharedMemoryName "DIO baseline route $index"
        Require ([bool]$dioRoute.writable) "DIO writable $index"
        Require-Equal "readWrite" $dioTag.access "DIO access $index"
    }

    $normalBinding = @($screen.widgets | Where-Object widgetId -eq "normal-value")[0].bindings[0]
    Require-Equal $targetTagIdByIndex["42"] $normalBinding.tagId "normal binding must map by index"
    $normalRule = @($screen.widgets | Where-Object widgetId -eq "normal-value")[0].stateRules[0]
    Require-Equal $targetTagIdByIndex["42"] $normalRule.conditions[0].tagId "state condition must map by index"
    $control = @($screen.widgets | Where-Object widgetId -eq "control")[0]
    Require-Equal $targetTagIdByIndex["985"] $control.action.tagId "control Action must map by index"
    $embeddedOutput = @($screen.widgets | Where-Object widgetId -eq "embedded-valid")[0].properties.designerJson | ConvertFrom-Json
    Require-Equal $targetTagIdByIndex["42"] $embeddedOutput.binding.tagId "embedded binding must map by index"
    Require-Equal $targetTagIdByIndex["151"] $embeddedOutput.action.tagId "embedded Action must map by index"

    Assert-FakeReferencesRemoved $screenDocuments $fakeTagIds @("fake-209", "fake-210", "fake-211") "fixture"
    $safety = Get-ScreenSafetyAudit $screenDocuments $routeByTagId
    Require-Equal 0 $safety.unboundEditableInputs "fixture unbound editable inputs"
    Require-Equal 0 $safety.actionlessActionableButtons "fixture actionless actionable buttons"
    Require-Equal 0 $safety.disabledActionWidgets "fixture navigation/control actions must remain enabled"
    Require-Equal 1 $safety.navigationActions "fixture navigation action count"
    Require-Equal 1 $safety.controlActions "fixture control action count"

    Require-Equal 3 $report.referencedTagCount "fixture valid referenced tags"
    Require-Equal 0 @($report.unmatchedReferencedTagIds).Count "fixture references must all resolve"
    Require-Equal 3 $report.ignoredSourceOnlyTagCount "fixture fake source-only tags"
    Require-Equal 1 $report.excludedBaselinePlaceholderCount "fixture excluded DISPLAY_RUNTIME count"
    Require-Equal 3 $report.removedInvalidBindingCount "fixture invalid binding removals"
    Require-Equal 6 $report.removedInvalidStateRuleCount "fixture invalid state rule removals"
    Require-Equal 3 $report.clearedInvalidStateBindingCount "fixture embedded state clear count"
    Require-Equal 1 $report.normalizedActionlessButtonCount "fixture normalized actionless buttons"
    Require-Equal 1 $report.normalizedUnboundInputCount "fixture normalized unbound inputs"
    Require (Test-Path -LiteralPath $package -PathType Leaf) "fixture package must be created"

    & (Join-Path $PSScriptRoot "remap_scada_project.ps1") `
        -SourceProjectDirectory $source `
        -BaselineProjectDirectory $baseline `
        -DeviceConfigDirectory $deviceConfigs `
        -OverlayDeviceConfigDirectory $overlayConfigs `
        -OutputProjectDirectory $overlayOutput `
        -OutputPackage $overlayPackage `
        -MachineCode "COMM202600104" `
        -PackageVersion "test-overlay"
    $overlayTags = @(Get-Content -LiteralPath (Join-Path $overlayOutput "tags.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
    $overlayRoutes = @(Get-Content -LiteralPath (Join-Path $overlayOutput "runtime-map.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
    $overlayEmsRoute = @($overlayRoutes | Where-Object { [uint32]$_.index -eq 151 })[0]
    $overlayEmsTag = @($overlayTags | Where-Object { [uint32]$_.indexFallback -eq 151 })[0]
    Require ([bool]$overlayEmsRoute.writable) "overlay device config must enable EMS index 151"
    Require-Equal "readWrite" $overlayEmsTag.access "overlay device config must enable EMS tag access"

    Write-Host "PASS remap_scada_project synthetic baseline regression"
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

$auditArguments = @(
    $AuditSourcePackage,
    $AuditBaselineProjectDirectory,
    $AuditDeviceConfigDirectory,
    $AuditOverlayDeviceConfigDirectory
)
$providedAuditArgumentCount = @($auditArguments | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
if ($providedAuditArgumentCount -notin @(0, 4)) {
    throw "all four COMM104 audit paths must be provided together"
}
if ($providedAuditArgumentCount -eq 4) {
    Require (Test-Path -LiteralPath $AuditSourcePackage -PathType Leaf) "audit source package does not exist"
    Require (Test-Path -LiteralPath $AuditBaselineProjectDirectory -PathType Container) "audit baseline project does not exist"
    Require (Test-Path -LiteralPath $AuditDeviceConfigDirectory -PathType Container) "audit device config directory does not exist"
    Require (Test-Path -LiteralPath $AuditOverlayDeviceConfigDirectory -PathType Container) "audit overlay device config directory does not exist"

    $auditRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gateway-scada-remap-comm104-audit-" + [guid]::NewGuid().ToString("N"))
    $auditSource = Join-Path $auditRoot "source"
    $auditOutput = Join-Path $auditRoot "output"
    $auditPackage = Join-Path $auditRoot "package\comm104-audit.kyscada"
    try {
        [void](New-Item -ItemType Directory -Path $auditSource -Force)
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($AuditSourcePackage, $auditSource)
        $auditSourceTags = @(Get-Content -LiteralPath (Join-Path $auditSource "tags.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
        $fakeSourceTags = @($auditSourceTags | Where-Object { [uint32]$_.indexFallback -in @(209, 210, 211) })
        Require-Equal 3 $fakeSourceTags.Count "COMM104 source fake FireFight tags"

        & (Join-Path $PSScriptRoot "remap_scada_project.ps1") `
            -SourceProjectDirectory $auditSource `
            -BaselineProjectDirectory $AuditBaselineProjectDirectory `
            -DeviceConfigDirectory $AuditDeviceConfigDirectory `
            -OverlayDeviceConfigDirectory $AuditOverlayDeviceConfigDirectory `
            -OutputProjectDirectory $auditOutput `
            -OutputPackage $auditPackage `
            -MachineCode "COMM202600104" `
            -PackageVersion "comm104-baseline-audit"

        $auditTags = @(Get-Content -LiteralPath (Join-Path $auditOutput "tags.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
        $auditRoutes = @(Get-Content -LiteralPath (Join-Path $auditOutput "runtime-map.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
        $auditReport = Get-Content -LiteralPath ($auditPackage + ".remap-report.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        $screenDocuments = @(Read-ScreenDocuments $auditOutput)
        $routeByTagId = New-RouteByTagId $auditRoutes
        $sourceIndexSet = [System.Collections.Generic.HashSet[uint32]]::new()
        $liveIndexSet = [System.Collections.Generic.HashSet[uint32]]::new()
        $sourceTagsByIndex = @{}
        foreach ($tag in $auditSourceTags) {
            $index = Get-TestReferenceIndex $tag
            Require ($index -gt 0) "COMM104 source tag has no valid index: $([string]$tag.tagId)"
            Require ($sourceIndexSet.Add($index)) "COMM104 duplicate source index: $index"
            $sourceTagsByIndex[[string]$index] = $tag
        }
        foreach ($tag in $auditTags) {
            $index = Get-TestReferenceIndex $tag
            Require ($index -gt 0) "COMM104 output tag has no valid index: $([string]$tag.tagId)"
            Require ($liveIndexSet.Add($index)) "COMM104 duplicate output index: $index"
        }
        $mappedSourceIndexes = @($sourceIndexSet | Where-Object { $liveIndexSet.Contains($_) })
        $sourceOnlyIndexes = @($sourceIndexSet | Where-Object { -not $liveIndexSet.Contains($_) })

        $sourceScreenDocuments = @(Read-ScreenDocuments $auditSource)
        $sourceReferencedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $sourceReferencedIndexes = [System.Collections.Generic.HashSet[uint32]]::new()
        foreach ($screen in $sourceScreenDocuments) {
            Collect-TestReferences $screen $sourceReferencedTagIds $sourceReferencedIndexes
        }
        $referencedSourceOnlyIndexes = @(
            $sourceOnlyIndexes | Where-Object {
                $sourceTag = $sourceTagsByIndex[[string]$_]
                $sourceReferencedIndexes.Contains([uint32]$_) -or
                    $sourceReferencedTagIds.Contains([string]$sourceTag.tagId)
            }
        )
        $referencedTagIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $embeddedReferenceIndexes = [System.Collections.Generic.HashSet[uint32]]::new()
        foreach ($screen in $screenDocuments) {
            Collect-TestReferences $screen $referencedTagIds $embeddedReferenceIndexes
        }

        Require-Equal 1896 $auditTags.Count "COMM104 output tags must equal fresh device Index set"
        Require-Equal 1896 $auditRoutes.Count "COMM104 output routes must equal fresh device Index set"
        Require-Equal 1896 $auditReport.outputTagCount "COMM104 report output tag count"
        Require-Equal 1896 $auditReport.runtimeMappingCount "COMM104 report runtime count"
        Require-Equal $mappedSourceIndexes.Count $auditReport.mappedTagCount "COMM104 source/live index intersection mapped"
        Require-Equal $sourceOnlyIndexes.Count $auditReport.unmatchedTagCount "COMM104 source-only indexes excluded"
        Require-Equal $sourceOnlyIndexes.Count $auditReport.ignoredSourceOnlyTagCount "COMM104 ignored source-only indexes"
        Require-Equal $fakeSourceTags.Count $referencedSourceOnlyIndexes.Count "COMM104 page-referenced source-only indexes"
        foreach ($fakeSourceTag in $fakeSourceTags) {
            Require ($referencedSourceOnlyIndexes -contains [uint32]$fakeSourceTag.indexFallback) "COMM104 fake FireFight index is not page-referenced: $([uint32]$fakeSourceTag.indexFallback)"
        }
        Require-Equal 13 $auditReport.excludedBaselinePlaceholderCount "COMM104 excluded DISPLAY_RUNTIME count"
        Require-Equal 0 $auditReport.excludedBaselineWithoutDeviceCount "COMM104 non-placeholder baseline points missing from device configs"
        Assert-NoProjectDuplicates $auditTags $auditRoutes "COMM104"
        Require-Equal 0 @($auditTags | Where-Object { [string]$_.tagId -like "DISPLAY_RUNTIME*" }).Count "COMM104 DISPLAY_RUNTIME tags"
        Require-Equal 0 @($auditRoutes | Where-Object { [string]$_.tagId -like "DISPLAY_RUNTIME*" }).Count "COMM104 DISPLAY_RUNTIME routes"

        $fireFightScreens = @($screenDocuments | Where-Object { [string]$_.screenId -eq "FireFight" })
        Require-Equal 1 $fireFightScreens.Count "COMM104 FireFight screen"
        Assert-FakeReferencesRemoved `
            $fireFightScreens `
            @($fakeSourceTags | ForEach-Object { [string]$_.tagId }) `
            @("qt_label_126", "qt_label_127", "qt_label_128") `
            "COMM104"
        Require-Equal 3 $auditReport.removedInvalidBindingCount "COMM104 invalid binding removals"
        Require-Equal 6 $auditReport.removedInvalidStateRuleCount "COMM104 invalid state rule removals"
        Require-Equal 3 $auditReport.clearedInvalidStateBindingCount "COMM104 embedded state clear count"

        Require-Equal 152 $referencedTagIds.Count "COMM104 unique mapped screen tag references"
        Require-Equal 152 $auditReport.referencedTagCount "COMM104 report screen tag references"
        Require-Equal 0 @($auditReport.unmatchedReferencedTagIds).Count "COMM104 screen references must all resolve"
        $referencedRoutes = [System.Collections.Generic.List[object]]::new()
        $referencedIndexes = [System.Collections.Generic.HashSet[uint32]]::new()
        foreach ($tagId in $referencedTagIds) {
            Require ($routeByTagId.ContainsKey($tagId)) "COMM104 referenced tag has no runtime route: $tagId"
            $route = $routeByTagId[$tagId]
            $referencedRoutes.Add($route)
            [void]$referencedIndexes.Add([uint32]$route.index)
        }
        Require-Equal 152 $referencedIndexes.Count "COMM104 referenced indexes"

        $expectedRouteCounts = [ordered]@{
            gateway_point_store_ttySP4 = 48
            gateway_point_store_ttySP1 = 32
            gateway_point_store_ttySP3 = 31
            gateway_point_store_can0_zhenghua = 12
            gateway_point_store_dio = 11
            gateway_point_store_ems_virtual = 9
            gateway_point_store_ttySP5 = 9
        }
        $actualRouteCounts = @{}
        foreach ($group in $referencedRoutes | Group-Object sharedMemoryName) {
            $actualRouteCounts[[string]$group.Name] = $group.Count
        }
        Require-Equal $expectedRouteCounts.Count $actualRouteCounts.Count "COMM104 referenced runtime route groups"
        foreach ($routeSpec in $expectedRouteCounts.GetEnumerator()) {
            Require ($actualRouteCounts.ContainsKey([string]$routeSpec.Key)) "COMM104 missing route group $($routeSpec.Key)"
            Require-Equal $routeSpec.Value $actualRouteCounts[[string]$routeSpec.Key] "COMM104 route count $($routeSpec.Key)"
        }
        Require (-not $actualRouteCounts.ContainsKey("gateway_point_store")) "COMM104 references must not use the default route"

        $baselineRoutes = @(Get-Content -LiteralPath (Join-Path $AuditBaselineProjectDirectory "runtime-map.json") -Raw -Encoding UTF8 | ConvertFrom-Json)
        $baselineRouteByIndex = @{}
        foreach ($route in $baselineRoutes) {
            $indexKey = [string][uint32]$route.index
            Require (-not $baselineRouteByIndex.ContainsKey($indexKey)) "baseline duplicate route index $indexKey"
            $baselineRouteByIndex[$indexKey] = $route
        }
        foreach ($route in $referencedRoutes) {
            $indexKey = [string][uint32]$route.index
            Require ($baselineRouteByIndex.ContainsKey($indexKey)) "referenced index missing from baseline $indexKey"
            Require-Equal $baselineRouteByIndex[$indexKey].sharedMemoryName $route.sharedMemoryName "baseline route preserved for $indexKey"
        }

        foreach ($index in @(151, 156, 161, 162)) {
            $route = @($auditRoutes | Where-Object { [uint32]$_.index -eq $index })[0]
            $tag = @($auditTags | Where-Object { [uint32]$_.indexFallback -eq $index })[0]
            Require-Equal "gateway_point_store_ems_virtual" $route.sharedMemoryName "COMM104 EMS route $index"
            Require ([bool]$route.writable) "COMM104 EMS overlay writable $index"
            Require-Equal "readWrite" $tag.access "COMM104 EMS overlay access $index"
        }
        foreach ($index in @(985, 986, 987, 988, 991)) {
            $route = @($auditRoutes | Where-Object { [uint32]$_.index -eq $index })[0]
            $tag = @($auditTags | Where-Object { [uint32]$_.indexFallback -eq $index })[0]
            Require-Equal "gateway_point_store_dio" $route.sharedMemoryName "COMM104 DIO route $index"
            Require ([bool]$route.writable) "COMM104 DIO writable $index"
            Require-Equal "readWrite" $tag.access "COMM104 DIO access $index"
        }

        $safety = Get-ScreenSafetyAudit $screenDocuments $routeByTagId
        Require-Equal 0 $safety.unboundEditableInputs "COMM104 unbound editable inputs"
        Require-Equal 0 $safety.actionlessActionableButtons "COMM104 actionless actionable buttons"
        Require-Equal 0 $safety.disabledActionWidgets "COMM104 navigation/control actions remain enabled"
        Require-Equal 141 $safety.navigationActions "COMM104 navigation actions preserved"
        Require-Equal 10 $safety.controlActions "COMM104 control actions preserved"
        Require-Equal 65 $auditReport.normalizedActionlessButtonCount "COMM104 normalized actionless buttons"
        Require-Equal 31 $auditReport.normalizedUnboundInputCount "COMM104 normalized unbound inputs"
        Require (Test-Path -LiteralPath $auditPackage -PathType Leaf) "COMM104 audit package must be created"

        Write-Host "COMM104 source/live sets: source=$($sourceIndexSet.Count), live=$($liveIndexSet.Count), intersection=$($mappedSourceIndexes.Count), sourceOnly=$($sourceOnlyIndexes.Count), sourceOnlyReferenced=$($referencedSourceOnlyIndexes.Count), sourceOnlyUnreferenced=$($sourceOnlyIndexes.Count - $referencedSourceOnlyIndexes.Count)"
        Write-Host "PASS COMM202600104 real-project baseline remap audit"
    } finally {
        if (Test-Path -LiteralPath $auditRoot) {
            Remove-Item -LiteralPath $auditRoot -Recurse -Force
        }
    }
}
