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
    [string]$DisplayName = "KY Stationary Storage Cabinet",
    [string]$RuntimeDeviceDirectory,
    [string]$ProjectOverlay
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "..\..\..\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
if ($PackageVersion -notmatch '^1\.0\.[0-9]+(?:[-._][A-Za-z0-9._-]+)?$') {
    throw "EMS 1.0 packageVersion must stay in the 1.0.x product line: $PackageVersion"
}
if ([string]::IsNullOrWhiteSpace($OutputProjectDirectory)) {
    $OutputProjectDirectory = Join-Path $repoRoot "generated\artifacts\releases\ky-ems-1.0-retention\scada-project"
}
if ([string]::IsNullOrWhiteSpace($OutputPackage)) {
    $OutputPackage = Join-Path $repoRoot "generated\artifacts\releases\ky-ems-1.0-retention\ky-ems-$MachineCode-$PackageVersion.kyscada"
}

function Read-Json([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-Json([string]$Path, [object]$Value) {
    $json = ConvertTo-Json -InputObject $Value -Depth 100
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Set-Property([object]$Object, [string]$Name, [object]$Value) {
    if ($null -ne $Object.PSObject.Properties[$Name]) {
        $Object.PSObject.Properties[$Name].Value = $Value
    } else {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function Get-RelativePathCompat([string]$Root, [string]$Path) {
    $relativePathMethod = [System.IO.Path].GetMethod(
        "GetRelativePath",
        [type[]]@([string], [string]))
    if ($null -ne $relativePathMethod) {
        return [System.IO.Path]::GetRelativePath($Root, $Path)
    }

    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $pathValue = [System.IO.Path]::GetFullPath($Path)
    $rootUri = [System.Uri]::new($rootPath)
    $pathUri = [System.Uri]::new($pathValue)
    return [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString()).Replace(
        '/',
        [System.IO.Path]::DirectorySeparatorChar)
}

function File-HashTable([string]$Root, [string[]]$Subdirectories) {
    $result = @{}
    foreach ($subdirectory in $Subdirectories) {
        $path = Join-Path $Root $subdirectory
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            continue
        }
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File | Sort-Object FullName) {
            $relative = (Get-RelativePathCompat $Root $file.FullName).Replace('\', '/')
            $result[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $result
}

function Assert-HashTablesEqual([hashtable]$Expected, [hashtable]$Actual) {
    if ($Expected.Count -ne $Actual.Count) {
        throw "EMS 1.0 visual asset count changed during retention upgrade"
    }
    foreach ($entry in $Expected.GetEnumerator()) {
        if (-not $Actual.ContainsKey($entry.Key) -or $Actual[$entry.Key] -ne $entry.Value) {
            throw "EMS 1.0 visual asset changed during retention upgrade: $($entry.Key)"
        }
    }
}

function Update-TagReferences([object]$Value, [hashtable]$Replacements, [ref]$ChangeCount) {
    if ($null -eq $Value -or $Value -is [string]) {
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [System.Management.Automation.PSCustomObject]) {
        foreach ($item in $Value) {
            Update-TagReferences $item $Replacements $ChangeCount
        }
        return
    }
    if ($Value -isnot [System.Management.Automation.PSCustomObject]) {
        return
    }

    foreach ($property in @($Value.PSObject.Properties)) {
        if ($property.Name -eq "tagId" -and
            $property.Value -is [string] -and
            $Replacements.ContainsKey([string]$property.Value)) {
            $property.Value = $Replacements[[string]$property.Value]
            $ChangeCount.Value++
            continue
        }
        Update-TagReferences $property.Value $Replacements $ChangeCount
    }
}

function Collect-TagReferences([object]$Value, [System.Collections.Generic.HashSet[string]]$References) {
    if ($null -eq $Value) {
        return
    }
    if ($Value -is [string]) {
        if ($Value -match '^\s*[\[{]') {
            try {
                Collect-TagReferences ($Value | ConvertFrom-Json) $References
            } catch {
                # UI properties may contain non-JSON text beginning with '[' or '{'.
            }
        }
        return
    }
    if ($Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [System.Management.Automation.PSCustomObject]) {
        foreach ($item in $Value) {
            Collect-TagReferences $item $References
        }
        return
    }
    if ($Value -isnot [System.Management.Automation.PSCustomObject]) {
        return
    }
    foreach ($property in @($Value.PSObject.Properties)) {
        if ($property.Name -eq "tagId" -and
            $property.Value -is [string] -and
            -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            [void]$References.Add([string]$property.Value)
        }
        Collect-TagReferences $property.Value $References
    }
}

function Get-ProjectTagReferences([string]$Root) {
    $references = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "*.json" |
        Where-Object Name -notin @("tags.json", "runtime-map.json", "checksums.json")) {
        Collect-TagReferences (Read-Json $file.FullName) $references
    }
    return $references
}

$sourceRoot = (Resolve-Path -LiteralPath $SourceProjectDirectory).Path
$virtualConfigPath = (Resolve-Path -LiteralPath $VirtualConfig).Path
$outputRoot = [System.IO.Path]::GetFullPath($OutputProjectDirectory)
if ($sourceRoot -eq $outputRoot) {
    throw "source and output EMS 1.0 project directories must be different"
}
if ($outputRoot -eq [System.IO.Path]::GetPathRoot($outputRoot) -or $outputRoot.Length -lt 12) {
    throw "unsafe EMS 1.0 output directory: $outputRoot"
}

foreach ($required in @("manifest.json", "nodes.json", "tags.json", "runtime-map.json", "screens")) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $required))) {
        throw "EMS 1.0 source project is missing $required"
    }
}

$sourceScreenHashes = File-HashTable $sourceRoot @("screens")
$sourceAssetHashes = File-HashTable $sourceRoot @("assets")
if (Test-Path -LiteralPath $outputRoot) {
    Remove-Item -LiteralPath $outputRoot -Recurse -Force
}
[void](New-Item -ItemType Directory -Path $outputRoot -Force)
foreach ($item in Get-ChildItem -LiteralPath $sourceRoot -Force) {
    Copy-Item -LiteralPath $item.FullName -Destination $outputRoot -Recurse -Force
}

$overlayRemoveTagIds = @{}
$pruneUnreferencedInvalidRoutes = $false
$visualOverlayChanges = 0
if (-not [string]::IsNullOrWhiteSpace($ProjectOverlay)) {
    $overlayPath = (Resolve-Path -LiteralPath $ProjectOverlay).Path
    $overlay = Read-Json $overlayPath
    $clearBindingTagIds = @{}
    foreach ($tagId in @($overlay.clearBindingTagIds)) {
        $clearBindingTagIds[[string]$tagId] = $true
    }
    foreach ($tagId in @($overlay.removeTagIds)) {
        $overlayRemoveTagIds[[string]$tagId] = $true
    }
    $pruneUnreferencedInvalidRoutes = [bool]$overlay.pruneUnreferencedInvalidRoutes

    foreach ($screenOverlay in @($overlay.removeWidgets)) {
        $screenPath = Join-Path $outputRoot "screens\$($screenOverlay.screen).json"
        if (-not (Test-Path -LiteralPath $screenPath -PathType Leaf)) {
            throw "EMS 1.0 overlay screen does not exist: $($screenOverlay.screen)"
        }
        $screen = Read-Json $screenPath
        $removeWidgetIds = @{}
        foreach ($widgetId in @($screenOverlay.widgetIds)) {
            $removeWidgetIds[[string]$widgetId] = $true
        }
        $before = @($screen.widgets).Count
        $screen.widgets = @($screen.widgets | Where-Object {
            -not $removeWidgetIds.ContainsKey([string]$_.widgetId)
        })
        $removed = $before - @($screen.widgets).Count
        if ($removed -ne $removeWidgetIds.Count) {
            throw "EMS 1.0 overlay widget mismatch on $($screenOverlay.screen): expected $($removeWidgetIds.Count), removed $removed"
        }
        Write-Json $screenPath $screen
        $visualOverlayChanges += $removed
    }

    $clearedTagIds = @{}
    foreach ($screenFile in Get-ChildItem -LiteralPath (Join-Path $outputRoot "screens") -File -Filter "*.json") {
        $screen = Read-Json $screenFile.FullName
        $changed = $false
        foreach ($widget in @($screen.widgets)) {
            $invalidBindings = @($widget.bindings | Where-Object {
                $clearBindingTagIds.ContainsKey([string]$_.tagId)
            })
            if ($invalidBindings.Count -eq 0) {
                continue
            }
            if ($invalidBindings.Count -ne @($widget.bindings).Count) {
                throw "EMS 1.0 overlay cannot partially clear widget bindings: $($widget.widgetId)"
            }
            foreach ($binding in $invalidBindings) {
                $clearedTagIds[[string]$binding.tagId] = $true
            }
            $widget.bindings = @()
            $widget.stateRules = @()
            Set-Property $widget "title" "--"
            if ($null -ne $widget.properties) {
                Set-Property $widget.properties "qtText" "--"
                Set-Property $widget.properties "defaultStateLabel" "--"
            }
            $visualOverlayChanges++
            $changed = $true
        }
        if ($changed) {
            Write-Json $screenFile.FullName $screen
        }
    }
    foreach ($tagId in $clearBindingTagIds.Keys) {
        if (-not $clearedTagIds.ContainsKey($tagId)) {
            throw "EMS 1.0 overlay binding tag was not found: $tagId"
        }
    }
}

$virtualDocument = Read-Json $virtualConfigPath
$virtualPoints = @{}
foreach ($meter in @($virtualDocument.meters)) {
    foreach ($point in @($meter.points)) {
        $virtualPoints[[string]$point.pointCode] = $point
    }
}
$sharedMemoryName = [string]$virtualDocument.memoryStore.sharedMemoryName
if ([string]::IsNullOrWhiteSpace($sharedMemoryName)) {
    throw "EMS virtual config does not define memoryStore.sharedMemoryName"
}

$tagsPath = Join-Path $outputRoot "tags.json"
$runtimePath = Join-Path $outputRoot "runtime-map.json"
$tagsDocument = Read-Json $tagsPath
$runtimeDocument = Read-Json $runtimePath
$tags = @($tagsDocument)
$runtimeMappings = @($runtimeDocument)
$tagById = @{}
$runtimeByTagId = @{}
foreach ($tag in $tags) { $tagById[[string]$tag.tagId] = $tag }
foreach ($mapping in $runtimeMappings) { $runtimeByTagId[[string]$mapping.tagId] = $mapping }

$requiredLegacyTagIds = [System.Collections.Generic.List[string]]::new()
for ($hour = 0; $hour -lt 24; $hour++) {
    foreach ($kind in @("power", "soc")) {
        $requiredLegacyTagIds.Add("device.ems.schedule.hour$hour.$kind.current")
        $requiredLegacyTagIds.Add("device.ems.schedule.hour$hour.$kind.set")
    }
}
$legacyTagCount = @($requiredLegacyTagIds | Where-Object { $tagById.ContainsKey($_) }).Count
$mergedCurrentAliases = @{}
$convertedRoutes = 0
if ($legacyTagCount -eq 96) {
    for ($hour = 0; $hour -lt 24; $hour++) {
        foreach ($kind in @("power", "soc")) {
            $pointCode = "ems_schedule_${kind}_$hour"
            $point = $virtualPoints[$pointCode]
            if ($null -eq $point -or -not [bool]$point.retain -or -not [bool]$point.write.enable) {
                throw "EMS virtual compatibility point is missing or not retained: $pointCode"
            }
            $currentTagId = "device.ems.schedule.hour$hour.$kind.current"
            $setTagId = "device.ems.schedule.hour$hour.$kind.set"
            foreach ($legacyTagId in @($currentTagId, $setTagId)) {
                if (-not $runtimeByTagId.ContainsKey($legacyTagId)) {
                    throw "EMS 1.0 source runtime mapping is missing: $legacyTagId"
                }
            }

            $tag = $tagById[$setTagId]
            Set-Property $tag "nodeId" $NodeId
            Set-Property $tag "deviceId" "EMS_CORE"
            Set-Property $tag "meterCode" "EMS_CORE"
            Set-Property $tag "pointCode" $pointCode
            Set-Property $tag "semanticRole" "ems.schedule.$kind.$hour"
            Set-Property $tag "unit" ([string]$point.read.unit)
            Set-Property $tag "dataType" "float64"
            Set-Property $tag "access" "readWrite"
            Set-Property $tag "indexFallback" ([uint32]$point.index)

            $mapping = $runtimeByTagId[$setTagId]
            Set-Property $mapping "nodeId" $NodeId
            Set-Property $mapping "sharedMemoryName" $sharedMemoryName
            Set-Property $mapping "index" ([uint32]$point.index)
            Set-Property $mapping "writable" $true
            Set-Property $mapping "dataType" "float64"
            Set-Property $mapping "unit" ([string]$point.read.unit)

            $mergedCurrentAliases[$currentTagId] = $setTagId
            $convertedRoutes++
        }
    }
} elseif ($legacyTagCount -eq 0) {
    # Some field projects already dropped the old 96 schedule tags. Add the
    # canonical retained routes without changing their current visual layout.
    foreach ($tag in $tags) {
        if ([string]$tag.semanticRole -match '^ems\.schedule\.hour[0-9]+\.(power|soc)\.(current|set)$') {
            Set-Property $tag "semanticRole" "local.$($tag.pointCode)"
        }
    }
    for ($hour = 0; $hour -lt 24; $hour++) {
        foreach ($kind in @("power", "soc")) {
            $pointCode = "ems_schedule_${kind}_$hour"
            $point = $virtualPoints[$pointCode]
            if ($null -eq $point -or -not [bool]$point.retain -or -not [bool]$point.write.enable) {
                throw "EMS virtual compatibility point is missing or not retained: $pointCode"
            }
            $tagId = "device.ems.schedule.hour$hour.$kind.set"
            $tags += [pscustomobject]@{
                tagId = $tagId
                nodeId = $NodeId
                deviceId = "EMS_CORE"
                meterCode = "EMS_CORE"
                pointCode = $pointCode
                semanticRole = "ems.schedule.$kind.$hour"
                displayName = [string]$point.name
                unit = [string]$point.read.unit
                dataType = "float64"
                access = "readWrite"
                indexFallback = [uint32]$point.index
            }
            $runtimeMappings += [pscustomobject]@{
                nodeId = $NodeId
                tagId = $tagId
                sharedMemoryName = $sharedMemoryName
                index = [uint32]$point.index
                writable = $true
                dataType = "float64"
                unit = [string]$point.read.unit
            }
            $convertedRoutes++
        }
    }
} else {
    throw "EMS 1.0 source contains a partial legacy schedule model: $legacyTagCount/96 tags"
}
if ($convertedRoutes -ne 48) {
    throw "EMS 1.0 retention conversion count mismatch: routes=$convertedRoutes"
}

$tags = @($tags | Where-Object { -not $mergedCurrentAliases.ContainsKey([string]$_.tagId) })
$runtimeMappings = @($runtimeMappings | Where-Object { -not $mergedCurrentAliases.ContainsKey([string]$_.tagId) })

$updatedTagReferences = 0
$referenceFiles = [System.Collections.Generic.List[string]]::new()
foreach ($name in @("alarms.json", "symbols.json", "topology.json", "trends.json")) {
    $path = Join-Path $outputRoot $name
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $referenceFiles.Add($path)
    }
}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $outputRoot "screens") -Recurse -File -Filter "*.json") {
    $referenceFiles.Add($file.FullName)
}
foreach ($path in $referenceFiles) {
    $document = Read-Json $path
    $fileChanges = 0
    Update-TagReferences $document $mergedCurrentAliases ([ref]$fileChanges)
    if ($fileChanges -gt 0) {
        Write-Json $path $document
        $updatedTagReferences += $fileChanges
    }
}

