[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../../common/powershell/Repository.ps1')
$root=Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$temp=Join-Path ([IO.Path]::GetTempPath()) ('gateway-inventory-test-'+[guid]::NewGuid().ToString('N'))
$source=Join-Path $temp 'source'
$password=ConvertTo-SecureString 'fixture-only' -AsPlainText -Force
function Save-Fixture($Tags,$Routes){
    ConvertTo-Json -InputObject @($Tags) -Depth 30|Set-Content (Join-Path $source 'tags.json') -Encoding utf8
    ConvertTo-Json -InputObject @($Routes) -Depth 30|Set-Content (Join-Path $source 'runtime-map.json') -Encoding utf8
}
function Generate-Fixture($Name){
    & (Join-Path $root 'products/ems/mobile/2.0/tools/generate-scada.ps1') -SourceProjectDirectory $source -OutputProjectDirectory (Join-Path $temp $Name) -OutputPackage (Join-Path $temp "$Name.kyscada") -LocalOperatorPassword $password|Out-Host
    $script:screens=@(Get-ChildItem (Join-Path $temp "$Name/screens") -Filter '*.json'|ForEach-Object {Get-Content $_ -Raw|ConvertFrom-Json})
    foreach($screen in $screens){foreach($widget in $screen.widgets){
        if($null-ne$widget.action-and$widget.action.type-eq'navigate'-and$widget.action.targetScreen-notin$screens.screenId){throw 'Dangling page navigation'}
        if($null-ne$widget.properties.PSObject.Properties['qtMenuScreensJson']){foreach($entry in @($widget.properties.qtMenuScreensJson|ConvertFrom-Json)){if($entry.screen-notin$screens.screenId){throw 'Dangling menu navigation'}}}
        $g=$widget.geometry
        if($g.x-lt0-or$g.y-lt0-or$g.x+$g.width-gt1440-or$g.y+$g.height-gt810){throw "Overflow: $($screen.screenId)/$($widget.widgetId)"}
    }}
}
try{
    Copy-Item (Join-Path $root 'products/ems/mobile/2.0/scada/base-project') $source -Recurse
    $tags=@(Get-Content (Join-Path $source 'tags.json') -Raw|ConvertFrom-Json)
    $routes=@(Get-Content (Join-Path $source 'runtime-map.json') -Raw|ConvertFrom-Json)
    $originalTags=$tags;$originalRoutes=$routes
    $bms=($tags|Where-Object indexFallback -eq 1566).deviceId
    foreach($tag in @($tags|Where-Object {$_.deviceId-eq$bms-and($_.indexFallback-lt2000-or$_.indexFallback-in(2001..2034+3001..3017+@(3999)))})){
        $copy=$tag|ConvertTo-Json -Depth 20|ConvertFrom-Json
        $copy.deviceId='SECOND_BMS';$copy.meterCode='SECOND_BMS';$copy.tagId='SECOND_BMS.'+$tag.pointCode;$copy.indexFallback+=10000
        $route=($routes|Where-Object tagId -eq $tag.tagId)|ConvertTo-Json -Depth 20|ConvertFrom-Json
        $route.tagId=$copy.tagId;$route.index=$copy.indexFallback;$route.sharedMemoryName='fixture_second_bms'
        $tags+=$copy;$routes+=$route
    }
    Save-Fixture $tags $routes
    Generate-Fixture 'two-clusters'
    $second=@($screens|Where-Object screenId -like 'Devices-Cells-2*')
    if($second.Count-ne2){throw 'Second cluster needs exactly two pages for 34 cells'}
    foreach($page in $second){foreach($binding in @($page.widgets|ForEach-Object {$_.bindings})){
        if($binding.tagId-notlike'SECOND_BMS.*'){throw 'Cluster borrowed telemetry from another device'}
    }}
    $last=$second|Where-Object screenId -eq 'Devices-Cells-2-Page2'
    if(@($last.widgets|Where-Object widgetId -match '^cell-\d+-number$').Count-ne2){throw 'Final cluster page has phantom cells'}
    $probeIds=@($last.widgets|Where-Object widgetId -match '^cell-\d+-temperature$'|ForEach-Object {$_.bindings[0].tagId})
    if($probeIds.Count-ne2-or$probeIds[0]-ne$probeIds[1]-or$probeIds[0]-notlike'*.mb3_s2_3017'){throw '33/34 must share probe 17'}
    # Retain one configured meter regardless of live online state, and one DIO breaker.
    $keep=($originalTags|Where-Object indexFallback -eq 9030).deviceId
    $remove=@(($originalTags|Where-Object indexFallback -in @(1030,1130)).deviceId)
    $tags=@($originalTags|Where-Object {$_.deviceId-notin$remove-and$_.indexFallback-notin@(997,998)-and$_.displayName-notmatch'^单体(电压|温度)\d+$'})
    $routes=@($originalRoutes|Where-Object {$_.tagId-in$tags.tagId})
    Save-Fixture $tags $routes
    Generate-Fixture 'sparse'
    if('Devices-Meters-Page2'-in$screens.screenId-or'Devices-Cells-Page2'-in$screens.screenId){throw 'Phantom pagination'}
    foreach($id in @('Devices-Mccb','Control-Mccb')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|Where-Object widgetId -match '^mccb-\d+-title$').Count-ne1){throw 'Breaker count is not configuration-based'}
    }
    $meter=$screens|Where-Object screenId -eq 'Devices-Meters'
    if(@($meter.widgets|Where-Object widgetId -match '^meter-\d+-title$').Count-ne1){throw 'Phantom meter'}
    if(($meter.widgets|Where-Object widgetId -eq 'meter-0-online').bindings[0].tagId-notlike"$keep.*"){throw 'Configured offline meter was lost'}
    $tags=@($tags|Where-Object {$_.deviceId-ne$keep-and$_.indexFallback-ne999})
    Save-Fixture $tags @($routes|Where-Object {$_.tagId-in$tags.tagId})
    Generate-Fixture 'empty'
    foreach($entry in @(@{S='Devices-Meters';W='meters-empty'},@{S='Devices-Mccb';W='mccb-empty'},@{S='Devices-Cells';W='cells-empty'})){
        if(@(($screens|Where-Object screenId -eq $entry.S).widgets|Where-Object widgetId -eq $entry.W).Count-ne1){throw 'Missing empty configuration state'}
    }
    # More devices than fit on one board must paginate rather than overflow.
    $tags=$originalTags;$routes=$originalRoutes
    for($i=1;$i-le8;$i++){
        $tag=($originalTags|Where-Object indexFallback -eq 999)|ConvertTo-Json -Depth 20|ConvertFrom-Json
        $route=($originalRoutes|Where-Object index -eq 999)|ConvertTo-Json -Depth 20|ConvertFrom-Json
        $tag.tagId="EXTRA_BREAKER.$i";$tag.indexFallback=50000+$i;$tag.displayName="测试${i}断路器合位"
        $route.tagId=$tag.tagId;$route.index=$tag.indexFallback
        $tags+=$tag;$routes+=$route
    }
    for($i=1;$i-le4;$i++){
        foreach($original in @($originalTags|Where-Object deviceId -eq $keep)){
            $tag=$original|ConvertTo-Json -Depth 20|ConvertFrom-Json
            $route=($originalRoutes|Where-Object tagId -eq $original.tagId)|ConvertTo-Json -Depth 20|ConvertFrom-Json
            $tag.deviceId="EXTRA_METER_$i";$tag.meterCode=$tag.deviceId;$tag.tagId=$tag.deviceId+'.'+$tag.pointCode;$tag.indexFallback+=($i*10000)
            $route.tagId=$tag.tagId;$route.index=$tag.indexFallback
            $tags+=$tag;$routes+=$route
        }
    }
    Save-Fixture $tags $routes
    Generate-Fixture 'large'
    foreach($id in @('Devices-Mccb-Page2','Control-Mccb-Page2','Devices-Meters-Page4')){if($id-notin$screens.screenId){throw "Missing overflow page: $id"}}
    if(@(($screens|Where-Object screenId -eq 'Control-Mccb-Page2').widgets|Where-Object widgetId -match '^mccb-\d+-title$').Count-ne2){throw 'Incorrect breaker remainder'}
    if(@(($screens|Where-Object screenId -eq 'Meters-Overview').widgets|Where-Object widgetId -eq 'meter-device-selector').Count-ne1){throw 'Large meter selector missing'}
    Write-Host 'lanhu-inventory.test passed'
}finally{
    $target=[IO.Path]::GetFullPath($temp);$parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if($target.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase)-and(Split-Path $target -Leaf)-like'gateway-inventory-test-*'-and(Test-Path -LiteralPath $target)){Remove-Item -LiteralPath $target -Recurse -Force}
}
