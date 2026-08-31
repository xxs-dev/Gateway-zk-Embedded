[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectDirectory,

    [Parameter(Mandatory = $true)]
    [string]$RuntimeDeviceDirectory,

    [string[]]$AdditionalDeviceConfigs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Read-Json([string]$Path) {
    Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-PropertyValue([object]$Value, [string]$Name, [object]$DefaultValue) {
    if ($null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name]) {
        return $Value.PSObject.Properties[$Name].Value
    }
    return $DefaultValue
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
                # Some display text can begin with '[' or '{' without being JSON.
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

function Add-DeviceConfigPoints([string]$Path, [hashtable]$PointsByRoute) {
    $config = Read-Json $Path
    $memoryName = [string]$config.memoryStore.sharedMemoryName
    if ([string]::IsNullOrWhiteSpace($memoryName)) {
        return
    }
    foreach ($meter in @($config.meters)) {
        foreach ($point in @($meter.points)) {
            $key = "$memoryName`u{001f}$([uint32]$point.index)"
            if ($PointsByRoute.ContainsKey($key)) {
                throw "runtime configs contain a duplicate shared-memory point: $memoryName/$($point.index)"
            }
            $PointsByRoute[$key] = [pscustomobject]@{
                meterCode = [string]$meter.meterCode
                pointCode = [string]$point.pointCode
                displayName = [string]$point.name
                unit = [string](Get-PropertyValue $point.read "unit" "")
                writable = [bool](Get-PropertyValue $point.write "enable" $false)
                dataType = [string](Get-PropertyValue $point.read "dataType" "")
            }
        }
    }
}

$projectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).Path
$runtimeRoot = (Resolve-Path -LiteralPath $RuntimeDeviceDirectory).Path
$tagsDocument = Read-Json (Join-Path $projectRoot "tags.json")
$routesDocument = Read-Json (Join-Path $projectRoot "runtime-map.json")
# Windows PowerShell 5.1 can emit a root JSON array as one Object[] item.
$tags = @($tagsDocument | ForEach-Object { $_ })
$routes = @($routesDocument | ForEach-Object { $_ })

$tagById = @{}
foreach ($tag in $tags) {
    $tagById[[string]$tag.tagId] = $tag
}
$routeByTagId = @{}
foreach ($route in $routes) {
    $routeByTagId[[string]$route.tagId] = $route
}

$pointsByRoute = @{}
foreach ($configFile in Get-ChildItem -LiteralPath $runtimeRoot -File -Filter "*.json") {
    Add-DeviceConfigPoints $configFile.FullName $pointsByRoute
}
foreach ($configPath in $AdditionalDeviceConfigs) {
    Add-DeviceConfigPoints (Resolve-Path -LiteralPath $configPath).Path $pointsByRoute
}

$references = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($file in Get-ChildItem -LiteralPath $projectRoot -Recurse -File -Filter "*.json" |
    Where-Object Name -notin @("tags.json", "runtime-map.json", "checksums.json")) {
    Collect-TagReferences (Read-Json $file.FullName) $references
}

$issues = [System.Collections.Generic.List[object]]::new()
foreach ($tagId in $references) {
    if (-not $tagById.ContainsKey($tagId)) {
        $issues.Add([pscustomobject]@{ tagId = $tagId; issue = "unknown-tag"; detail = "" })
        continue
    }
    if (-not $routeByTagId.ContainsKey($tagId)) {
        $issues.Add([pscustomobject]@{ tagId = $tagId; issue = "missing-route"; detail = "" })
        continue
    }
    $tag = $tagById[$tagId]
    $route = $routeByTagId[$tagId]
    $routeKey = "$($route.sharedMemoryName)`u{001f}$([uint32]$route.index)"
    if (-not $pointsByRoute.ContainsKey($routeKey)) {
        $issues.Add([pscustomobject]@{
            tagId = $tagId
            issue = "missing-runtime-point"
            detail = "$($route.sharedMemoryName)/$($route.index)"
        })
        continue
    }
    $point = $pointsByRoute[$routeKey]
    $tagMeterCode = [string](Get-PropertyValue $tag "meterCode" "")
    $tagPointCode = [string](Get-PropertyValue $tag "pointCode" "")
    $routeUnit = [string](Get-PropertyValue $route "unit" "")
    $routeWritable = [bool](Get-PropertyValue $route "writable" $false)
    if (-not [string]::IsNullOrWhiteSpace($tagMeterCode) -and
        $tagMeterCode -ne [string]$point.meterCode) {
        $issues.Add([pscustomobject]@{
            tagId = $tagId
            issue = "meter-code"
            detail = "$tagMeterCode != $($point.meterCode)"
        })
    }
    if (-not [string]::IsNullOrWhiteSpace($tagPointCode) -and
        $tagPointCode -ne [string]$point.pointCode) {
        $issues.Add([pscustomobject]@{
            tagId = $tagId
            issue = "point-code"
            detail = "$tagPointCode != $($point.pointCode)"
        })
    }
    if ($routeUnit -ne [string]$point.unit) {
        $issues.Add([pscustomobject]@{
            tagId = $tagId
            issue = "unit"
            detail = "'$routeUnit' != '$($point.unit)'"
        })
    }
    if ($routeWritable -ne [bool]$point.writable) {
        $issues.Add([pscustomobject]@{
            tagId = $tagId
            issue = "writable"
            detail = "$routeWritable != $($point.writable)"
        })
    }
}

if ($issues.Count -ne 0) {
    $issues | Sort-Object issue, tagId | Format-Table -AutoSize -Wrap
    throw "SCADA runtime audit failed: $($issues.Count) issue(s) across $($references.Count) referenced tags"
}

Write-Host "SCADA runtime audit passed"
Write-Host "  referenced tags: $($references.Count)"
Write-Host "  runtime points: $($pointsByRoute.Count)"
Write-Host "  tags/routes: $($tags.Count)/$($routes.Count)"