$projectReferences = Get-ProjectTagReferences $outputRoot
foreach ($tagId in $overlayRemoveTagIds.Keys) {
    if ($projectReferences.Contains($tagId)) {
        throw "EMS 1.0 overlay cannot remove a referenced tag: $tagId"
    }
    $tags = @($tags | Where-Object { [string]$_.tagId -ne $tagId })
    $runtimeMappings = @($runtimeMappings | Where-Object { [string]$_.tagId -ne $tagId })
}

if (-not [string]::IsNullOrWhiteSpace($RuntimeDeviceDirectory)) {
    $runtimeRoot = (Resolve-Path -LiteralPath $RuntimeDeviceDirectory).Path
    $actualRoutes = @{}
    foreach ($configFile in Get-ChildItem -LiteralPath $runtimeRoot -File -Filter "*.json") {
        $config = Read-Json $configFile.FullName
        $memoryName = [string]$config.memoryStore.sharedMemoryName
        if ([string]::IsNullOrWhiteSpace($memoryName)) {
            continue
        }
        foreach ($meter in @($config.meters)) {
            foreach ($point in @($meter.points)) {
                $actualRoutes["$memoryName`u{001f}$([uint32]$point.index)"] = $true
            }
        }
    }
    foreach ($point in $virtualPoints.Values) {
        $actualRoutes["$sharedMemoryName`u{001f}$([uint32]$point.index)"] = $true
    }

    $tagIds = @{}
    foreach ($tag in $tags) {
        $tagIds[[string]$tag.tagId] = $true
    }
    $runtimeByTagId = @{}
    foreach ($mapping in $runtimeMappings) {
        $runtimeByTagId[[string]$mapping.tagId] = $mapping
    }
    foreach ($tagId in $projectReferences) {
        if (-not $tagIds.ContainsKey($tagId)) {
            throw "EMS 1.0 project references an unknown tag: $tagId"
        }
        if (-not $runtimeByTagId.ContainsKey($tagId)) {
            throw "EMS 1.0 project references a tag without runtime route: $tagId"
        }
        $mapping = $runtimeByTagId[$tagId]
        $routeKey = "$($mapping.sharedMemoryName)`u{001f}$([uint32]$mapping.index)"
        if (-not $actualRoutes.ContainsKey($routeKey)) {
            throw "EMS 1.0 project references a route absent from runtime config: $tagId"
        }
    }

    $invalidMappings = @($runtimeMappings | Where-Object {
        -not $actualRoutes.ContainsKey("$($_.sharedMemoryName)`u{001f}$([uint32]$_.index)")
    })
    if ($invalidMappings.Count -ne 0 -and -not $pruneUnreferencedInvalidRoutes) {
        throw "EMS 1.0 project contains routes absent from runtime config: $($invalidMappings[0].tagId)"
    }
    if ($invalidMappings.Count -ne 0) {
        $invalidTagIds = @{}
        foreach ($mapping in $invalidMappings) {
            if ($projectReferences.Contains([string]$mapping.tagId)) {
                throw "EMS 1.0 cannot prune a referenced invalid route: $($mapping.tagId)"
            }
            $invalidTagIds[[string]$mapping.tagId] = $true
        }
        $runtimeMappings = @($runtimeMappings | Where-Object {
            -not $invalidTagIds.ContainsKey([string]$_.tagId)
        })
        $tags = @($tags | Where-Object {
            -not $invalidTagIds.ContainsKey([string]$_.tagId)
        })
        Write-Host "  pruned invalid unreferenced routes: $($invalidMappings.Count)"
    }
}

