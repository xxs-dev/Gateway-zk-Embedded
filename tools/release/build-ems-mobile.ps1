[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COMM[0-9]+$')]
    [string]$MachineCode,
    [Parameter(Mandatory = $true)]
    [Security.SecureString]$LocalOperatorPassword,
    [string]$SourceProjectDirectory,
    [string]$RuntimeConfigDirectory,
    [string]$OutputDirectory,
    [string]$PackageVersion,
    [string]$DisplayName = "Kaiyuan Mobile Energy Storage Vehicle",
    [string]$NodeId = "edge-1",
    [string]$LocalOperatorUsername = "operator",
    [ValidateRange(60, 86400)]
    [int]$LocalSessionTimeoutSeconds = 900
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\..\products\ems\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$productRoot = Join-Path $repoRoot "products\ems\mobile\2.0"
$product = Get-Content -LiteralPath (Join-Path $productRoot "product.json") -Raw -Encoding UTF8 | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($SourceProjectDirectory)) {
    $SourceProjectDirectory = Join-Path $productRoot "scada\base-project"
}
if ([string]::IsNullOrWhiteSpace($RuntimeConfigDirectory)) {
    $RuntimeConfigDirectory = Join-Path $repoRoot "config\factory\runtime"
}
if ([string]::IsNullOrWhiteSpace($PackageVersion)) {
    $PackageVersion = [string]$product.currentPackageVersion
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "generated\artifacts\releases\ems-mobile\$MachineCode\$PackageVersion"
}

$outputProject = Join-Path $OutputDirectory "scada-project"
$outputPackage = Join-Path $OutputDirectory "ky-mobile-ems-$MachineCode-$PackageVersion.kyscada"
$implementation = Join-Path $productRoot "tools\generate-scada.ps1"
& $implementation `
    -SourceProjectDirectory $SourceProjectDirectory `
    -RuntimeConfigDirectory $RuntimeConfigDirectory `
    -OutputProjectDirectory $outputProject `
    -OutputPackage $outputPackage `
    -MachineCode $MachineCode `
    -DisplayName $DisplayName `
    -PackageVersion $PackageVersion `
    -NodeId $NodeId `
    -LocalOperatorUsername $LocalOperatorUsername `
    -LocalOperatorPassword $LocalOperatorPassword `
    -LocalSessionTimeoutSeconds $LocalSessionTimeoutSeconds

& (Join-Path $PSScriptRoot "validate-ems-package.ps1") `
    -Package $outputPackage `
    -Product mobile `
    -ExpectedMachineCode $MachineCode

Write-Host "EMS 2.0 release ready: $outputPackage"
