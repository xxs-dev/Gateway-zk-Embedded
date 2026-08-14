[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COMM[0-9]+$')]
    [string]$MachineCode,
    [string]$SourceProjectDirectory,
    [string]$OutputDirectory,
    [string]$PackageVersion,
    [string]$DisplayName = "KY Stationary Storage Cabinet",
    [string]$NodeId = "edge-1",
    [string]$RuntimeDeviceDirectory,
    [string]$ProjectOverlay
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\products\ems\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$productRoot = Join-Path $repoRoot "products\ems\stationary\1.0"
$product = Get-Content -LiteralPath (Join-Path $productRoot "product.json") -Raw -Encoding UTF8 | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($SourceProjectDirectory)) {
    $SourceProjectDirectory = Join-Path $productRoot "scada\base-project"
}
if ([string]::IsNullOrWhiteSpace($PackageVersion)) {
    $PackageVersion = [string]$product.currentPackageVersion
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "generated\artifacts\releases\ems-stationary\$MachineCode\$PackageVersion"
}

$outputProject = Join-Path $OutputDirectory "scada-project"
$outputPackage = Join-Path $OutputDirectory "ky-ems-$MachineCode-$PackageVersion.kyscada"
$implementation = Join-Path $productRoot "tools\upgrade-retention.ps1"
$implementationArguments = @{
    SourceProjectDirectory = $SourceProjectDirectory
    VirtualConfig = Join-Path $repoRoot "config\factory\runtime\devices\device_ems_virtual.json"
    OutputProjectDirectory = $outputProject
    OutputPackage = $outputPackage
    MachineCode = $MachineCode
    PackageVersion = $PackageVersion
    NodeId = $NodeId
    DisplayName = $DisplayName
}
if (-not [string]::IsNullOrWhiteSpace($RuntimeDeviceDirectory)) {
    $implementationArguments.RuntimeDeviceDirectory = $RuntimeDeviceDirectory
}
if (-not [string]::IsNullOrWhiteSpace($ProjectOverlay)) {
    $implementationArguments.ProjectOverlay = $ProjectOverlay
}
& $implementation @implementationArguments

& (Join-Path $PSScriptRoot "validate-ems-package.ps1") `
    -Package $outputPackage `
    -Product stationary `
    -ExpectedMachineCode $MachineCode

Write-Host "EMS 1.0 release ready: $outputPackage"