$semanticKeys = @{}
foreach ($tag in $tags) {
    if ([string]::IsNullOrWhiteSpace([string]$tag.semanticRole)) {
        continue
    }
    $semanticKey = "$($tag.nodeId)`u{001f}$($tag.deviceId)`u{001f}$($tag.semanticRole)"
    if ($semanticKeys.ContainsKey($semanticKey)) {
        throw "EMS 1.0 semantic role is ambiguous: $($tag.semanticRole)"
    }
    $semanticKeys[$semanticKey] = [string]$tag.tagId
}

$manifestPath = Join-Path $outputRoot "manifest.json"
$manifest = Read-Json $manifestPath
Set-Property $manifest "projectId" "ky-ems-$MachineCode"
Set-Property $manifest "projectName" "$DisplayName EMS 1.0 Round UI - $MachineCode"
Set-Property $manifest "packageVersion" $PackageVersion
Set-Property $manifest "createdAt" ([DateTimeOffset]::UtcNow.ToString("o"))
Set-Property $manifest "createdBy" "Gateway EMS 1.0 stationary storage retention compatibility tool"
Set-Property $manifest "productVersion" "1.0"

$nodesPath = Join-Path $outputRoot "nodes.json"
$nodesDocument = Read-Json $nodesPath
$nodes = @($nodesDocument)
foreach ($node in $nodes) {
    Set-Property $node "nodeId" $NodeId
    Set-Property $node "machineCode" $MachineCode
    Set-Property $node "displayName" $MachineCode
}

