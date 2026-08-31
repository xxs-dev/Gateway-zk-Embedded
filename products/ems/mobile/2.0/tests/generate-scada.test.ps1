[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\..\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("gateway-ems2-test-" + [Guid]::NewGuid().ToString("N"))
$password = ConvertTo-SecureString "ems2-test-only" -AsPlainText -Force

try {
    & (Join-Path $repoRoot "tools\release\build-ems-mobile.ps1") `
        -MachineCode COMM202600999 `
        -LocalOperatorPassword $password `
        -SourceProjectDirectory (Join-Path $repoRoot "products\ems\mobile\2.0\scada\base-project") `
        -OutputDirectory $fixtureRoot `
        -DisplayName "EMS 2.0 Test"

    $projectRoot = Join-Path $fixtureRoot "scada-project"
    $manifest = Get-Content -LiteralPath (Join-Path $projectRoot "manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$manifest.entryScreen -ne "Meters-Overview") {
        throw "XMind V2 must enter the public instrument overview"
    }
    $screens = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot "screens") -Filter "*.json" -File)
    if ($screens.Count -ne 34) { throw "XMind V2 must contain 34 screens" }
    foreach ($required in @("Guide-Setup", "Meters-Electrical-Output", "Topology", "Vehicle-Control", "Info-Messages", "Maintenance")) {
        if (-not (Test-Path -LiteralPath (Join-Path $projectRoot "screens\$required.json"))) {
            throw "XMind V2 screen is missing: $required"
        }
    }
    $permissions = Get-Content -LiteralPath (Join-Path $projectRoot "permissions.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    if (@($permissions.localAccess.protectedScreenPrefixes) -join ',' -ne 'Guide-,Vehicle-,Strategy-,Control-') {
        throw "XMind V2 protected screen prefixes are incomplete"
    }
    $pcsDevice = Get-Content -LiteralPath (Join-Path $projectRoot "screens\Devices-Pcs.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $pcsWidgetById = @{}
    foreach ($widget in @($pcsDevice.widgets)) { $pcsWidgetById[[string]$widget.widgetId] = $widget }
    foreach ($group in @(@("pcs-va", "pcs-vb", "pcs-vc"), @("pcs-ia", "pcs-ib", "pcs-ic"))) {
        $values = @($group | ForEach-Object { $pcsWidgetById["$($_)-value"] })
        if (@($values | Where-Object { $null -eq $_ }).Count -ne 0) {
            throw "PCS voltage/current column widget is missing"
        }
        $actualY = @($values.geometry.y) -join ','
        $sortedY = @($values.geometry.y | Sort-Object) -join ','
        if (@($values.geometry.x | Select-Object -Unique).Count -ne 1 -or
            $actualY -ne $sortedY) {
            throw "PCS phase voltage/current values must run vertically from A to B to C"
        }
    }
    $bmsDevice = Get-Content -LiteralPath (Join-Path $projectRoot "screens\Devices-Bms.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $bmsWidgetById = @{}
    foreach ($widget in @($bmsDevice.widgets)) { $bmsWidgetById[[string]$widget.widgetId] = $widget }
    foreach ($group in @(
        @("bms-voltage", "bms-current"), @("bms-soh", "bms-temperature"),
        @("bms-cell-vmax", "bms-cell-vmin"), @("bms-cell-tmax", "bms-cell-tmin"),
        @("bms-charge", "bms-discharge")
    )) {
        $values = @($group | ForEach-Object { $bmsWidgetById["$($_)-value"] })
        if (@($values | Where-Object { $null -eq $_ }).Count -ne 0 -or
            @($values.geometry.x | Select-Object -Unique).Count -ne 1 -or
            [double]$values[0].geometry.y -ge [double]$values[1].geometry.y) {
            throw "BMS related metrics must be arranged as vertical pairs"
        }
    }
    $energyScreen = Get-Content -LiteralPath (Join-Path $projectRoot "screens\Meters-Energy.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $energyWidgetById = @{}
    foreach ($widget in @($energyScreen.widgets)) { $energyWidgetById[[string]$widget.widgetId] = $widget }
    foreach ($period in @("last", "today", "total")) {
        $charge = $energyWidgetById["energy-storage-1-$period-charge-value"]
        $discharge = $energyWidgetById["energy-storage-1-$period-discharge-value"]
        if ($null -eq $charge -or $null -eq $discharge -or
            [double]$charge.geometry.x -ne [double]$discharge.geometry.x -or
            [double]$charge.geometry.y -ge [double]$discharge.geometry.y) {
            throw "Energy charge/discharge metrics must be paired vertically by period"
        }
    }
    $allScreens = @($screens | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json })
    $invalidCharts = @($allScreens.widgets | Where-Object {
        $_.type -eq "qtChart" -and [int]$_.properties.chartDefaultWindowMinutes -ne 60
    })
    if ($invalidCharts.Count -ne 0) { throw "XMind V2 charts must default to one hour" }
    $pcsControl = Get-Content -LiteralPath (Join-Path $projectRoot "screens\Control-Pcs.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $phaseControl = @($pcsControl.widgets | Where-Object type -eq "pcsPhasePowerControl")
    if ($phaseControl.Count -eq 1) {
        $runtimeMap = Get-Content -LiteralPath (Join-Path $projectRoot "runtime-map.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        $indexByTag = @{}
        foreach ($mapping in $runtimeMap) { $indexByTag[[string]$mapping.tagId] = [uint32]$mapping.index }
        $boundIndexes = @($phaseControl[0].bindings | ForEach-Object { $indexByTag[[string]$_.tagId] } | Sort-Object)
        if (@(Compare-Object ([uint32[]]@(1318,1319,1320,1321,1322,1323)) $boundIndexes).Count -ne 0) {
            throw "PCS phase power control must bind indexes 1318-1323"
        }
    }
    Write-Host "generate-scada.test passed"
} finally {
    $tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixtureRoot)
    if ((Test-Path -LiteralPath $resolvedFixture) -and
        $resolvedFixture.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedFixture) -like 'gateway-ems2-test-*') {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
