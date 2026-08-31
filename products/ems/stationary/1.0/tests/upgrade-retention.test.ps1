$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\..\..\common\powershell\Repository.ps1")
$repoRoot = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("ems1-retention-test-" + [Guid]::NewGuid().ToString("N"))
$sourceRoot = Join-Path $fixtureRoot "source"
$outputRoot = Join-Path $fixtureRoot "output"
$package = Join-Path $fixtureRoot "ky-ems-test.kyscada"

function Write-Json([string]$Path, [object]$Value) {
    $parent = Split-Path -Parent $Path
    [void](New-Item -ItemType Directory -Path $parent -Force)
    $json = ConvertTo-Json -InputObject $Value -Depth 100
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

try {
    [void](New-Item -ItemType Directory -Path (Join-Path $sourceRoot "screens") -Force)
    [void](New-Item -ItemType Directory -Path (Join-Path $sourceRoot "assets") -Force)
    [System.IO.File]::WriteAllText(
        (Join-Path $sourceRoot "screens\FirstPage.json"),
        '{"screenId":"FirstPage","width":1920,"height":1080,"widgets":[{"widgetId":"schedule","type":"qtValue","title":"Schedule","geometry":{"x":10,"y":20,"width":200,"height":80},"bindings":[{"nodeId":"edge-1","tagId":"device.ems.schedule.hour0.power.current","slot":"value"}],"properties":{"qtText":"unchanged"}}]}',
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $sourceRoot "assets\round.png"), 'round-layout', [System.Text.UTF8Encoding]::new($false))

    Write-Json (Join-Path $sourceRoot "manifest.json") ([pscustomobject]@{
        schemaVersion = "2.0"; projectId = "ky-ems-COMM202600999"; projectName = "EMS 1.0"
        packageVersion = "1.0.0"; entryScreen = "FirstPage"; packageRole = "project"
    })
    Write-Json (Join-Path $sourceRoot "nodes.json") @([pscustomobject]@{
        nodeId = "edge-1"; machineCode = "COMM202600999"; displayName = "COMM202600999"; roles = @("control")
    })

    $tags = [System.Collections.Generic.List[object]]::new()
    $mappings = [System.Collections.Generic.List[object]]::new()
    for ($hour = 0; $hour -lt 24; $hour++) {
        foreach ($kind in @("power", "soc")) {
            foreach ($role in @("current", "set")) {
                $writable = $role -eq "set"
                $tagId = "device.ems.schedule.hour$hour.$kind.$role"
                $tags.Add([pscustomobject]@{
                    tagId = $tagId; nodeId = "edge-1"; deviceId = "device"; meterCode = "device"
                    pointCode = "ems.schedule.hour$hour.$kind.$role"; semanticRole = $tagId
                    displayName = $tagId; unit = ""; dataType = "float64"
                    access = $(if ($writable) { "readWrite" } else { "read" }); indexFallback = 200 + $tags.Count
                })
                $mappings.Add([pscustomobject]@{
                    nodeId = "edge-1"; tagId = $tagId; sharedMemoryName = "gateway_point_store"
                    index = 200 + $mappings.Count; writable = $writable; dataType = "float64"; unit = ""
                })
            }
        }
    }
    Write-Json (Join-Path $sourceRoot "tags.json") @($tags)
    Write-Json (Join-Path $sourceRoot "runtime-map.json") @($mappings)
    foreach ($name in @("alarms.json", "permissions.json", "symbols.json", "trends.json")) {
        Write-Json (Join-Path $sourceRoot $name) @()
    }
    Write-Json (Join-Path $sourceRoot "topology.json") ([pscustomobject]@{ mode = "integrated" })
    Write-Json (Join-Path $sourceRoot "checksums.json") ([pscustomobject]@{})

    & (Join-Path $repoRoot "products\ems\stationary\1.0\tools\upgrade-retention.ps1") `
        -SourceProjectDirectory $sourceRoot `
        -VirtualConfig (Join-Path $repoRoot "config\factory\runtime\devices\device_ems_virtual.json") `
        -OutputProjectDirectory $outputRoot `
        -OutputPackage $package `
        -MachineCode "COMM202600777" `
        -PackageVersion "1.0.1-test"

    $manifest = Get-Content -Raw (Join-Path $outputRoot "manifest.json") | ConvertFrom-Json
    if ($manifest.productVersion -ne "1.0" -or
        $manifest.packageVersion -ne "1.0.1-test" -or
        $manifest.projectName -notmatch "Stationary Storage Cabinet") {
        throw "EMS 1.0 manifest version mismatch"
    }
    $runtimeDocument = Get-Content -Raw (Join-Path $outputRoot "runtime-map.json") | ConvertFrom-Json
    $runtime = @($runtimeDocument)
    $converted = @($runtime | Where-Object {
        $_.sharedMemoryName -eq "gateway_point_store_ems_virtual" -and $_.index -ge 400 -and $_.index -le 447
    })
    if ($converted.Count -ne 48 -or @($converted | Where-Object writable).Count -ne 48) {
        throw "expected 48 retained writable EMS 1.0 mappings, got $($converted.Count)/$(@($converted | Where-Object writable).Count)"
    }
    $convertedTagsDocument = Get-Content -Raw (Join-Path $outputRoot "tags.json") | ConvertFrom-Json
    $convertedTags = @($convertedTagsDocument)
    $duplicateSemanticRoles = @(
        $convertedTags |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.semanticRole) } |
            Group-Object { "$($_.nodeId)`u{001f}$($_.deviceId)`u{001f}$($_.semanticRole)" } |
            Where-Object Count -gt 1
    )
    if ($duplicateSemanticRoles.Count -ne 0) {
        throw "EMS 1.0 semantic roles must be unique"
    }
    $setpointTag = $convertedTags | Where-Object tagId -eq "device.ems.schedule.hour0.power.set"
    if (@($convertedTags | Where-Object tagId -eq "device.ems.schedule.hour0.power.current").Count -ne 0 -or
        $setpointTag.semanticRole -ne "ems.schedule.power.0" -or
        $setpointTag.access -ne "readWrite") {
        throw "EMS 1.0 current/setpoint tags were not merged into one read/write tag"
    }
    $duplicateRoutes = @(
        $runtime |
            Group-Object { "$($_.nodeId)`u{001f}$($_.sharedMemoryName)`u{001f}$($_.index)" } |
            Where-Object Count -gt 1
    )
    if ($duplicateRoutes.Count -ne 0) {
        throw "EMS 1.0 runtime routes must be unique"
    }
    $convertedScreen = Get-Content -Raw (Join-Path $outputRoot "screens\FirstPage.json") | ConvertFrom-Json
    if ($convertedScreen.widgets[0].bindings[0].tagId -ne "device.ems.schedule.hour0.power.set" -or
        $convertedScreen.widgets[0].geometry.width -ne 200 -or
        $convertedScreen.widgets[0].properties.qtText -ne "unchanged") {
        throw "EMS 1.0 screen tag reference merge changed visual properties"
    }
    if (-not (Test-Path -LiteralPath $package -PathType Leaf)) {
        throw "EMS 1.0 package was not generated"
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $invalidEntryNames = @($archive.Entries | Where-Object { $_.FullName.Contains('\') })
        if ($invalidEntryNames.Count -ne 0) {
            throw "EMS 1.0 package contains Windows-only ZIP entry paths"
        }

        $checksums = Get-Content -Raw (Join-Path $outputRoot "checksums.json") | ConvertFrom-Json
        foreach ($property in $checksums.PSObject.Properties) {
            $entry = $archive.GetEntry($property.Name)
            if ($null -eq $entry) {
                throw "EMS 1.0 package is missing checksummed entry: $($property.Name)"
            }
            $stream = $entry.Open()
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $actualHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
            } finally {
                $sha.Dispose()
                $stream.Dispose()
            }
            if ($actualHash -ne [string]$property.Value) {
                throw "EMS 1.0 package checksum mismatch: $($property.Name)"
            }
        }
    } finally {
        $archive.Dispose()
    }
    Write-Host "upgrade_ems_1_scada_retention_test passed"
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}
