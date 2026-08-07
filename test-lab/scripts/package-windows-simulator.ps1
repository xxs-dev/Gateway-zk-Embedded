param(
    [string]$BuildDirectory = "$PSScriptRoot\..\..\build-test-lab-windows",
    [string]$OutputDirectory = "$PSScriptRoot\..\dist-windows",
    [string]$MinGwBinDirectory = 'C:\Users\12193\AppData\Local\Programs\CLion\bin\mingw\bin'
)

$ErrorActionPreference = 'Stop'
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$MinGwBinDirectory = [IO.Path]::GetFullPath($MinGwBinDirectory)
$executable = Join-Path $BuildDirectory 'gateway-test-lab-sim.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Simulator executable not found: $executable"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $executable -Destination $OutputDirectory -Force
Copy-Item -LiteralPath "$PSScriptRoot\Gateway-TestLabSimulator.ps1" -Destination $OutputDirectory -Force
Copy-Item -LiteralPath "$PSScriptRoot\Show-GatewayTestLabResults.ps1" -Destination $OutputDirectory -Force

$runtimeLibraries = @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')
foreach ($library in $runtimeLibraries) {
    $source = Join-Path $MinGwBinDirectory $library
    if (-not (Test-Path -LiteralPath $source)) {
        throw "MinGW runtime library not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination $OutputDirectory -Force
}

$checksumFile = Join-Path $OutputDirectory 'SHA256SUMS.txt'
$hashes = Get-ChildItem -LiteralPath $OutputDirectory -File |
    Where-Object { $_.FullName -ne $checksumFile } |
    Sort-Object Name |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_.Path))" }
Set-Content -LiteralPath $checksumFile -Value $hashes -Encoding ascii
Write-Output "Windows simulator package: $OutputDirectory"
