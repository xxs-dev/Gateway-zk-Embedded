[CmdletBinding()]
param(
    [string]$QtVersion = "6.8.3",
    [string]$QtRoot,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [switch]$InstallQt,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $repoRoot ".tools\qt-windows"
if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    $QtRoot = Join-Path $toolRoot "$QtVersion\mingw_64"
}
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot "build-scada-qt-windows"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $BuildDirectory "publish"
}

$qtBin = Join-Path $QtRoot "bin"
$qtCmake = Join-Path $QtRoot "lib\cmake\Qt6"
$mingwRoot = Join-Path $toolRoot "Tools\mingw1310_64"
$mingwBin = Join-Path $mingwRoot "bin"

if ($InstallQt -and (!(Test-Path (Join-Path $qtBin "windeployqt.exe")) -or !(Test-Path (Join-Path $mingwBin "mingw32-make.exe")))) {
    $python = (Get-Command python -ErrorAction Stop).Source
    & $python -m pip install --user --disable-pip-version-check aqtinstall cmake
    & $python -m aqt install-qt windows desktop $QtVersion win64_mingw -O $toolRoot
    & $python -m aqt install-tool windows desktop tools_mingw1310 -O $toolRoot
}

$windeployqt = Join-Path $qtBin "windeployqt.exe"
$make = Join-Path $mingwBin "mingw32-make.exe"
$compiler = Join-Path $mingwBin "g++.exe"
foreach ($required in @($windeployqt, $make, $compiler, $qtCmake)) {
    if (!(Test-Path $required)) {
        throw "Windows Qt toolchain is incomplete: $required. Run this script with -InstallQt once."
    }
}

if ($Clean -and (Test-Path $BuildDirectory)) {
    Remove-Item -LiteralPath $BuildDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDirectory, $OutputDirectory | Out-Null

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmakeCommand) {
    $python = (Get-Command python -ErrorAction Stop).Source
    $scriptsDirectory = (& $python -c "import sysconfig; print(sysconfig.get_path('scripts', scheme='nt_user'))").Trim()
    $cmakeCandidate = Join-Path $scriptsDirectory "cmake.exe"
    if (!(Test-Path $cmakeCandidate)) {
        throw "CMake is not installed. Run this script with -InstallQt once."
    }
    $cmake = $cmakeCandidate
} else {
    $cmake = $cmakeCommand.Source
}
$sourceDirectory = Join-Path $repoRoot "tools\scada-windows-renderer"
$env:Path = "$mingwBin;$qtBin;$env:Path"
& $cmake -S $sourceDirectory -B $BuildDirectory -G "MinGW Makefiles" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_PREFIX_PATH=$QtRoot" `
    "-DCMAKE_C_COMPILER=$(Join-Path $mingwBin 'gcc.exe')" `
    "-DCMAKE_CXX_COMPILER=$compiler"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

& $cmake --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "SCADA Windows renderer build failed with exit code $LASTEXITCODE." }

$executable = Join-Path $BuildDirectory "KY-SCADA-Windows.exe"
if (!(Test-Path $executable)) { throw "Renderer output was not produced: $executable" }
Copy-Item -LiteralPath $executable -Destination $OutputDirectory -Force
& $windeployqt --release --no-translations --no-system-d3d-compiler --dir $OutputDirectory (Join-Path $OutputDirectory "KY-SCADA-Windows.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }

Get-FileHash (Join-Path $OutputDirectory "KY-SCADA-Windows.exe") -Algorithm SHA256
Get-ChildItem -LiteralPath $OutputDirectory | Sort-Object Name | Select-Object Name, Length
