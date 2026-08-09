[CmdletBinding()]
param(
    [string]$SourceProjectDirectory,
    [string]$RuntimeConfigDirectory,
    [string]$OutputProjectDirectory,
    [string]$OutputPackage,
    [string]$MachineCode = "COMM202600999",
    [string]$DisplayName = "凯源移动储能车",
    [string]$PackageVersion = "2.0.9-compact",
    [string]$NodeId = "edge-1",
    [string]$LocalOperatorUsername = "operator",
    [Security.SecureString]$LocalOperatorPassword,
    [ValidateRange(60, 86400)]
    [int]$LocalSessionTimeoutSeconds = 900
)

$implementation = Join-Path $PSScriptRoot "..\products\ems\mobile\2.0\tools\generate-scada.ps1"
& $implementation @PSBoundParameters
