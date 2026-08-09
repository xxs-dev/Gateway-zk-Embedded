[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceProjectDirectory,
    [string]$VirtualConfig = "config/factory/runtime/devices/device_ems_virtual.json",
    [string]$OutputProjectDirectory,
    [string]$OutputPackage,
    [string]$MachineCode = "COMM202600999",
    [string]$PackageVersion = "1.0.1",
    [string]$NodeId = "edge-1",
    [string]$DisplayName = "KY Stationary Storage Cabinet"
)

$implementation = Join-Path $PSScriptRoot "..\products\ems\stationary\1.0\tools\upgrade-retention.ps1"
& $implementation @PSBoundParameters
