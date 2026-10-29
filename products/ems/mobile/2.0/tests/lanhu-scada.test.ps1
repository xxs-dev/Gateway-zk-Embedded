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
    if('Devices-Cells-2'-in$screens.screenId){throw 'Unconfigured second battery cluster must not be generated'}
    foreach($asset in @('login-background.png','login-logo.png','login-user.png','login-password.png','access-lock.png','single-load-meter.png','single-grid-meter.png','single-pcs.png','single-battery.png')){
        if(-not(Test-Path (Join-Path $output "project/assets/lanhu-assets/$asset"))){throw "Missing login asset: $asset"}
    }
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
    $topology=$screens|Where-Object screenId -eq 'Topology'
    if(@($topology.widgets|Where-Object type -eq 'qtParallelTopology').Count-ne1){throw 'Dynamic vehicle topology is missing'}
    if(@($topology.widgets|Where-Object widgetId -like 'topology-storage-*').Count){throw 'Fixed vehicle placeholders must not remain'}
    if(@($topology.widgets|Where-Object type -eq 'qtParallelTopology'|ForEach-Object {$_.bindings}).Count){throw 'Dynamic peers must not borrow local telemetry'}
    $localTags=($topology.widgets|Where-Object type -eq 'qtParallelTopology').properties.localTelemetryTagsJson|ConvertFrom-Json
    $expectedLocal=@{p=1230;q=1234;pa=1227;pb=1228;pc=1229;ua=1220;ub=1221;uc=1222;factor=1242;frequency=1226;soc=1569;online=1399;bmsOnline=3999;breaker=997}
    foreach($key in $expectedLocal.Keys){if($map[$localTags.$key].index-ne$expectedLocal[$key]){throw "Local topology source mismatch: $key"}}
    $emu=$screens|Where-Object screenId -eq 'Devices-Emu'
    foreach($screen in $screens){
        $modeButtons=@($screen.widgets|Where-Object {$null-ne$_.properties.PSObject.Properties['emsControlModeTarget']})
        if($modeButtons.Count -and $screen.screenId -notlike 'Control-*'){throw 'EMS mode switch must stay on authenticated control pages'}
    $soe=$screens|Where-Object screenId -eq 'Info-Soe'
    if(@($soe.widgets|Where-Object type -eq 'qtSoeTable').Count-ne1){throw 'Native SOE history missing'}
    if(@($soe.widgets|Where-Object {$null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint')}).Count){throw 'SOE must be read-only'}
        if($modeButtons.Count -and (($modeButtons.properties.emsControlModeTarget|Sort-Object)-join',')-ne'local,remote'){throw 'EMS control mode selector must contain both choices'}
    }
    if(@(($screens|Where-Object screenId -eq 'Control-Pcs').widgets|Where-Object {$null-ne$_.properties.PSObject.Properties['emsControlModeTarget']}).Count-ne2){throw 'EMS control mode selector missing'}
    $hostFields=@($emu.widgets|Where-Object {$null-ne$_.properties.PSObject.Properties['localSystemField']}|ForEach-Object {$_.properties.localSystemField})
    if((($hostFields|Sort-Object)-join',')-ne((@('machineCode','model','firmware','controlMode','cloud','thirdParty','cpu','memory','disk','temperature')|Sort-Object)-join',')){throw 'EMU must bind all host fields'}
    $single=$screens|Where-Object screenId -eq 'Topology-Single'
    $gridState=$single.widgets|Where-Object widgetId -eq 'single-pcs-grid-state'
    if($gridState.properties.qtBackgroundColor-ne'#90979F'-or$gridState.properties.unavailableStateLabel-ne'--'){throw 'Unknown PCS mode must retain a neutral background and unavailable label'}
    if($gridState.stateRules.Count-ne2){throw 'PCS mode pair rules missing'}
    foreach($rule in $gridState.stateRules){
        if($rule.match-ne'all'-or$rule.conditions.Count-ne2){throw 'PCS mode must require both feedback points'}
        foreach($condition in $rule.conditions){
            $index=$map[$condition.tagId].index
            $expected=if($index-eq[int]$rule.code){'1'}else{'0'}
            if($index-notin@(1216,1217)-or$condition.comparison-ne'eq'-or$condition.value-ne$expected){throw 'PCS mode pair is not exclusive and binary'}
        }
        if(@($rule.conditions.tagId|Select-Object -Unique).Count-ne2){throw 'PCS mode pair duplicates one feedback point'}
    }
    if(@($single.widgets|Where-Object widgetId -eq 'single-pcs-dc-divider').Count-ne1){throw 'Single topology DC summary divider is missing'}
    foreach($entry in @(@{Id='single-load-label-1';X=463},@{Id='single-load-label-2';X=610},@{Id='single-grid-label-1';X=1116},@{Id='single-grid-label-2';X=1263},@{Id='single-pcs-ac-label-1';X=811},@{Id='single-pcs-ac-label-2';X=981})){
        if(($single.widgets|Where-Object widgetId -eq $entry.Id).geometry.x-ne$entry.X){throw "Single topology summary spacing differs from the design: $($entry.Id)"}
    }
    foreach($field in $single.widgets|Where-Object type -eq 'qtValue'){
        if($field.properties.qtFontWeight-ne'Normal'){throw "Single topology value is too bold: $($field.widgetId)"}
    }
    foreach($id in @('single-load-band','single-grid-band','single-pcs-band','single-battery-band')){
        if(($single.widgets|Where-Object widgetId -eq $id).properties.qtCornerRadius-ne8){throw "Single topology title band loses its rounded corners: $id"}
    }
    foreach($entry in @(@{Id='single-load-value-0';I=9039},@{Id='single-grid-value-0';I=1139},@{Id='single-pcs-ac-value-0';I=1230},@{Id='single-pcs-dc-voltage';I=1244},@{Id='single-pcs-dc-power';I=1243},@{Id='single-battery-soc';I=1569},@{Id='single-load-breaker';I=999},@{Id='single-grid-breaker';I=998},@{Id='single-pcs-breaker';I=997})){
        $widget=@($single.widgets|Where-Object widgetId -eq $entry.Id)
        if($widget.Count-ne1-or$widget[0].bindings.Count-ne1-or$map[$widget[0].bindings[0].tagId].index-ne$entry.I){throw "Single topology binding mismatch: $($entry.Id)"}
    }
    if(@($single.widgets|Where-Object {$_.widgetId-like'single-bus-*'-or$_.widgetId-eq'single-battery-breaker-reserved'}|ForEach-Object {$_.bindings}).Count){throw 'Unconfirmed bus/DC feedback borrows another source'}
    if(@($single.widgets|Where-Object {$null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint')}).Count){throw 'Single topology must remain read-only'}
    if(@($single.widgets|Group-Object widgetId|Where-Object Count -gt 1).Count){throw 'Duplicate single topology widget IDs'}
    if(@($strategy.widgets|Where-Object {$null-ne$_.action-and$_.action.type-eq'toggle'}).Count-ne1){throw 'Only confirmed reverse-power enable may be writable'}
    foreach($id in @('Control-Mccb','Control-Diesel')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|Where-Object {$null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint')}).Count-ne0){throw 'Reserved device has fabricated writes'}
    }
    $diesel=$screens|Where-Object screenId -eq 'Control-Diesel'
    if(@($diesel.widgets|Where-Object widgetId -eq 'diesel-empty').Count-ne1){throw 'Unconfigured diesel must have an empty state'}
    $cell=$screens|Where-Object screenId -eq 'Trends-Cell'
    if(($cell.widgets|Where-Object widgetId -eq 'trend-top-panel').geometry.height-ne264-or($cell.widgets|Where-Object widgetId -eq 'trend-bottom-panel').geometry.y-ne509){throw 'Cell chart panel geometry differs from the approved board'}
    $series=@(($cell.widgets|Where-Object widgetId -eq 'trend-cell-v').properties.chartSeriesJson|ConvertFrom-Json)
    if($series[1].color-ne'#129AF5'){throw 'Cell maximum series must use blue'}
    foreach($id in @('Maintenance','Meters-Energy')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|Where-Object widgetId -notlike 'lanhu-*').Count-ne0){throw 'Missing-design page is not blank'}
    }
    $pcs=$screens|Where-Object screenId -eq 'Control-Pcs'
    $bms=$screens|Where-Object screenId -eq 'Control-Bms'
    if(@($bms.widgets|Where-Object {$null-ne$_.action-and$_.action.type-in@('toggle','pulse','writeSetpoint')}).Count){throw 'Subordinate BMU/BCU controls are out of scope'}
    if(@($bms.widgets|Where-Object widgetId -in @('control-switch-mode','bms-reboot-reserved','bms-power-on-reserved','bms-power-off-reserved')).Count){throw 'Excluded BMS control placeholders remain'}
    foreach($entry in @(
        @{Screen='Control-Pcs';Widget='control-mode-value';Index=1310;Value='3';Label='恒功率充电'},
        @{Screen='Control-Bms';Widget='control-mode-value';Index=1454;Value='0';Label='静置'},
        @{Screen='Control-Bms';Widget='bms-status';Index=1550;Value='0';Label='正常'},
        @{Screen='Control-Thermal';Widget='control-mode-value';Index=128;Value='4';Label='自动控制'},
        @{Screen='Control-Thermal';Widget='tms-status-0';Index=163;Value='1';Label='开机'},
        @{Screen='Devices-Bms';Widget='bms-charge-state';Index=1454;Value='0';Label='静置'},
        @{Screen='Devices-Thermal';Widget='liquid-mode';Index=128;Value='4';Label='自动控制'}
    )){
        $enum=($screens|Where-Object screenId -eq $entry.Screen).widgets|Where-Object widgetId -eq $entry.Widget
        if($enum.type-ne'statusCard'-or$enum.bindings.Count-ne1-or$map[$enum.bindings[0].tagId].index-ne$entry.Index){throw "Missing enum feedback: $($entry.Screen)/$($entry.Widget)"}
        if($enum.properties.defaultStateLabel-ne'未知状态'-or$enum.properties.unavailableStateLabel-ne'反馈不可用'){throw 'Enums must distinguish unknown from unavailable, without raw-number fallback'}
        $rule=@($enum.stateRules|Where-Object {$_.conditions[0].value-eq$entry.Value})
        if($rule.Count-ne1-or$rule[0].label-ne$entry.Label){throw "Wrong enum meaning: $($entry.Screen)/$($entry.Widget)"}
        if($null-ne$enum.action){throw 'Feedback enum acquired a control action'}
    }
    foreach($entry in @(@{Screen='Control-Mccb';Text='无统一模式反馈'},@{Screen='Control-Diesel';Text='未配置模式反馈'})){
        $mode=($screens|Where-Object screenId -eq $entry.Screen).widgets|Where-Object widgetId -eq 'control-mode-value'
        if($mode.bindings.Count-ne0-or$mode.properties.qtText-ne$entry.Text){throw 'Missing modes must not borrow another device state'}
    }
    $status=$pcs.widgets|Where-Object widgetId -eq 'pcs-run-current'
    $indexes=@($status.bindings|ForEach-Object {$map[$_.tagId].index}|Sort-Object)
    if(($indexes-join',')-ne'1209,1210,1211'){throw 'PCS run status bound to incorrect points'}
    foreach($id in @('Monitor-Network','Monitor-Strategy','Devices-Emu','Devices-Pcs','Devices-Bms','Devices-Thermal','Devices-Mccb','Devices-Meters','Devices-Meters-Page2','Devices-Diesel','Devices-Cells')){
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
                if($widget.widgetId-like'*-voltage'){
                    $voltageIndexes+=$index
                    if($widget.properties.valueSuffix.Trim()-ne'V'){throw 'Scaled cell voltage must be V'}
                }else{
                    $temperatureIndexes+=$index
                    $slot=[int]($widget.widgetId-split'-')[1]
                    $number=[int]($screen.widgets|Where-Object widgetId -eq "cell-$slot-number").properties.qtText
                    if($index-ne(3000+[math]::Ceiling($number/2))){throw 'Each probe must serve its adjacent cell pair'}
                }
            }
        }
    }
    if((($voltageIndexes|Sort-Object)-join',') -ne ((2001..2260)-join',')){throw 'Cell voltage coverage is not exactly 260'}
    if((($temperatureIndexes|Sort-Object)-join',') -ne ((3001..3130|ForEach-Object {$_;$_})-join',')){throw 'Cell temperature coverage is not exactly 130 shared probes'}
    foreach($id in @('Devices-Diesel','Devices-Emu')){
        if(@(($screens|Where-Object screenId -eq $id).widgets|ForEach-Object {$_.bindings}).Count){throw "Reserved device borrows another source: $id"}
    }
    foreach($screen in $screens){foreach($table in $screen.widgets|Where-Object {$_.type-eq'alarmTable'-and$null-ne$_.properties.PSObject.Properties['qtAlarmPresentation']-and$_.properties.qtAlarmPresentation-eq'summary'}){
        if($null-eq$table.properties.PSObject.Properties['qtAlarmDeviceIdsJson']){throw 'Unfiltered monitor alarms'}
        $devices=@($table.properties.qtAlarmDeviceIdsJson|ConvertFrom-Json)
        if($screen.screenId-eq'Devices-Diesel'-and$devices.Count){throw 'Diesel displays unrelated alarms'}
    }}
    $monitorPcs=$screens|Where-Object screenId -eq 'Devices-Pcs'
    if(($monitorPcs.widgets|Where-Object widgetId -eq 'pcs-overview-band').geometry.y-ne396){throw 'PCS DC/AC footer not updated'}
    $monitorBms=$screens|Where-Object screenId -eq 'Devices-Bms'
    if(@($monitorBms.widgets|Where-Object widgetId -eq 'bms-energy-total-panel').Count){throw 'Old merged BMS totals remain'}
    if(@($monitorBms.widgets|Where-Object widgetId -like 'bms-energy-total-*-tile').Count-ne3){throw 'BMS totals require three independent tiles'}
    $thermal=$screens|Where-Object screenId -eq 'Devices-Thermal'
    foreach($section in @('liquid','dehumidifier','fan','fire','alarms')){
        $heading=$thermal.widgets|Where-Object widgetId -eq "thermal-$section-heading"
        if($heading.properties.qtTextAlignment-ne'Center'){throw "TMS section is not centered: $section"}
    }
    $acPanel=($monitorPcs.widgets|Where-Object widgetId -eq 'pcs-ac-panel').geometry
    foreach($field in $monitorPcs.widgets|Where-Object widgetId -like 'pcs-ac-total-*'){
        if($field.geometry.y+$field.geometry.height -gt $acPanel.y+$acPanel.height-4){throw 'PCS totals overflow their panel'}
    }
    foreach($screen in $screens){
        $brand=($screen.widgets|Where-Object widgetId -eq 'lanhu-brand').geometry
        $separator=($screen.widgets|Where-Object widgetId -eq 'lanhu-brand-separator').geometry
        if($brand.x+$brand.width -gt $separator.x-8){throw 'Header brand overlaps separator'}
        foreach($label in $screen.widgets|Where-Object {$_.widgetId -like '*-label' -and $_.type -eq 'qtLabel'}){
            $value=$screen.widgets|Where-Object widgetId -eq ($label.widgetId -replace '-label$','')
            if($null -ne $value -and $value.geometry.x -eq $label.geometry.x -and $value.geometry.y -gt $label.geometry.y){
                if($label.geometry.y+$label.geometry.height -gt $value.geometry.y){throw "Overlapping label/value: $($screen.screenId)/$($label.widgetId)"}
            }
        }
    }
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
