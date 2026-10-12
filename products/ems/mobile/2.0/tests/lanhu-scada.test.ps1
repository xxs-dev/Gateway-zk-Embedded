[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\..\common\powershell\Repository.ps1')
$root=Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$output=Join-Path ([IO.Path]::GetTempPath()) ('gateway-lanhu-test-'+[guid]::NewGuid().ToString('N'))
$password=ConvertTo-SecureString 'lanhu-test-only' -AsPlainText -Force
try {
    & (Join-Path $root 'products\ems\mobile\2.0\tools\generate-scada.ps1') -SourceProjectDirectory (Join-Path $root 'products\ems\mobile\2.0\scada\base-project') -OutputProjectDirectory (Join-Path $output 'project') -OutputPackage (Join-Path $output 'lanhu.kyscada') -MachineCode COMM202600999 -LocalOperatorPassword $password
    $screens=@(Get-ChildItem (Join-Path $output 'project\screens') -Filter '*.json' | ForEach-Object {Get-Content $_.FullName -Raw|ConvertFrom-Json})
    if($screens.Count-ne56){throw 'Lanhu screen count changed'}
    $map=@{}; Get-Content (Join-Path $output 'project\runtime-map.json') -Raw|ConvertFrom-Json|ForEach-Object {$map[$_.tagId]=$_}
    foreach($screen in $screens){
        if($screen.width-ne1440-or$screen.height-ne810){throw "Invalid canvas: $($screen.screenId)"}
        if(@($screen.widgets|Where-Object widgetId -like 'lanhu-nav-*'|Where-Object type -eq 'qtButton').Count-ne8){throw 'Missing navigation'}
        foreach($widget in $screen.widgets){
            $g=$widget.geometry
            if($g.x-lt0-or$g.y-lt0-or($g.x+$g.width)-gt1440-or($g.y+$g.height)-gt810){throw "Out of bounds: $($screen.screenId)/$($widget.widgetId)"}
            foreach($binding in $widget.bindings){if(-not$map.ContainsKey($binding.tagId)){throw 'Unresolved binding'}}
            if($null-ne$widget.action-and$widget.action.type-eq'navigate'-and$widget.action.targetScreen-notin$screens.screenId){throw 'Missing navigation target'}
        }
    }
    $strategy=$screens|Where-Object screenId -eq 'Strategy-Overview'
    if(@($strategy.widgets|Where-Object {$null-ne$_.action-and$_.action.type-eq'toggle'}).Count-ne1){throw 'Only confirmed reverse-power enable may be writable'}
    foreach($id in @('Control-Mccb','Control-Diesel')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|Where-Object {$null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint')}).Count-ne0){throw 'Reserved device has fabricated writes'}
    }
    $diesel=$screens|Where-Object screenId -eq 'Control-Diesel'
    foreach($entry in @(@{Id='diesel-start-title';T='遥控快速停机'},@{Id='diesel-breaker-title';T='遥控市电合分闸'},@{Id='diesel-mode-title';T='遥控发电合分闸'},@{Id='diesel-reset-title';T='遥控手/自动'})){
        if(($diesel.widgets|Where-Object widgetId -eq $entry.Id).properties.qtText-ne$entry.T){throw 'Diesel labels differ from the approved board'}
    }
    $cell=$screens|Where-Object screenId -eq 'Trends-Cell'
    if(($cell.widgets|Where-Object widgetId -eq 'trend-top-panel').geometry.height-ne264-or($cell.widgets|Where-Object widgetId -eq 'trend-bottom-panel').geometry.y-ne509){throw 'Cell chart panel geometry differs from the approved board'}
    $series=@(($cell.widgets|Where-Object widgetId -eq 'trend-cell-v').properties.chartSeriesJson|ConvertFrom-Json)
    if($series[1].color-ne'#129AF5'){throw 'Cell maximum series must use blue'}
    foreach($id in @('Maintenance','Meters-Energy')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|Where-Object widgetId -notlike 'lanhu-*').Count-ne0){throw 'Missing-design page is not blank'}
    }
    $pcs=$screens|Where-Object screenId -eq 'Control-Pcs'
    $status=$pcs.widgets|Where-Object widgetId -eq 'pcs-run-current'
    $indexes=@($status.bindings|ForEach-Object {$map[$_.tagId].index}|Sort-Object)
    if(($indexes-join',')-ne'1209,1210,1211'){throw 'PCS run status bound to incorrect points'}
    foreach($id in @('Monitor-Network','Monitor-Strategy','Devices-Emu','Devices-Pcs','Devices-Bms','Devices-Thermal','Devices-Mccb','Devices-Meters','Devices-Meters-Page2','Devices-Diesel','Devices-Cells','Devices-Cells-2')){
        $monitor=$screens|Where-Object screenId -eq $id
        if(@($monitor.widgets|Where-Object widgetId -notlike 'lanhu-*').Count-lt15){throw "Monitor layout missing: $id"}
        if(@($monitor.widgets|Where-Object {$_.type-eq'pcsPhasePowerControl'-or($null-ne$_.action-and$_.action.type-in@('writeSetpoint','toggle','pulse'))}).Count){throw "Monitor is writable: $id"}
    }
    $tags=@{};Get-Content (Join-Path $output 'project\tags.json') -Raw|ConvertFrom-Json|ForEach-Object {$tags[$_.tagId]=$_}
    $voltageIndexes=@();$temperatureIndexes=@()
    foreach($screen in $screens|Where-Object {$_.screenId-eq'Devices-Cells'-or$_.screenId-like'Devices-Cells-Page*'}){
        $selector=$screen.widgets|Where-Object widgetId -eq 'cells-page-selector'
        $menu=@($selector.properties.qtMenuScreensJson|ConvertFrom-Json)
        if($menu.Count-ne9-or@($menu|Where-Object {$_.screen-notin$screens.screenId}).Count){throw 'Incomplete cell page menu'}
        foreach($widget in $screen.widgets|Where-Object widgetId -match '^cell-[0-9]+-(voltage|temperature)$'){
            foreach($binding in $widget.bindings){
                $index=$map[$binding.tagId].index
                if($tags[$binding.tagId].deviceId-ne$tags[($map.Values|Where-Object index -eq 1566).tagId].deviceId){throw 'Cell source differs from BMS'}
                if($widget.widgetId-like'*-voltage'){$voltageIndexes+=$index}else{$temperatureIndexes+=$index}
            }
        }
    }
    if((($voltageIndexes|Sort-Object)-join',') -ne ((2001..2260)-join',')){throw 'Cell voltage coverage is not exactly 260'}
    if((($temperatureIndexes|Sort-Object)-join',') -ne ((3001..3260)-join',')){throw 'Cell temperature coverage is not exactly 260'}
    foreach($id in @('Devices-Diesel','Devices-Cells-2','Devices-Emu')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|ForEach-Object {$_.bindings}).Count){throw "Reserved device borrows another source: $id"}
    }
    foreach($screen in $screens){foreach($table in $screen.widgets|Where-Object {$_.type-eq'alarmTable'-and$null-ne$_.properties.PSObject.Properties['qtAlarmPresentation']-and$_.properties.qtAlarmPresentation-eq'summary'}){
        if($null-eq$table.properties.PSObject.Properties['qtAlarmDeviceIdsJson']){throw 'Unfiltered monitor alarms'}
        $devices=@($table.properties.qtAlarmDeviceIdsJson|ConvertFrom-Json)
        if($screen.screenId-eq'Devices-Diesel'-and$devices.Count){throw 'Diesel displays unrelated alarms'}
    }}
    $monitorPcs=$screens|Where-Object screenId -eq 'Devices-Pcs'
    foreach($entry in @(@{Id='pcs-dc-3';I=1250;U='kWh'},@{Id='pcs-ac-total-3';I=1248;U='kWh'})){
        $widget=$monitorPcs.widgets|Where-Object widgetId -eq $entry.Id
        if($map[$widget.bindings[0].tagId].index-ne$entry.I-or$widget.properties.valueSuffix.Trim()-ne$entry.U){throw 'PCS energy binding mismatch'}
    }
    & (Join-Path $root 'tools\release\validate-ems-package.ps1') -Package (Join-Path $output 'lanhu.kyscada') -Product mobile -ExpectedMachineCode COMM202600999
    # Read-only sources retain feedback and contain no executable control widgets.
    $source=Join-Path $output 'readonly-source';Copy-Item (Join-Path $root 'products\ems\mobile\2.0\scada\base-project') $source -Recurse
    $mapPath=Join-Path $source 'runtime-map.json';$maps=Get-Content $mapPath -Raw|ConvertFrom-Json
    $maps|ForEach-Object {$_.writable=$false};ConvertTo-Json -InputObject $maps -Depth 30|Set-Content $mapPath -Encoding utf8
    & (Join-Path $root 'products\ems\mobile\2.0\tools\generate-scada.ps1') -SourceProjectDirectory $source -OutputProjectDirectory (Join-Path $output 'readonly') -OutputPackage (Join-Path $output 'readonly.kyscada') -LocalOperatorPassword $password
    $readonly=@(Get-ChildItem (Join-Path $output 'readonly\screens') -Filter '*.json'|ForEach-Object {Get-Content $_.FullName -Raw|ConvertFrom-Json})
    if(@($readonly.widgets|Where-Object {$_.type-eq'pcsPhasePowerControl'-or($null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint'))}).Count-ne0){throw 'Read-only source contains controls'}
    Write-Host 'lanhu-scada.test passed'
} finally {
    $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $target=[IO.Path]::GetFullPath($output)
    if($target.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase)-and(Split-Path $target -Leaf)-like'gateway-lanhu-test-*'-and(Test-Path -LiteralPath $target)){Remove-Item -LiteralPath $target -Recurse -Force}
}
