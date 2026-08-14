[CmdletBinding()]
param()

$implementation = Join-Path $PSScriptRoot "..\products\ems\stationary\1.0\tests\upgrade-retention.test.ps1"
& $implementation
