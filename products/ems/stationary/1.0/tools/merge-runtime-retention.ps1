[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceVirtualConfig,

    [Parameter(Mandatory = $true)]
    [string]$CompatibilityVirtualConfig,

    [Parameter(Mandatory = $true)]
    [string]$OutputVirtualConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-Json([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-Json([string]$Path, [object]$Value) {
    $parent = Split-Path -Parent $Path
    [void](New-Item -ItemType Directory -Path $parent -Force)
    $json = ConvertTo-Json -InputObject $Value -Depth 100
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

$sourcePath = (Resolve-Path -LiteralPath $SourceVirtualConfig).Path
$compatibilityPath = (Resolve-Path -LiteralPath $CompatibilityVirtualConfig).Path
$source = Read-Json $sourcePath
$compatibility = Read-Json $compatibilityPath

if ([string]$source.memoryStore.sharedMemoryName -ne [string]$compatibility.memoryStore.sharedMemoryName) {
    throw "EMS virtual shared-memory names do not match"
}
if (@($source.meters).Count -ne 1 -or @($compatibility.meters).Count -ne 1) {
    throw "EMS virtual retention merge requires one meter in each config"
}

$sourcePoints = [System.Collections.Generic.List[object]]::new()
foreach ($point in @($source.meters[0].points)) {
    $sourcePoints.Add($point)
}
$pointCodes = @{}
$indexes = @{}
foreach ($point in $sourcePoints) {
    $pointCodes[[string]$point.pointCode] = $true
    $indexes[[uint32]$point.index] = $true
}

$retainedPoints = @(
    $compatibility.meters[0].points |
        Where-Object { [uint32]$_.index -ge 400 -and [uint32]$_.index -le 447 } |
        Sort-Object { [uint32]$_.index }
)
if ($retainedPoints.Count -ne 48) {
    throw "compatibility config must contain exactly 48 EMS 1.0 retained points"
}

$added = 0
foreach ($point in $retainedPoints) {
    if (-not [bool]$point.retain -or -not [bool]$point.write.enable) {
        throw "EMS 1.0 compatibility point must be writable and retained: $($point.pointCode)"
    }
    $hasCode = $pointCodes.ContainsKey([string]$point.pointCode)
    $hasIndex = $indexes.ContainsKey([uint32]$point.index)
    if ($hasCode -or $hasIndex) {
        if (-not ($hasCode -and $hasIndex)) {
            throw "partial EMS retention collision: $($point.pointCode)/$($point.index)"
        }
        continue
    }
    $sourcePoints.Add($point)
    $pointCodes[[string]$point.pointCode] = $true
    $indexes[[uint32]$point.index] = $true
    $added++
}

$source.meters[0].points = @($sourcePoints | Sort-Object { [uint32]$_.index })
Write-Json $OutputVirtualConfig $source

$merged = Read-Json $OutputVirtualConfig
$mergedRetained = @(
    $merged.meters[0].points |
        Where-Object { [uint32]$_.index -ge 400 -and [uint32]$_.index -le 447 }
)
if ($mergedRetained.Count -ne 48 -or
    @($mergedRetained | Where-Object { -not [bool]$_.retain -or -not [bool]$_.write.enable }).Count -ne 0) {
    throw "merged EMS virtual config did not preserve all 48 retained points"
}

Write-Host "EMS 1.0 runtime retention merge passed"
Write-Host "  source points: $($sourcePoints.Count - $added)"
Write-Host "  retained points added: $added"
Write-Host "  output points: $($sourcePoints.Count)"
Write-Host "  output: $OutputVirtualConfig"