Write-Json $manifestPath $manifest
Write-Json $nodesPath $nodes
Write-Json $tagsPath $tags
Write-Json $runtimePath $runtimeMappings
Assert-HashTablesEqual $sourceAssetHashes (File-HashTable $outputRoot @("assets"))
$outputScreenHashes = File-HashTable $outputRoot @("screens")
if ($updatedTagReferences -eq 0 -and $visualOverlayChanges -eq 0) {
    Assert-HashTablesEqual $sourceScreenHashes $outputScreenHashes
} else {
    if ($sourceScreenHashes.Count -ne $outputScreenHashes.Count) {
        throw "EMS 1.0 screen count changed during tag merge"
    }
    foreach ($entry in $sourceScreenHashes.GetEnumerator()) {
        if (-not $outputScreenHashes.ContainsKey($entry.Key)) {
            throw "EMS 1.0 screen was removed during tag merge: $($entry.Key)"
        }
    }
}

$checksumTable = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $outputRoot -Recurse -File |
    Where-Object Name -ne "checksums.json" |
    Sort-Object FullName) {
    $relative = (Get-RelativePathCompat $outputRoot $file.FullName).Replace('\', '/')
    $checksumTable[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
Write-Json (Join-Path $outputRoot "checksums.json") ([pscustomobject]$checksumTable)

$packageDirectory = Split-Path -Parent $OutputPackage
[void](New-Item -ItemType Directory -Path $packageDirectory -Force)
if (Test-Path -LiteralPath $OutputPackage) {
    Remove-Item -LiteralPath $OutputPackage -Force
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$packageStream = [System.IO.File]::Open(
    $OutputPackage,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::None)
$archive = $null
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $packageStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    foreach ($file in Get-ChildItem -LiteralPath $outputRoot -Recurse -File | Sort-Object FullName) {
        # ZIP entry names always use '/', including when packaged by Windows PowerShell 5.1.
        $entryName = (Get-RelativePathCompat $outputRoot $file.FullName).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.FullName,
            $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    if ($null -ne $archive) {
        $archive.Dispose()
    }
    $packageStream.Dispose()
}

$packageHash = (Get-FileHash -LiteralPath $OutputPackage -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "EMS 1.0 retention-compatible SCADA project generated"
Write-Host "  machineCode: $MachineCode"
Write-Host "  version: $PackageVersion (product 1.0)"
Write-Host "  retained read/write routes: $convertedRoutes"
Write-Host "  merged legacy current-value references: $updatedTagReferences"
Write-Host "  project overlay changes: $visualOverlayChanges"
Write-Host "  visual files preserved: $($sourceScreenHashes.Count + $sourceAssetHashes.Count)"
Write-Host "  project: $outputRoot"
Write-Host "  package: $OutputPackage"
Write-Host "  package sha256: $packageHash"
