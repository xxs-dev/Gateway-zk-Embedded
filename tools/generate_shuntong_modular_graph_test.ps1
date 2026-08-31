param(
    [string]$Generator = "tools/generate_shuntong_modular_graph.ps1"
)

$ErrorActionPreference = "Stop"
$testRoot = "tmp/gateway-shuntong-graph-test-" + [guid]::NewGuid().ToString("N")
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Assert-Equal([object]$Actual, [object]$Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message; expected=$Expected actual=$Actual"
    }
}

function Get-Node([object]$Graph, [string]$Id) {
    @($Graph.nodes | Where-Object id -eq $Id)
}

try {
    $graphPath = Join-Path $testRoot "graph.json"
    & $Generator `
        -Output $graphPath `
        -RuntimeOutput (Join-Path $testRoot "runtime.json") `
        -CompatibilityOutput (Join-Path $testRoot "compat.json") `
        -VirtualOutput (Join-Path $testRoot "virtual.json") `
        -RuntimeVirtualOutput (Join-Path $testRoot "runtime-virtual.json") `
        -PhaseControlModeIndex 20128 `
        -PhaseControlModeValue 4

    $graph = Get-Content -Raw -LiteralPath $graphPath | ConvertFrom-Json
    $modeValue = Get-Node $graph "cycle_phase_mode_target"
    $modeWrite = Get-Node $graph "cycle_phase_control_mode"
    $safetyGate = Get-Node $graph "cycle_runtime_safety_gate"
    $startWrite = Get-Node $graph "cycle_start"

    Assert-Equal $modeValue.Count 1 "phase control value node is missing or duplicated"
    Assert-Equal $modeValue[0].parameters.inputs[0].value 4 "phase control value does not use the configured enum"
    Assert-Equal $modeWrite.Count 1 "phase control write node is missing or duplicated"
    Assert-Equal $modeWrite[0].parameters.targetIndex 20128 "phase control write target is not the semantic mode point"
    Assert-Equal $modeWrite[0].parameters.minValue 4 "phase control write minimum does not match the enum"
    Assert-Equal $modeWrite[0].parameters.maxValue 4 "phase control write maximum does not match the enum"
    Assert-Equal $modeWrite[0].ports[2].binding.index 20128 "phase control target port was not regenerated"
    Assert-Equal $safetyGate[0].parameters.conditions[4].index 20128 "runtime safety gate uses the wrong mode point"
    Assert-Equal $safetyGate[0].parameters.conditions[4].value 4 "runtime safety gate uses the wrong mode enum"
    Assert-Equal @(Get-Node $graph "cycle_stop_clear").Count 0 "stop command must not be cleared by writing zero"
    Assert-Equal $startWrite[0].parameters.minValue 1 "start command must only submit its active pulse"
    Assert-Equal $startWrite[0].parameters.permitIndex $startWrite[0].parameters.inputIndex "start command permit must follow the pulse value"

    $safeIndexes = @(
        (Get-Node $graph "cycle_safe_p0")[0].parameters.outputIndex,
        (Get-Node $graph "cycle_safe_p1")[0].parameters.outputIndex,
        (Get-Node $graph "cycle_safe_p2")[0].parameters.outputIndex
    )
    Assert-Equal ($safeIndexes -join ",") "700345,700346,700347" "cycle safety output indexes changed"

    $oldBindings = @(
        $graph.nodes | ForEach-Object { $_.ports } | Where-Object { $_.binding.index -eq 1341 }
    )
    Assert-Equal $oldBindings.Count 0 "generated graph still contains the obsolete phase mode index"
    Write-Host "Shuntong graph semantic control regression test passed."
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
