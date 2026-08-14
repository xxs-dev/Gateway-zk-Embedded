[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Package,

    [Parameter(Mandatory = $true)]
    [ValidateSet("stationary", "mobile")]
    [string]$Product,

    [string]$ExpectedMachineCode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$packagePath = (Resolve-Path -LiteralPath $Package).Path
$tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempBase ("gateway-ems-package-" + [Guid]::NewGuid().ToString("N"))

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Require-File([string]$Root, [string]$RelativePath) {
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "package is missing required file: $RelativePath"
    }
    return $path
}

try {
    [void](New-Item -ItemType Directory -Path $tempRoot -Force)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $entryNames = @{}
        foreach ($entry in $archive.Entries) {
            $entryName = [string]$entry.FullName
            if ($entryName.Contains('\') -or
                $entryName.StartsWith('/') -or
                $entryName -match '(^|/)\.\.(/|$)' -or
                $entryName -match '^[A-Za-z]:') {
                throw "package contains an unsafe ZIP entry: $entryName"
            }
            if ($entryNames.ContainsKey($entryName)) {
                throw "package contains duplicate ZIP entry: $entryName"
            }
            $entryNames[$entryName] = $true
        }
        [System.IO.Compression.ZipFileExtensions]::ExtractToDirectory($archive, $tempRoot)
    } finally {
        $archive.Dispose()
    }

    $manifestPath = Require-File $tempRoot "manifest.json"
    $nodesPath = Require-File $tempRoot "nodes.json"
    $tagsPath = Require-File $tempRoot "tags.json"
    $runtimeMapPath = Require-File $tempRoot "runtime-map.json"
    $checksumsPath = Require-File $tempRoot "checksums.json"
    [void](Require-File $tempRoot "permissions.json")
    [void](Require-File $tempRoot "topology.json")

    $manifest = Read-Json $manifestPath
    $nodesDocument = Read-Json $nodesPath
    $tagsDocument = Read-Json $tagsPath
    $runtimeMapDocument = Read-Json $runtimeMapPath
    # Windows PowerShell 5.1 emits a root JSON array as one Object[] pipeline item.
    $nodes = @($nodesDocument | ForEach-Object { $_ })
    $tags = @($tagsDocument | ForEach-Object { $_ })
    $runtimeMap = @($runtimeMapDocument | ForEach-Object { $_ })
    $checksums = Read-Json $checksumsPath

    $expectedVersion = if ($Product -eq "stationary") { "1.0" } else { "2.0" }
    $expectedVersionPattern = if ($Product -eq "stationary") { '^1\.0\.' } else { '^2\.0\.' }
    $expectedScreenCount = if ($Product -eq "stationary") { 15 } else { 17 }
    if ([string]$manifest.productVersion -ne $expectedVersion -or
        [string]$manifest.packageVersion -notmatch $expectedVersionPattern) {
        throw "package product/version mismatch: expected $expectedVersion, got $($manifest.productVersion)/$($manifest.packageVersion)"
    }

    $screensDirectory = Join-Path $tempRoot "screens"
    $screens = @(Get-ChildItem -LiteralPath $screensDirectory -Filter "*.json" -File)
    if ($screens.Count -ne $expectedScreenCount) {
        throw "package screen count mismatch: expected $expectedScreenCount, got $($screens.Count)"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $screensDirectory ("$($manifest.entryScreen).json")))) {
        throw "package entry screen does not exist: $($manifest.entryScreen)"
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedMachineCode)) {
        $invalidNodes = @($nodes | Where-Object { [string]$_.machineCode -ne $ExpectedMachineCode })
        if ($nodes.Count -eq 0 -or $invalidNodes.Count -ne 0) {
            throw "package node machineCode does not match: $ExpectedMachineCode"
        }
    }

    $duplicateRoutes = @(
        $runtimeMap |
            Group-Object { "$($_.nodeId)|$($_.sharedMemoryName)|$($_.index)" } |
            Where-Object Count -gt 1
    )
    if ($duplicateRoutes.Count -ne 0) {
        throw "package contains duplicate shared-memory routes: $($duplicateRoutes[0].Name)"
    }

    $duplicateSemantics = @(
        $tags |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.semanticRole) } |
            Group-Object { "$($_.nodeId)|$($_.deviceId)|$($_.semanticRole)" } |
            Where-Object Count -gt 1
    )
    if ($duplicateSemantics.Count -ne 0) {
        throw "package contains duplicate semantic roles: $($duplicateSemantics[0].Name)"
    }

    $tagIds = @{}
    foreach ($tag in $tags) {
        $tagKey = "$($tag.nodeId)|$($tag.tagId)"
        if ($tagIds.ContainsKey($tagKey)) {
            throw "package contains duplicate tag identity: $tagKey"
        }
        $tagIds[$tagKey] = $true
    }
    foreach ($mapping in $runtimeMap) {
        $tagKey = "$($mapping.nodeId)|$($mapping.tagId)"
        if (-not $tagIds.ContainsKey($tagKey)) {
            throw "runtime route references an unknown tag: $tagKey"
        }
    }

    $checksumNames = @{}
    foreach ($property in $checksums.PSObject.Properties) {
        $relativePath = [string]$property.Name
        if ($relativePath.Contains('\') -or
            $relativePath.StartsWith('/') -or
            $relativePath -match '(^|/)\.\.(/|$)' -or
            $relativePath -match '^[A-Za-z]:') {
            throw "checksums.json contains an unsafe path: $relativePath"
        }
        $checksumNames[$relativePath] = $true
        $filePath = Require-File $tempRoot $relativePath
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne [string]$property.Value) {
            throw "package checksum mismatch: $relativePath"
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $tempRoot -Recurse -File |
        Where-Object Name -ne "checksums.json") {
        $relativePath = $file.FullName.Substring($tempRoot.Length + 1).Replace('\', '/')
        if (-not $checksumNames.ContainsKey($relativePath)) {
            throw "package file is not covered by checksums.json: $relativePath"
        }
    }

    if ($Product -eq "stationary") {
        $retainedRoutes = @($runtimeMap | Where-Object {
            [string]$_.sharedMemoryName -eq "gateway_point_store_ems_virtual" -and
            [int]$_.index -ge 400 -and [int]$_.index -le 447 -and [bool]$_.writable
        })
        if ($retainedRoutes.Count -ne 48) {
            throw "EMS 1.0 package must contain 48 retained read/write routes"
        }
    } else {
        $permissions = Read-Json (Join-Path $tempRoot "permissions.json")
        if (@($permissions.localAccess.protectedScreenPrefixes) -join ',' -ne 'Strategy-,Control-') {
            throw "EMS 2.0 package must protect Strategy-* and Control-* screens"
        }
        $screenText = ($screens | ForEach-Object {
            Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8
        }) -join "`n"
        if ($screenText -match '(?i)\bUPS\b|不间断电源') {
            throw "EMS 2.0 package still contains UPS content"
        }
    }

    $packageHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "EMS package validation passed"
    Write-Host "  product: $Product $expectedVersion"
    Write-Host "  machineCode: $ExpectedMachineCode"
    Write-Host "  version: $($manifest.packageVersion)"
    Write-Host "  screens: $($screens.Count)"
    Write-Host "  tags/routes: $($tags.Count)/$($runtimeMap.Count)"
    Write-Host "  sha256: $packageHash"
} finally {
    $resolvedTempRoot = [System.IO.Path]::GetFullPath($tempRoot)
    if ((Test-Path -LiteralPath $resolvedTempRoot) -and
        $resolvedTempRoot.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTempRoot) -like 'gateway-ems-package-*') {
        Remove-Item -LiteralPath $resolvedTempRoot -Recurse -Force
    }
}
