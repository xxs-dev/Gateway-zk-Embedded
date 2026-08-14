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
