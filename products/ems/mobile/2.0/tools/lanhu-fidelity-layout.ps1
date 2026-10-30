# The approved boards use a 1440 x 810 canvas. Coordinates below are design pixels.
function Set-RefInlineStatusLayout($Widgets) {
    foreach($widget in $Widgets){
        if($widget.type-eq'statusCard'-and-not$widget.title-and$widget.properties.qtBorderless-eq'true'-and$widget.properties.qtHideStatusLamp-eq'true'){
            $widget.properties|Add-Member qtAutoFit 'true' -Force
        }
    }
}
function Set-RefEnergyUnitLayout($Widgets) {
    foreach($widget in $Widgets){
        if($widget.type-eq'qtValue'-and$widget.properties.PSObject.Properties['valueSuffix']-and$widget.properties.valueSuffix.Trim()-in@('kWh','kvarh')){
            $widget.properties|Add-Member qtEnergyAutoUnit 'true' -Force
        }
    }
}
function Set-RefTopologyFieldAlignment($Widgets) {
    foreach($widget in $Widgets){
        if($widget.widgetId-match'^topology-(transformer|grid|diesel)-title-[0-3]$'){
            $widget.properties.qtTextAlignment='Center'
        }
    }
}
function New-RefScreen($Id, $Widgets) {
    if((Has-Index 1214)-and(Tag-At 1214).tagId-eq'MTR20250600657.mb4_s1_1214'){
        $validMode=$true
        if($script:runtimePointByIndex.ContainsKey([uint32]1214)){
            $mode=$script:runtimePointByIndex[[uint32]1214]
            $validMode=$mode.point.address-eq86-and$mode.point.read.function-eq2-and$mode.point.read.enable-and$mode.meterCode-eq'MTR20250600657'
        }
        if($validMode){
            foreach($widget in $Widgets){
                $pcsWrite=$null-ne$widget.action-and$widget.action.type-in@('writeSetpoint','toggle','pulse')-and$widget.action.tagId.StartsWith('MTR20250600657.')
                $pcsPower=$widget.type-eq'pcsPhasePowerControl'-and$widget.bindings.Count-eq6-and@($widget.bindings|Where-Object {-not$_.tagId.StartsWith('MTR20250600657.')}).Count-eq0
                if($pcsWrite-or$pcsPower){$widget.properties|Add-Member pcsRemoteTag ([string](Tag-At 1214).tagId) -Force}
            }
        }
    }
    Set-RefInlineStatusLayout $Widgets
    Set-RefEnergyUnitLayout $Widgets
    if($Id-eq'Topology'){Set-RefTopologyFieldAlignment $Widgets}
    $screen=New-Screen $Id $Id $Widgets
    $screen.width=1440; $screen.height=810
    return $screen
}
function Ref-Panel($Id,$X,$Y,$W,$H,$Color='#FFFFFF') { New-LanhuPanel $Id $X $Y $W $H $Color }
function Ref-Text($Id,$Text,$X,$Y,$W,$H,$Size=16,$Color='#303438',$Bold=$false,$Align='Left') {
    $widget=New-Text $Id $Text $X $Y $W $H $Size $Color $Align $Bold
    $widget.properties | Add-Member qtVerticalAlignment 'Center' -Force
    $widget.properties | Add-Member qtAutoFit 'true' -Force
    return $widget
}
function Ref-Button($Id,$Text,$Target,$X,$Y,$W,$H,$Active=$false,$Radius=8) {
    $button=New-LanhuButton $Id $Text $Target $Active $X $Y $W $H
    $button.properties.qtFontSize=16; $button.properties.qtCornerRadius=$Radius
    $button.properties | Add-Member qtTheme 'light' -Force
    return $button
}
function Ref-Value($Widgets,$Id,$Index,$X,$Y,$W,$H,$Unit='',$Size=16) {
    Add-LanhuValue $Widgets $Id $Index $X $Y $W $H $Unit $Size
    $Widgets[$Widgets.Count-1].properties | Add-Member qtVerticalAlignment 'Center' -Force
    $Widgets[$Widgets.Count-1].properties | Add-Member qtAutoFit 'true' -Force
}
function Ref-EnumValue($Widgets,$Id,$Index,$Labels,$X,$Y,$W,$H,$Size=16) {
    $available=$null-ne$Index-and(Has-Index $Index)
    if($available-and$script:runtimePointByIndex.ContainsKey([uint32]$Index)){
        $point=$script:runtimePointByIndex[[uint32]$Index].point
        $available=$null-ne$point.PSObject.Properties['read']-and[bool]$point.read.enable
        if($null-ne$point.PSObject.Properties['valueMap']-and$null-ne$point.valueMap){
            $mapped=[ordered]@{}
            foreach($property in $point.valueMap.PSObject.Properties){$mapped[$property.Name]=[string]$property.Value}
            if($mapped.Count-gt0){$Labels=$mapped}
        }
    }
    if(-not$available){$Widgets.Add((Ref-Text $Id '未配置反馈' $X $Y $W $H $Size '#90979F' $false 'Center'));return}
    $rules=@();$priority=0
    foreach($key in $Labels.Keys){$priority++;$rules+=New-StateRule "$Id-$priority" $Labels[$key] '#303438' $Index 'eq' ([string]$key) $priority}
    # State rules keep unknown codes and invalid quality out of numeric fallback.
    $Widgets.Add((New-Widget $Id 'statusCard' '' $X $Y $W $H 5 @((New-Binding $Index)) $rules $null ([ordered]@{
        qtBackgroundColor='transparent';qtFontSize=$Size;qtTheme='light';qtTextColor='#303438';
        qtBorderless='true';qtHideStatusLamp='true';qtVerticalAlignment='Center';qtTextAlignment='Center';qtAutoFit='true';
        defaultStateLabel='未知状态';unavailableStateLabel='反馈不可用';stateColorMode='indicator'
    })))
}
function Ref-Chrome($Widgets,$Id,$Active) {
    $Widgets.Add((New-Frame 'lanhu-background' 0 0 1440 810 '#E5E9EC' -100))
    $clock=Ref-Text 'lanhu-clock' '' 140 23 300 30 16
    $clock.properties | Add-Member qtClockFormat 'yyyy-MM-dd  HH:mm:ss'
    $Widgets.Add($clock)
    $logo=New-LanhuImage 'lanhu-logo' 'login-logo.png' 472 18 116 36
    $logo.properties | Add-Member qtImageFit 'contain';$Widgets.Add($logo)
    $Widgets.Add((Ref-Text 'lanhu-brand' $projectDisplayName 600 17 184 36 23 '#25292D' $true))
    $Widgets.Add((New-Frame 'lanhu-brand-separator' 792 26 1 16 '#C3CBD1' 1))
    $Widgets.Add((Ref-Text 'lanhu-machine' $MachineCode 816 22 355 28 18))
    $Widgets.Add((New-Frame 'lanhu-header-rule' 132 60 1290 1 '#D4DADF' 1))
    $Widgets.Add((Ref-Button 'lanhu-login' '登  录' 'Control-Pcs' 1272 8 108 48 $true 24))
    $nav=@(@{Id='Guide';T='引导';Target='Guide-Setup'},@{Id='Meters';T='仪表';Target='Meters-Overview'},@{Id='Topology';T='拓扑';Target='Topology'},@{Id='Trends';T='曲线';Target='Trends-Custom'},@{Id='Alarms';T='告警';Target='Info-Alarms'},@{Id='Monitor';T='监测';Target='Devices-Pcs'},@{Id='Control';T='控制';Target='Control-Pcs'},@{Id='Settings';T='设置';Target='Maintenance'})
    for($i=0;$i-lt8;$i++) {
        $n=$nav[$i];$y=42+$i*93
        if($n.Id-eq'Monitor'){$n.Target='Devices-Emu'}
        $b=Ref-Button "lanhu-nav-$($n.Id)" '' $n.Target 12 $y 82 76 ($Active-eq$n.Id) 16
        $b.properties | Add-Member qtImageFile "assets/lanhu-assets/nav-$i-$(if($Active-eq$n.Id){'active'}else{'idle'}).png"
        $Widgets.Add($b)
        $label=Ref-Text "lanhu-nav-label-$i" $n.T 14 ($y+43) 78 30 20 $(if($Active-eq$n.Id){'#FFFFFF'}else{'#626970'}) $false 'Center'
        $label.zIndex=22; $Widgets.Add($label)
    }
}
function Ref-Tabs($Widgets,$Tabs,$Active,$X,$Y,$W) {
    $panel=Ref-Panel "ref-tabs-$Y" $X $Y $W 48
    $panel.properties.qtCornerRadius=24; $Widgets.Add($panel)
    $width=$W/$Tabs.Count
    for($i=0;$i-lt$Tabs.Count;$i++) {
        $t=$Tabs[$i];$b=Ref-Button "ref-tab-$Y-$($t.Id)" $t.T $t.Target ($X+$i*$width) $Y $width 48 ($t.Id-eq$Active) 24
        $Widgets.Add($b)
    }
}
function Ref-MeterTabs($Widgets,$Active) {
    $tabs=@($lanhuMeters|ForEach-Object {@{Id=$_.Source;T=$_.Name;Target=$_.Target}})
    if(Has-Index 1220){$tabs+=@{Id='Pcs';T='PCS交流侧';Target='Meters-Electrical-Pcs'}}
    if($tabs.Count-gt0-and$tabs.Count-le6){Ref-Tabs $Widgets $tabs $Active 300 80 930}
    elseif($tabs.Count-gt6){
        $current=@($tabs|Where-Object Id -eq $Active)[0]
        $select=Ref-Button 'meter-device-selector' $current.T $current.Target 563 80 360 40
        $select.properties|Add-Member qtMenuScreensJson (ConvertTo-Json -InputObject @($tabs|ForEach-Object {@{text=$_.T;screen=$_.Target}}) -Compress);$Widgets.Add($select)
    }
}
function Ref-Chart($Widgets,$Id,$Indexes,$Names,$Units,$X,$Y,$W,$H) {
    $palette=@('#D99B14','#18CC70','#F42B59','#129AF5')
    $colors=@();for($i=0;$i-lt$Indexes.Count;$i++){$colors+=$palette[$i%$palette.Count]}
    $chart=New-Chart $Id '' ([uint32[]]$Indexes) ([string[]]$Names) ([string[]]$Units) ([string[]]$colors) $X $Y $W $H
    $series=@($chart.properties.chartSeriesJson|ConvertFrom-Json)
    $chart.properties.chartSeriesJson=ConvertTo-Json -InputObject $series -Depth 8 -Compress
    $chart.properties | Add-Member qtTheme 'light'
    $chart.properties | Add-Member qtLegendAlignment 'Center'
    $chart.properties | Add-Member qtAxisFontSize 14
    $chart.properties | Add-Member qtLegendFontSize 14
    $Widgets.Add($chart)
}
function Build-RefMeter($Id,$Source) {
    if($Id-eq'Meters-Overview'-and$Source-notin@($lanhuMeters|ForEach-Object {$_.Source})-and$lanhuMeters.Count){$Source=$lanhuMeters[0].Source}
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $Id 'Meters';Ref-MeterTabs $w $Source
    if($Source-ne'Pcs'-and$Source-notin@($lanhuMeters|ForEach-Object {$_.Source})){
        $w.Add((Ref-Text 'meter-empty' '未配置电表' 162 260 1000 50 22));return New-RefScreen $Id $w
    }
    $w.Add((New-LanhuImage 'meter-heading-icon' 'meter-heading.png' 132 150 26 26))
    $w.Add((Ref-Text 'meter-heading' '实时监测值' 170 147 420 32 18 '#303438' $true))
    $w.Add((Ref-Panel 'meter-table' 132 190 617 239));$w.Add((Ref-Panel 'meter-summary' 772 190 631 239))
    $w.Add((New-Frame 'meter-head-row' 148 210 582 48 '#F0F3F9' 1))
    $w.Add((New-Frame 'meter-stripe-row' 148 310 582 50 '#F6F8FA' 1))
    $indexes=Get-LanhuMeterIndexes $Source
    $columns=@(@{T='电压';K='V';U='V';X=232;W=76},@{T='电流';K='I';U='A';X=326;W=76},@{T='有功功率';K='P';U='kW';X=419;W=92},@{T='无功功率';K='Q';U='kvar';X=532;W=92},@{T='视在功率';K='S';U='kVA';X=631;W=92})
    foreach($c in $columns) {
        $w.Add((Ref-Text "meter-head-$($c.K)" $c.T $c.X 220 $c.W 30 14 '#303438' $false 'Center'))
        foreach($phase in 0..2){Ref-Value $w "meter-$($c.K)-$phase" $indexes[$c.K][$phase] $c.X (266+$phase*52) $c.W 34 $c.U 16}
    }
    foreach($p in 0..2){
        $w.Add((New-Widget "meter-phase-dot-$p" 'qtEllipse' '' 164 (278+$p*52) 8 8 2 @() @() $null ([ordered]@{qtBackgroundColor=@('#FFA21C','#18CC70','#F42B31')[$p]})))
        $w.Add((Ref-Text "meter-phase-$p" "$(@('A','B','C')[$p])相" 185 (266+$p*52) 40 34 14))
    }
    $summary=@(@{Id='v';T='平均电压';I=$indexes.AvgV;U='V';C='#E6F2F8'},@{Id='i';T='平均电流';I=$indexes.AvgI;U='A';C='#F0EDF7'},@{Id='pf';T='功率因数';I=$indexes.Factor;U='';C='#E9F8F3'},@{Id='hz';T='频率';I=$indexes.Frequency;U='Hz';C='#FCF6EB'})
    for($i=0;$i-lt4;$i++){
        $s=$summary[$i];$x=790+$i*152
        $w.Add((Ref-Panel "meter-summary-$($s.Id)" $x 211 143 92 $s.C))
        $w.Add((New-LanhuImage "meter-summary-icon-$i" "metric-$i.png" ($x+12) 239 38 38))
        $w.Add((Ref-Text "meter-summary-title-$i" $s.T ($x+62) 230 77 26 14 '#777E85'))
        Ref-Value $w "meter-summary-value-$i" $s.I ($x+56) 255 82 28 $s.U 16
    }
    $w.Add((Ref-Panel 'meter-totals' 790 317 594 92 '#E6F2F8'))
    $totals=@(@{T='总有功功率';I=$indexes.TotalP;U='kW'},@{T='总无功功率';I=$indexes.TotalQ;U='kvar'},@{T='总视在功率';I=$indexes.TotalS;U='kVA'})
    foreach($i in 0..2){$s=$totals[$i];$x=830+$i*187;$w.Add((New-LanhuImage "meter-total-icon-$i" 'metric-total.png' $x 344 38 38));$w.Add((Ref-Text "meter-total-title-$i" $s.T ($x+48) 334 120 30 14 '#777E85'));Ref-Value $w "meter-total-value-$i" $s.I ($x+48) 362 116 30 $s.U 16}
    $w.Add((New-LanhuImage 'meter-chart-icon' 'chart-heading.png' 132 447 26 26))
    $w.Add((Ref-Text 'meter-chart-title' '近2小时有功功率曲线' 170 444 700 32 18 '#303438' $true))
    $w.Add((Ref-Panel 'meter-chart-panel' 132 486 1271 298))
    Ref-Chart $w 'meter-power-chart' @($indexes.P+@($indexes.TotalP)) @('Pa','Pb','Pc','P总') @('kW','kW','kW','kW') 154 512 1228 260
    $w[$w.Count-1].properties.chartDefaultWindowMinutes=120
    return New-RefScreen $Id $w
}
function Set-RefFeedback($Button,$Index,$Value='') {
    if($null-ne$Index-and(Has-Index $Index)){
        $Button.properties|Add-Member controlFeedbackTag ([string]$script:mappingByIndex[[uint32]$Index].tagId) -Force
        if($Value-ne''){$Button.properties|Add-Member controlFeedbackValue ([string]$Value) -Force}
    }
}
function Test-RefProtocolPoint($Index,$Address,$Function,$DevicePattern) {
    if(-not(Is-WritableIndex $Index)-or-not$script:runtimePointByIndex.ContainsKey([uint32]$Index)){return $false}
    $entry=$script:runtimePointByIndex[[uint32]$Index];$p=$entry.point
    return $p.enabled-and$p.address-eq$Address-and$p.write.function-eq$Function-and$entry.meterCode-match$DevicePattern
}
function Ref-ControlChrome($Widgets,$Kind) {
    Ref-Chrome $Widgets "Control-$Kind" 'Control'
    Ref-Tabs $Widgets @(@{Id='Device';T='设备控制';Target='Control-Pcs'},@{Id='Strategy';T='策略控制';Target='Strategy-Overview'}) 'Device' 562 80 308
    $Widgets.Add((Ref-Panel 'control-body' 132 155 1271 614))
    $Widgets.Add((New-Frame 'control-tabs-band' 132 155 1271 51 '#F7F7F7' 1))
    $Widgets.Add((Ref-Text 'ems-control-mode-title' 'EMS控制权限' 1000 168 130 30 16 '#777E85'))
    foreach($m in @(@{Id='remote';T='远程';X=1140},@{Id='local';T='就地';X=1250})){
        $b=Ref-Button "ems-control-mode-$($m.Id)" $m.T '' $m.X 163 100 36
        $b.properties|Add-Member emsControlModeTarget $m.Id
        $Widgets.Add($b)
    }
    $tabs=@(@{Id='Pcs';T='PCS';X=154},@{Id='Bms';T='BMS';X=272},@{Id='Thermal';T='TMS';X=382},@{Id='Mccb';T='MCCB';X=494},@{Id='Diesel';T='柴油发电机';X=608})
    foreach($t in $tabs){
        $b=Ref-Button "control-tab-$($t.Id)" $t.T "Control-$($t.Id)" $t.X 160 $(if($t.Id-eq'Diesel'){126}else{66}) 42
        $b.properties.qtTransparent=$true;$b.properties.qtTextColor=if($t.Id-eq$Kind){'#129AF5'}else{'#303438'}
        $Widgets.Add($b)
        if($t.Id-eq$Kind){$Widgets.Add((New-Frame 'control-active-line' $t.X 203 $(if($t.Id-eq'Diesel'){86}else{66}) 3 '#129AF5' 21))}
    }
    $Widgets.Add((New-LanhuImage 'control-mode-icon' 'control-mode.png' 162 250 66 66))
    $Widgets.Add((Ref-Text 'control-mode-heading' $(if($Kind-eq'Bms'-and$null-ne$script:lanhuBau){'BAU控制'}else{'模式控制'}) 252 248 420 32 20 '#303438' $true))
    $Widgets.Add((Ref-Text 'control-mode-title' $(if($Kind-eq'Bms'){'充放电状态：'}else{'当前模式：'}) 252 289 120 26 18 '#777E85'))
    switch($Kind){
        # Yingbo V3.3.24, holding register 301. This is a mode, not a running indication.
        'Pcs' {Ref-EnumValue $Widgets 'control-mode-value' 1310 ([ordered]@{'0'='无';'1'='恒流充电';'2'='恒压充电';'3'='恒功率充电';'4'='直流恒压模式'}) 376 289 220 26 18}
        'Bms' {Ref-EnumValue $Widgets 'control-mode-value' $(if($null-ne$script:lanhuBau){Get-LanhuBauPoint 8205}else{1454}) ([ordered]@{'0'='静置';'1'='放电';'2'='充电'}) 376 289 220 26 18}
        # Xinmiao readback 128 uses 1..4; write-only 164 uses different 0..3 codes.
        'Thermal' {Ref-EnumValue $Widgets 'control-mode-value' 128 ([ordered]@{'1'='水泵循环';'2'='制冷模式';'3'='制热模式';'4'='自动控制'}) 376 289 220 26 18}
        default {$Widgets.Add((Ref-Text 'control-mode-value' $(if($Kind-eq'Mccb'){'无统一模式反馈'}else{'未配置模式反馈'}) 376 289 220 26 18 '#90979F'))}
    }
    if($Kind-eq'Bms'){return}
    $b=Ref-Button 'control-switch-mode' $(if($Kind-eq'Diesel'){'停机'}else{'切换模式'}) '' 1222 242 150 48 $true 24
    $b.properties | Add-Member qtIconFile $(if($Kind-eq'Diesel'){'assets/lanhu-assets/command-stop.png'}else{'assets/lanhu-assets/command-mode.png'})
    if($Kind-eq'Diesel'){$b.properties.qtBackgroundColor='#F52D59'}
    if($Kind-eq'Pcs'-and(Test-RefProtocolPoint 1310 301 6 '^MTR20250600657$')){
        $b.action=New-Action 'writeSetpoint' '' 1310
        $b.properties|Add-Member controlOptionsJson (ConvertTo-Json -Compress @(@{label='无';value=0},@{label='恒流充电';value=1},@{label='恒压充电';value=2},@{label='恒功率充电';value=3},@{label='直流恒压模式';value=4}))
        Set-RefFeedback $b 1310
    }
    if($Kind-eq'Thermal'-and(Test-RefProtocolPoint 164 200 6 '_LIQUID_COOLING$')){
        $b.action=New-Action 'writeSetpoint' '' 164
        $b.properties|Add-Member controlOptionsJson (ConvertTo-Json -Compress @(@{label='自动控制';value=0;feedbackValue=4},@{label='制冷模式';value=1;feedbackValue=2},@{label='制热模式';value=2;feedbackValue=3},@{label='水泵循环';value=3;feedbackValue=1}))
        Set-RefFeedback $b 128
    }
    $Widgets.Add($b)
}
function Ref-Command($Widgets,$Id,$Title,$Index,$Value,$X,$Y,$Color='#2599D6') {
    $b=Ref-Button $Id $Title '' $X $Y 82 36 $false 18
    $b.properties.qtBackgroundColor='#F2FAFF';$b.properties.qtBorderColor=$Color;$b.properties.qtTextColor=$Color;$b.properties.qtFontSize=14
    $icon=if($Title-in@('停机','停止','快速停机')){'stop'}elseif($Title-eq'运行'-or$Title-eq'启用'){'run'}elseif($Title-eq'复位'){'reset'}elseif($Title-eq'合闸'){'breaker'}elseif($Title-eq'自动'){'auto'}else{''}
    if($icon){$b.properties | Add-Member qtIconFile "assets/lanhu-assets/command-$icon.png"}
    if($null-ne$Index -and (Is-WritableIndex ([uint32]$Index))){$b.action=New-Action 'writeSetpoint' '' $Index $Value}
    $Widgets.Add($b)
}
function Ref-ControlPanel($Widgets,$Id,$Title,$X,$Y,$Width,$Height) {
    $frame=Ref-Panel "$Id-panel" $X $Y $Width $Height '#F7F7F7';$Widgets.Add($frame)
    $Widgets.Add((New-Frame "$Id-body" ($X+1) ($Y+44) ($Width-2) ($Height-45) '#FFFFFF' 1))
    $Widgets.Add((Ref-Text "$Id-heading" $Title $X ($Y+6) $Width 34 18 '#303438' $true 'Center'))
    $outline=New-Frame "$Id-outline" $X $Y $Width $Height 'transparent' 4
    $outline.properties|Add-Member qtCornerRadius 8;$outline.properties|Add-Member qtBorderColor '#E6E6E6';$Widgets.Add($outline)
}
function Build-RefPcs {
    $w=[System.Collections.Generic.List[object]]::new();Ref-ControlChrome $w 'Pcs'
    Ref-ControlPanel $w 'pcs-run' '运行状态' 162 350 592 123
    Ref-ControlPanel $w 'pcs-grid' '并离网状态' 780 350 592 123
    $w.Add((Ref-Text 'pcs-run-current-title' '当前状态：' 182 419 76 32 14 '#777E85'))
    if((Has-Index 1209)-and(Has-Index 1210)-and(Has-Index 1211)){
        $w.Add((New-Widget 'pcs-run-current' 'statusCard' '' 258 422 46 24 5 @((New-Binding 1209),(New-Binding 1210),(New-Binding 1211)) @((New-StateRule 'stopped' '停机' '#F52D59' 1209 'eq' '1' 10),(New-StateRule 'standby' '待机' '#FFA21C' 1210 'eq' '1' 20),(New-StateRule 'running' '运行' '#18C773' 1211 'eq' '1' 30)) $null ([ordered]@{qtBackgroundColor='transparent';qtFontSize=16;qtTextColor='#FFFFFF';qtTheme='light';qtBorderless='true';qtVerticalAlignment='Center';qtTextAlignment='Center';qtHideStatusLamp='true';unavailableStateLabel='--'})))
    }else{Ref-Value $w 'pcs-run-current' $null 258 419 46 32}
    Ref-Command $w 'control-pcs-stop' '停机' 1202 '1' 438 417 '#EE5078'
    Set-RefFeedback $w[$w.Count-1] 1209 '1'
    Ref-Command $w 'control-pcs-start' '运行' 1201 '1' 542 417
    Set-RefFeedback $w[$w.Count-1] 1211 '1'
    Ref-Command $w 'control-pcs-reset' '复位' 1200 '1' 645 417
    $w.Add((Ref-Text 'pcs-grid-current-title' '当前状态：' 800 419 76 32 14 '#777E85'))
    if((Has-Index 1216)-and(Has-Index 1217)){
        $w.Add((New-Widget 'pcs-grid-current' 'statusCard' '' 876 422 46 24 5 @((New-Binding 1216),(New-Binding 1217)) @((New-StateRule 'grid' '并网' '#18C773' 1216 'eq' '1' 10),(New-StateRule 'offgrid' '离网' '#F52D59' 1217 'eq' '1' 20)) $null ([ordered]@{qtBackgroundColor='transparent';qtFontSize=16;qtTextColor='#FFFFFF';qtTheme='light';qtBorderless='true';qtVerticalAlignment='Center';qtTextAlignment='Center';qtHideStatusLamp='true';unavailableStateLabel='--'})))
    }else{Ref-Value $w 'pcs-grid-current' $null 876 419 46 32}
    $gridIndex=if(Test-RefProtocolPoint 1315 306 6 '^MTR20250600657$'){1315}else{$null}
    Ref-Command $w 'pcs-grid-connect' '并网' $gridIndex '0' 1158 417
    Set-RefFeedback $w[$w.Count-1] 1315 '0'
    $w[$w.Count-1].properties|Add-Member controlWarning '此操作修改并离网设定；实际运行状态以并离网遥信为准。'
    Ref-Command $w 'pcs-grid-disconnect' '离网' $gridIndex '1' 1264 417 '#EE5078'
    Set-RefFeedback $w[$w.Count-1] 1315 '1'
    $w[$w.Count-1].properties|Add-Member controlWarning '此操作修改并离网设定；实际运行状态以并离网遥信为准。'
    $bindings=@();foreach($e in @(@{I=1318;S='activeA'},@{I=1319;S='activeB'},@{I=1320;S='activeC'},@{I=1321;S='reactiveA'},@{I=1322;S='reactiveB'},@{I=1323;S='reactiveC'})){if(Is-WritableIndex $e.I){$bindings+=New-Binding $e.I $e.S}}
    if($bindings.Count-gt0){$w.Add((New-Widget 'control-pcs-phase-power' 'pcsPhasePowerControl' '' 162 497 1210 226 5 $bindings @() $null ([ordered]@{qtTheme='light';qtPresentation='lanhu'})))}
    else {foreach($i in 0..1){Ref-ControlPanel $w "pcs-power-$i" @('有功功率控制','无功功率控制')[$i] (162+$i*618) 497 592 226}}
    return New-RefScreen 'Control-Pcs' $w
}
function Ref-SettingRow($Widgets,$Id,$Title,$Index,$X,$Y,$Width=560,$Icon='control-temperature.png') {
    $Widgets.Add((New-LanhuImage "$Id-icon" $Icon $X ($Y+5) 28 30))
    $Widgets.Add((Ref-Text "$Id-title" $Title ($X+40) $Y 155 40 14 '#777E85'))
    $box=Ref-Panel "$Id-box" ($X+177) $Y ($Width-288) 38 '#FFFFFF';$box.zIndex=2;$box.properties|Add-Member qtBorderColor '#E6E6E6';$Widgets.Add($box)
    Ref-Value $Widgets "$Id-feedback" $Index ($X+190) $Y ($Width-316) 38 '' 14
    $b=Ref-Button $Id '修改' '' ($X+$Width-90) $Y 84 36 $false 18
    $b.properties.qtTextColor='#2599D6';$b.properties.qtBorderColor='#89C3E0';$b.properties.qtBackgroundColor='#F2FAFF';$b.properties.qtFontSize=14
    $b.properties|Add-Member qtIconFile 'assets/lanhu-assets/command-edit.png'
    if($null-ne$Index -and (Is-WritableIndex ([uint32]$Index))){$b.action=New-Action 'writeSetpoint' '' $Index}
    $Widgets.Add($b)
}
function Ref-DeviceTile($Widgets,$Id,$Title,$Icon,$Command,$X,$Y,$Color='#2599D6',$Feedback=$null) {
    $panel=Ref-Panel "$Id-panel" $X $Y 385 84
    $panel.properties|Add-Member qtBorderColor '#E6E6E6';$Widgets.Add($panel)
    $Widgets.Add((New-LanhuImage "$Id-icon" $Icon ($X+14) ($Y+18) 42 42))
    $Widgets.Add((Ref-Text "$Id-title" $Title ($X+74) ($Y+10) 178 28 16 '#303438' $true))
    $Widgets.Add((Ref-Text "$Id-status-title" '当前状态：' ($X+74) ($Y+40) 76 28 14 '#777E85'))
    if($null-ne$Feedback){Ref-EnumValue $Widgets "$Id-feedback" $Feedback ([ordered]@{'0'='分闸';'1'='合闸'}) ($X+150) ($Y+40) 118 28 14}
    else{Ref-Value $Widgets "$Id-feedback" $null ($X+150) ($Y+40) 46 28}
    Ref-Command $Widgets "$Id-reserved" $Command $null '' ($X+280) ($Y+24) $Color
    if($Command-eq'快速停机'){$Widgets[$Widgets.Count-1].geometry.width=112;$Widgets[$Widgets.Count-1].geometry.x=$X+258}
}
function Build-RefDeviceControl($Kind,$Page=1) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-ControlChrome $w $Kind
    switch($Kind){
        'Bms' {
            Ref-ControlPanel $w 'bms-control' '运行状态' 162 350 592 123
            $w.Add((Ref-Text 'bms-status-title' '当前状态：' 182 419 76 32 14 '#777E85'))
            Ref-EnumValue $w 'bms-status' $(if($null-ne$script:lanhuBau){Get-LanhuBauPoint 8193}else{1550}) ([ordered]@{'0'='正常';'1'='禁充';'2'='禁放';'3'='待机';'4'='停机'}) 258 419 124 32
            if($null-ne$script:lanhuBau){
                $bau=$script:lanhuBau
                $scope="BAU · $($bau.sourceCom) · 地址$($bau.slave)"
                $w.Add((Ref-Text 'bms-control-target' $scope 780 248 570 40 18 '#303438' $true 'Right'))
                $power=Get-LanhuBauPoint 16385 $true
                $reboot=Get-LanhuBauPoint 16386 $true
                Ref-Command $w 'bau-power-on' '上电' $power '2' 438 417
                $w[$w.Count-1].properties|Add-Member controlWarning ($scope+'：执行上电流程。驱动回执不代表接触器已闭合。')
                Ref-Command $w 'bau-power-off' '下电' $power '1' 542 417 '#EE5078'
                $w[$w.Count-1].properties|Add-Member controlWarning ($scope+'：执行下电流程，按协议在电流小于5A后断开总正、总负接触器。')
                Ref-Command $w 'bau-reboot' '重启' $reboot '1' 645 417 '#EE5078'
                $w[$w.Count-1].properties|Add-Member controlWarning ($scope+'：先执行下电流程再重启，不是故障复位。')
                Ref-ControlPanel $w 'bau-contactors' '接触器反馈' 780 350 592 123
                $w.Add((Ref-Text 'bau-positive-label' '总正：' 800 419 76 32 16 '#777E85'))
                Ref-EnumValue $w 'bau-positive' (Get-LanhuBauPoint 8195 $false '总正接触器状态') ([ordered]@{'0'='断开';'1'='闭合'}) 876 419 120 32
                $w.Add((Ref-Text 'bau-negative-label' '总负：' 1090 419 76 32 16 '#777E85'))
                Ref-EnumValue $w 'bau-negative' (Get-LanhuBauPoint 8195 $false '主负接触器状态') ([ordered]@{'0'='断开';'1'='闭合'}) 1166 419 120 32
                $w.Add((Ref-Text 'bms-monitor-only' '下属Pack级BMU / BCU：仅监测' 182 492 1000 36 16 '#90979F'))
            }else{
                $w.Add((Ref-Text 'bms-monitor-only' 'BMU / BCU：仅监测' 438 417 290 32 16 '#90979F'))
            }
        }
        'Thermal' {
            Ref-ControlPanel $w 'tms-control-0' '液冷控制' 162 350 592 123
            $w.Add((Ref-Text 'tms-status-title-0' '开关机使能：' 182 419 100 32 14 '#777E85'))
            Ref-EnumValue $w 'tms-status-0' 163 ([ordered]@{'0'='关机';'1'='开机'}) 282 419 142 32
            $enableIndex=if(Test-RefProtocolPoint 163 201 6 '_LIQUID_COOLING$'){163}else{$null}
            Ref-Command $w 'tms-stop-0' '停止' $enableIndex '0' 542 417 '#EE5078'
            Set-RefFeedback $w[$w.Count-1] 163 '0'
            Ref-Command $w 'tms-enable-0' '启用' $enableIndex '1' 645 417
            Set-RefFeedback $w[$w.Count-1] 163 '1'
            Ref-ControlPanel $w 'tms-temperature' '液冷设定温度' 780 350 592 123
            Ref-Value $w 'tms-set-temperature' 166 802 410 150 40 '℃' 20
            $w.Add((Ref-Text 'tms-temperature-unavailable' '修改范围待厂家确认' 982 410 360 40 16 '#90979F'))
            Ref-ControlPanel $w 'dehumidifier-settings' '除湿器参数' 162 497 1210 246
            $settings=@(@{I=220;T='加热启动温度(℃)'},@{I=221;T='加热回差温度(℃)'},@{I=222;T='除湿启动湿度(%)'},@{I=223;T='除湿回差湿度(%)'},@{I=224;T='排风启动温度(℃)'},@{I=225;T='排风回差温度(℃)'})
            for($i=0;$i-lt$settings.Count;$i++){
                $s=$settings[$i];$index=if(Test-RefProtocolPoint $s.I ($s.I-210) 16 '_DEHUMIDIFIER$'){$s.I}else{$null}
                Ref-SettingRow $w "tms-setting-$($s.I)" $s.T $index (180+($i%2)*612) (558+[math]::Floor($i/2)*54) 540
                $button=$w[$w.Count-1]
                if($null-ne$index){
                    $write=$script:runtimePointByIndex[[uint32]$index].point.write
                    if($null-ne$write.PSObject.Properties['minValue']-and$null-ne$write.PSObject.Properties['maxValue']){
                        $button.properties|Add-Member controlMin ([string]$write.minValue)
                        $button.properties|Add-Member controlMax ([string]$write.maxValue)
                        $button.properties|Add-Member controlDecimals '0'
                        Set-RefFeedback $button $index
                    }else{$button.action=$null}
                }
            }
        }
        'Mccb' {
            $items=@($lanhuBreakers|Select-Object -Skip (($Page-1)*9) -First 9)
            for($i=0;$i-lt$items.Count;$i++){
                $item=$items[$i];$x=162+($i%3)*412;$y=348+[math]::Floor($i/3)*102
                Ref-DeviceTile $w "mccb-$i" $item.Name 'control-breaker.png' '合闸' $x $y '#2599D6' $item.Position
                $w.RemoveAt($w.Count-1)
                foreach($op in @(@{Id='open';T='分闸';I=$item.Trip;F='0';Y=$y+4;C='#EE5078'},@{Id='close';T='合闸';I=$item.Close;F='1';Y=$y+44;C='#2599D6'})){
                    Ref-Command $w "mccb-$i-$($op.Id)" $op.T $op.I '0' ($x+288) $op.Y $op.C
                    $button=$w[$w.Count-1];Set-RefFeedback $button $item.Position $op.F
                    $button.properties|Add-Member controlWarning ($item.Name+'：立即'+$op.T+'，请确认现场允许操作。')
                }
            }
            if(-not$items.Count){$w.Add((Ref-Text 'mccb-empty' '未配置断路器' 162 350 1000 50 22))}
            if($lanhuBreakers.Count-gt9){Monitor-Pager $w 'mccb' $(if($Page-gt1){Get-LanhuPageId 'Control-Mccb' ($Page-1)}else{''}) $(if($Page*9-lt$lanhuBreakers.Count){Get-LanhuPageId 'Control-Mccb' ($Page+1)}else{''})}
        }
        'Diesel' {
            $w.Add((Ref-Text 'diesel-empty' '未配置柴油发电机控制点' 162 350 1000 50 22 '#90979F'))
        }
    }
    return New-RefScreen (Get-LanhuPageId "Control-$Kind" $Page) $w
}
function Build-RefStrategy {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w 'Strategy-Overview' 'Control'
    Ref-Tabs $w @(@{Id='Device';T='设备控制';Target='Control-Pcs'},@{Id='Strategy';T='策略控制';Target='Strategy-Overview'}) 'Strategy' 562 80 308
    $w.Add((Ref-Panel 'strategy-body' 132 155 1271 614))
    $items=@(@{Id='reverse';T='防逆流';I=458},@{Id='capacity';T='动态增容';I=$null},@{Id='reactive';T='无功治理';I=$null},@{Id='emergency';T='应急保电';I=$null},@{Id='balance';T='不平衡治理';I=$null},@{Id='schedule';T='定时充放电';I=$null},@{Id='voltage';T='电压治理';I=$null})
    for($i=0;$i-lt7;$i++){$s=$items[$i];$x=162+$(if($i-lt4){0}else{618});$y=178+$(if($i-lt4){$i}else{$i-4})*98;$panel=Ref-Panel "strategy-$($s.Id)-panel" $x $y 592 79;$panel.properties|Add-Member qtBorderColor '#E6E6E6';$w.Add($panel);$w.Add((New-LanhuImage "strategy-$($s.Id)-icon" "strategy-$i.png" ($x+16) ($y+18) 43 44));$w.Add((Ref-Text "strategy-$($s.Id)-title" $s.T ($x+74) ($y+22) 360 36 18));$b=Ref-Button "strategy-$($s.Id)-enable" '禁止  使能' '' ($x+474) ($y+26) 90 28 $false 14;$b.properties.qtFontSize=12;$b.properties.qtBorderColor='#129AF5';if($null-ne$s.I-and(Is-WritableIndex $s.I)){$b.action=New-Action 'toggle' '' $s.I;$b.properties|Add-Member qtToggleStyle 'true'};$w.Add($b)}
    return New-RefScreen 'Strategy-Overview' $w
}
function Ref-State($Widgets,$Id,$Index,$Normal,$Good,$Bad,$X,$Y,$W=100,$H=28) {
    if($null-ne$Index-and(Has-Index $Index)){
        $rules=@((New-StateRule "$Id-good" $Good '#18C773' $Index 'eq' $Normal 10),(New-StateRule "$Id-bad" $Bad '#F52D59' $Index 'ne' $Normal 20))
        $rules[0].image='assets/lanhu-assets/status-normal.png';$rules[1].image='assets/lanhu-assets/status-abnormal.png'
        $s=New-Widget $Id 'statusLamp' '' $X $Y $W $H 5 @((New-Binding $Index)) $rules $null ([ordered]@{qtBackgroundColor='transparent';qtTextColor='#303438';qtFontSize=16;qtTheme='light';qtBorderless='true';qtVerticalAlignment='Center';qtStatusIcon='true';stateColorMode='indicator';unavailableStateLabel='--'})
        if($W-lt82){$s.properties.qtStatusIcon='false';$s.properties|Add-Member qtHideStatusLamp 'true'}
        $Widgets.Add($s)
    }else{$Widgets.Add((Ref-Text $Id '--' $X $Y $W $H 16 '#90979F' $false 'Center'))}
}
function Build-RefGuide($Stage) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w "Guide-$Stage" 'Guide'
    Ref-Tabs $w @(@{Id='Setup';T='自检阶段';Target='Guide-Setup'},@{Id='Deploy';T='部署阶段';Target='Guide-Deploy'}) $(if($Stage-eq'Setup'){'Setup'}else{'Deploy'}) 614 80 308
    if($Stage-eq'Setup'){
        $titles=@('网络检查','车辆安全检查','功能检查');$icons=@('guide-network.png','guide-safety.png','guide-function.png')
        foreach($i in 0..2){$x=132+$i*429;$w.Add((Ref-Panel "guide-panel-$i" $x 165 414 498));$w.Add((New-LanhuImage "guide-icon-$i" $icons[$i] ($x+21) 183 63 64));$w.Add((Ref-Text "guide-title-$i" $titles[$i] ($x+102) 183 300 38 22 '#303438' $true));$w.Add((New-Frame "guide-rule-$i" ($x+21) 262 372 1 '#EDF0F3' 1))}
        foreach($i in 0..2){$w.Add((Ref-Text "guide-description-$i" @('检查网络通信状态，确保数据连接正常','检查车辆运行与安全相关模块状态','检查各系统功能模块运行状态')[$i] (234+$i*429) 218 300 28 14 '#777E85'))}
        $cols=@(
            @(@{T='4G连接检查';I=920000003;N='1'},@{T='GPS定位检查';I=$xmindVehiclePoints.gps;N='1'}),
            @(@{T='车辆检查-制动';I=$xmindVehiclePoints.brake;N='1'},@{T='车辆检查-支撑';I=$xmindVehiclePoints.support;N='1'},@{T='车辆安全检查';I=$dioPoints.emergency;N=(Resolve-XMindNormalValue $dioPoints.emergency '0')},@{T='车辆安全检查';I=$null;N='1'}),
            @(@{T='拓扑检查';I=$null;N='1'},@{T='PCS检查';I=1399;N='1'},@{T='BMS检查';I=3999;N='1'},@{T='液冷检查';I=127;N='1'},@{T='消防检查';I=310000;N='1'},@{T='智能表检查';I=$null;N='1'},@{T='断路器检查';I=$null;N='1'})
        )
        foreach($col in 0..2){$x=153+$col*429;for($row=0;$row-lt$cols[$col].Count;$row++){$s=$cols[$col][$row];$y=275+$row*48;$w.Add((New-Widget "guide-number-bg-$col-$row" 'qtEllipse' '' $x ($y+2) 28 28 1 @() @() $null ([ordered]@{qtBackgroundColor='#F7F9FA'})));$w.Add((Ref-Text "guide-number-$col-$row" ($row+1) $x $y 24 30 16 '#303438' $false 'Center'));$w.Add((Ref-Text "guide-item-$col-$row" $s.T ($x+43) $y 250 30 16));Ref-State $w "guide-state-$col-$row" $s.I $s.N '正常' '异常' ($x+294) $y 82 30}}
        foreach($row in 2..3){$y=275+$row*48;$w.Add((Ref-Panel "guide-safety-badge-$row" 737 ($y+5) 42 22 $(if($row-eq2){'#E5F4FC'}else{'#F1F2F3'})));$w.Add((Ref-Text "guide-safety-badge-label-$row" $(if($row-eq2){'自动'}else{'手动'}) 737 ($y+5) 42 22 14 $(if($row-eq2){'#2599D6'}else{'#777E85'}) $false 'Center'))}
        $manual=Ref-Button 'guide-manual-reserved' '手动检查结果' '' 660 589 214 48 $false 24;$manual.properties.qtBorderColor='#89C3E0';$manual.properties.qtTextColor='#129AF5';$w.Add($manual)
        $w.Add((Ref-Panel 'guide-footer' 132 681 1271 88));$w.Add((New-LanhuImage 'guide-footer-icon' 'guide-footer.png' 153 698 60 58));$w.Add((Ref-Text 'guide-result-title' '自检结果：' 234 704 116 40 22 '#303438' $true));$w.Add((Ref-Text 'guide-result-normal' '正常 -- 项' 397 704 180 40 20));$w.Add((Ref-Text 'guide-result-abnormal' '异常 -- 项' 566 704 180 40 20));$w.Add((New-LanhuImage 'guide-count-normal-icon' 'status-normal.png' 353 708 30 30));$w.Add((New-LanhuImage 'guide-count-abnormal-icon' 'status-abnormal.png' 519 708 30 30));$w.Add((Ref-Button 'guide-next' '去部署  →' 'Guide-Deploy' 1208 699 176 50 $true 25))
    }else{
        $w.Add((Ref-Text 'guide-mode-title' '运行方式' 132 149 100 44 18 '#777E85'))
        $w.Add((Ref-Button 'guide-emergency-mode-reserved' '应急保电' '' 237 149 126 44 $false));$w.Add((Ref-Button 'guide-grid-mode-reserved' '并网治理' '' 381 149 126 44 $false))
        $titles=@('网侧电压检查','相序检查','柴油机挂载检查');$icons=@('guide-voltage.png','guide-phase.png','guide-diesel.png')
        foreach($i in 0..2){$x=132+$i*429;$w.Add((Ref-Panel "guide-panel-$i" $x 210 414 453));$w.Add((New-LanhuImage "guide-icon-$i" $icons[$i] ($x+21) 227 63 64));$w.Add((Ref-Text "guide-title-$i" $titles[$i] ($x+102) 240 300 38 22 '#303438' $true));$w.Add((New-Frame "guide-rule-$i" ($x+21) 308 372 1 '#EDF0F3' 1))}
        $w.Add((Ref-Text 'guide-voltage-heading' '检查数据' 153 331 180 30 16 '#777E85'));$w.Add((Ref-Panel 'guide-voltage-band' 153 372 372 52 '#F8FAFB'))
        foreach($i in 0..2){$x=158+$i*123;$w.Add((Ref-Text "guide-voltage-phase-$i" "$(@('A','B','C')[$i])相" $x 382 38 30 16 '#777E85'));Ref-Value $w "guide-voltage-$i" (1030+$i) ($x+40) 382 76 30 'V' 16}
        $w.Add((Ref-Text 'guide-voltage-result-label' '检查结果' 153 455 116 32 16 '#777E85'));Ref-State $w 'guide-voltage-result' $null '1' '正常' '异常' 270 455 120 32
        $w.Add((Ref-Text 'guide-phase-supply-label' '供电状态' 582 331 106 30 16 '#777E85'));Ref-State $w 'guide-phase-supply' $null '1' '已供电' '未供电' 687 331 152 30
        $w.Add((Ref-Text 'guide-phase-result-label' '检查结果' 582 372 106 30 16 '#777E85'));Ref-State $w 'guide-phase-result' 322024 '0' '相序正确' '相序错误' 687 372 152 30
        $w.Add((Ref-Panel 'guide-phase-reserved-space' 582 407 372 176 '#F8FAFB'))
        $w.Add((Ref-Text 'guide-diesel-mounted-label' '是否挂载柴油机' 1010 328 232 34 16 '#777E85'));$w.Add((Ref-Button 'guide-diesel-enable-reserved' '--' '' 1170 328 58 28 $false 14))
        $w.Add((Ref-Text 'guide-diesel-state-label' '柴油机挂载情况' 1010 369 194 34 16 '#777E85'));Ref-State $w 'guide-diesel-state' $null '1' '已挂载' '未挂载' 1204 369 166 34
        $w.Add((Ref-Panel 'guide-diesel-reserved-space' 1010 412 372 126 '#F8FAFB'))
        foreach($b in @(@{Id='voltage';T='电压检查';X=269;W=140},@{Id='phase-start';T='启动/停止';X=606;W=140},@{Id='phase-confirm';T='相序正确';X=788;W=140},@{Id='diesel';T='挂载检查';X=1127;W=140})){$button=Ref-Button "guide-$($b.Id)-reserved" $b.T '' $b.X 595 $b.W 46 $false 23;$button.properties.qtTextColor='#129AF5';$button.properties.qtBorderColor='#89C3E0';$w.Add($button)}
        $w.Add((Ref-Panel 'guide-footer' 132 681 1271 88));$w.Add((New-LanhuImage 'guide-footer-icon' 'guide-info.png' 153 698 60 58));$w.Add((Ref-Button 'guide-complete-reserved' '完成部署' '' 1208 699 176 50 $true 25))
    }
    return New-RefScreen "Guide-$Stage" $w
}
function Build-RefTrends($Kind) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w "Trends-$Kind" 'Trends'
    Ref-Tabs $w @(@{Id='Custom';T='自定义';Target='Trends-Custom'},@{Id='Cell';T='电芯';Target='Trends-Cell'}) $(if($Kind-eq'Cell'){'Cell'}else{'Custom'}) 634 80 268
    $title=if($Kind-eq'Cell'){'电芯电压'}else{'功率与电压'}
    $w.Add((New-LanhuImage 'trend-top-icon' $(if($Kind-eq'Cell'){'cell-voltage-heading.png'}else{'power-heading.png'}) 132 150 26 26));$w.Add((Ref-Text 'trend-top-title' $title 170 149 600 30 18))
    $w.Add((Ref-Panel 'trend-top-panel' 132 188 1271 $(if($Kind-eq'Cell'){264}else{300})))
    if($Kind-eq'Cell'){
        Ref-Chart $w 'trend-cell-v' @(1577,1574) @('最低电压','最高电压') @('mV','mV') 152 204 1230 236
        $series=@($w[$w.Count-1].properties.chartSeriesJson|ConvertFrom-Json);$series[1].color='#129AF5';$w[$w.Count-1].properties.chartSeriesJson=ConvertTo-Json -InputObject $series -Depth 8 -Compress
        $second='电芯温度';$indexes=@(1583,1580);$names=@('最低温度','最高温度');$units=@('℃','℃')
    }else{
        $selector=Ref-Button 'trend-source-selector' '电源侧监测' 'Trends-Power' 1220 148 182 32
        $selector.properties | Add-Member qtMenuScreensJson (ConvertTo-Json -Compress @(@{text='电源侧监测';screen='Trends-Custom'},@{text='电压';screen='Trends-Voltage'},@{text='电流';screen='Trends-Current'},@{text='直流';screen='Trends-Dc'},@{text='功率因数';screen='Trends-Factor'}))
        $w.Add($selector)
        switch($Kind){
            'Voltage'{$a=@(1030,1031,1032);$n=@('Ua','Ub','Uc');$u=@('V','V','V')}
            'Current'{$a=@(1033,1034,1035);$n=@('Ia','Ib','Ic');$u=@('A','A','A')}
            'Dc'{$a=@(1566,1244);$n=@('BMS总压','PCS输入');$u=@('V','V')}
            'Factor'{$a=@(1048,1049,1050,1051);$n=@('PFa','PFb','PFc','PF总');$u=@('','','','')}
            default{$a=@(1036,1037,1038,1039,1040,1041,1042,1043,1030,1031,1032);$n=@('Pa','Pb','Pc','P总','Qa','Qb','Qc','Q总','Ua','Ub','Uc');$u=@('kW','kW','kW','kW','kvar','kvar','kvar','kvar','V','V','V')}
        }
        Ref-Chart $w 'trend-custom-main' $a $n $u 152 204 1230 272
        if($Kind -notin @('Voltage','Current','Dc','Factor')){$w[$w.Count-1].properties | Add-Member chartSecondaryUnit 'V'}
        $second='功率因数';$indexes=@(1051);$names=@('功率因数');$units=@('')
    }
    $headingY=if($Kind-eq'Cell'){470}else{507};$panelY=if($Kind-eq'Cell'){509}else{545};$panelHeight=769-$panelY
    $w.Add((New-LanhuImage 'trend-bottom-icon' $(if($Kind-eq'Cell'){'cell-temperature-heading.png'}else{'chart-heading.png'}) 132 $headingY 26 26));$w.Add((Ref-Text 'trend-bottom-title' $second 170 ($headingY-2) 600 30 18));$w.Add((Ref-Panel 'trend-bottom-panel' 132 $panelY 1271 $panelHeight));Ref-Chart $w 'trend-second' $indexes $names $units 152 ($panelY+16) 1230 ($panelHeight-28)
    if($Kind-eq'Cell'){$series=@($w[$w.Count-1].properties.chartSeriesJson|ConvertFrom-Json);$series[1].color='#129AF5';$w[$w.Count-1].properties.chartSeriesJson=ConvertTo-Json -InputObject $series -Depth 8 -Compress}
    if($Kind-ne'Cell'){$w[$w.Count-1].properties | Add-Member qtHideLegend 'true'}
    return New-RefScreen "Trends-$Kind" $w
}
function Build-RefAlarms {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w 'Info-Alarms' 'Alarms'
    foreach($i in 0..2){$x=132+$i*429;$id=@('today','current','total')[$i];$w.Add((Ref-Panel "alarm-$id-panel" $x 90 414 92));$w.Add((New-LanhuImage "alarm-$id-icon" "alarm-$id.png" ($x+21) 106 60 60));$w.Add((Ref-Text "alarm-$id-title" @('今日告警数','当前告警数','累计告警数')[$i] ($x+99) 118 200 36 18));$count=Ref-Text "alarm-$id-value" '--' ($x+306) 110 92 50 34 '#303438' $true 'Center';if($i-eq1){$count.properties|Add-Member alarmActiveCount 'true'};$w.Add($count)}
    $w.Add((New-LanhuImage 'alarm-heading-icon' 'meter-heading.png' 132 199 26 26));$w.Add((Ref-Text 'alarm-heading' '当前告警数据' 172 198 600 32 18));$w.Add((Ref-Panel 'alarm-data-panel' 132 238 1271 534))
    $w.Add((New-Widget 'lanhu-alarm-table' 'alarmTable' '' 152 252 1230 450 3 @() @() $null ([ordered]@{qtTheme='light';qtAlarmPresentation='lanhu';qtBackgroundColor='#FFFFFF';qtTextColor='#303438';qtFontSize=14})))
    $w.Add((Ref-Button 'alarm-soe' 'SOE 事件' 'Info-Soe' 1220 196 182 32))
    return New-RefScreen 'Info-Alarms' $w
}
function Build-RefSoe {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w 'Info-Soe' 'Alarms'
    Ref-Tabs $w @(@{Id='Active';T='当前告警';Target='Info-Alarms'},@{Id='Soe';T='SOE 事件';Target='Info-Soe'}) 'Soe' 562 80 308
    $w.Add((New-Widget 'soe-history' 'qtSoeTable' '' 152 160 1230 600 3 @() @() $null ([ordered]@{soeDatabase='/opt/modbus-gateway/data/iec103-soe.db'})))
    return New-RefScreen 'Info-Soe' $w
}
function Ref-TopologyCard($Widgets,$Id,$Title,$Source,$Image,$X,$Y,$Storage=$false,$Idle=$false) {
    $lower=$Y -gt 400
    $imageW=if($Storage){158}elseif($Image-eq'diesel.png'){160}else{116}
    $Widgets.Add((New-LanhuImage "$Id-image" $Image ($X+(376-$imageW)/2) $Y $imageW $(if($Image-eq'diesel.png'){127}else{124})))
    $Widgets.Add((Ref-Panel "$Id-panel" $X ($Y+134) 376 $(if($lower){160}else{184}) $(if($Idle){'#EEF1F5'}else{'#FFFFFF'})))
    $Widgets.Add((New-Frame "$Id-heading-bg" $X ($Y+134) 376 42 '#F7F7F7' 1))
    if($Storage -or $lower){
        $Widgets.Add((Ref-Text "$Id-title" $Title ($X+24) ($Y+139) 164 34 18 '#303438' $true 'Right'))
        $Widgets.Add((Ref-Text "$Id-breaker-label" $(if($Storage){'PCS断路器'}else{'断路器'}) ($X+198) ($Y+139) 86 34 14))
        if($Source-eq'Pcs'){Ref-State $Widgets "$Id-breaker" 997 '1' '合闸' '分闸' ($X+288) ($Y+142) 58 28}else{Ref-Value $Widgets "$Id-breaker" $null ($X+288) ($Y+142) 58 28}
    }else{$Widgets.Add((Ref-Text "$Id-title" $Title ($X+10) ($Y+139) 356 34 18 '#303438' $true 'Center'))}
    $m=if($null-ne$Source){Get-LanhuMeterIndexes $Source}else{$null}
    $items=@(@{T='P总';K='TotalP';U='kW'},@{T='Q总';K='TotalQ';U='kvar'},@{T='功率因数';K='Factor';U=''},@{T='频率';K='Frequency';U='Hz'})
    foreach($i in 0..3){$s=$items[$i];$columnX=$X+14+$i*91;$Widgets.Add((Ref-Text "$Id-title-$i" $s.T $columnX ($Y+182) 90 30 14 '#777E85'));Ref-Value $Widgets "$Id-value-$i" $(if($m){$m[$s.K]}else{$null}) $columnX ($Y+214) 90 30 $s.U 17}
    $Widgets.Add((New-Frame "$Id-rule" ($X+16) ($Y+252) 344 1 '#EDF0F3' 1))
    foreach($i in 0..2){$columnX=$X+16+$i*122;if(-not$lower){$Widgets.Add((Ref-Text "$Id-phase-label-$i" "U$(@('a','b','c')[$i])" $columnX ($Y+259) 30 25 13 '#777E85'));Ref-Value $Widgets "$Id-phase-$i" $(if($m){$m.V[$i]}else{$null}) ($columnX+32) ($Y+259) 82 25 'V' 13};$py=$Y+$(if($lower){259}else{285});$Widgets.Add((Ref-Text "$Id-power-label-$i" "P$(@('a','b','c')[$i])" $columnX $py 30 25 13 '#777E85'));Ref-Value $Widgets "$Id-power-$i" $(if($m){$m.P[$i]}else{$null}) ($columnX+32) $py 82 25 'kW' 13}
    if($Storage-and-not$Idle){
        $Widgets.Add((New-Widget "$Id-soc-line" 'qtFillProgress' '' ($X+16) ($Y+251) 344 2 3 @((New-Binding 1569)) @() $null ([ordered]@{qtClass='QProgressBar';qtBackgroundColor='#18CC70';progressMaxValue=100;progressMaxWidth=344;progressMaxHeight=2})))
        Ref-Value $Widgets "$Id-soc" 1569 ($X+250) ($Y+240) 90 20 '%' 12
    }
}
function Ref-SingleBand($Widgets,$Id,$X,$Y,$Width,$Height) {
    $Widgets.Add((New-LanhuPanel $Id $X $Y $Width $Height '#F7F7F7' 1))
    $Widgets.Add((New-Frame "$Id-bottom" $X ($Y+8) $Width ($Height-8) '#F7F7F7' 1))
}
function Ref-SingleSummary($Widgets,$Id,$Meter,$X,$Y,$Width,$Offsets=@(0,138,285),$Dividers=@(99,246)) {
    $items=@(@{T='P总';K='TotalP';U='kW'},@{T='Q总';K='TotalQ';U='kvar'},@{T='功率因数';K='Factor';U=''})
    foreach($i in 0..2){
        $item=$items[$i];$left=$X+$Offsets[$i]
        $cellWidth=if($i-lt2){$Dividers[$i]-$Offsets[$i]-8}else{$Width-$Offsets[$i]}
        $Widgets.Add((Ref-Text "$Id-label-$i" $item.T $left $Y $cellWidth 24 14 '#777E85'))
        Ref-Value $Widgets "$Id-value-$i" $Meter[$item.K] $left ($Y+26) $cellWidth 28 $item.U 18
        $Widgets[$Widgets.Count-1].properties.qtTextAlignment='Left'
        if($i-lt2){$Widgets.Add((New-Frame "$Id-divider-$i" ($X+$Dividers[$i]) ($Y+8) 1 36 '#E0E3E6' 1))}
    }
}
function Ref-SinglePhases($Widgets,$Id,$Meter,$X,$Y,$Width,$Reactive=$false,$Offsets=@(0,137,274)) {
    $rows=@(@{K='V';T='U';U='V'},@{K='P';T='P';U='kW'})
    if($Reactive){$rows+=@{K='Q';T='Q';U='kvar'}}
    $Widgets.Add((New-Frame "$Id-rule" $X ($Y-8) $Width 1 '#E0E3E6' 1))
    for($row=0;$row-lt$rows.Count;$row++){
        $item=$rows[$row]
        foreach($i in 0..2){
            $left=$X+$Offsets[$i];$top=$Y+$row*25
            $cellWidth=if($i-lt2){$Offsets[$i+1]-$Offsets[$i]}else{$Width-$Offsets[$i]}
            $Widgets.Add((Ref-Text "$Id-$row-$i-label" ($item.T+@('a','b','c')[$i]) $left $top 30 24 13 '#777E85'))
            Ref-Value $Widgets "$Id-$row-$i" $Meter[$item.K][$i] ($left+33) $top ($cellWidth-39) 24 $item.U 13
            $Widgets[$Widgets.Count-1].properties.qtTextAlignment='Left'
        }
    }
}
function Ref-SingleBreaker($Widgets,$Id,$Index,$X,$Y) {
    # Use real DI feedback only; missing/bad quality stays neutral, never closed red.
    $bindings=@();$rules=@()
    if(Has-Index $Index){
        $bindings=@((New-Binding $Index))
        $rules=@((New-StateRule 'open' '' '#18C773' $Index 'eq' '0' 10),(New-StateRule 'closed' '' '#F52D59' $Index 'eq' '1' 20))
    }
    $Widgets.Add((New-Widget $Id 'statusCard' '' $X $Y 9 18 5 $bindings $rules $null ([ordered]@{
        qtBackgroundColor='#90979F';qtBorderless='true';qtHideStatusLamp='true';qtCornerRadius=2
        stateLabelVisible='false';unavailableStateLabel=' ';qtFontSize=1;qtTheme='light'
    })))
}
function Build-RefSingleTopology($Widgets) {
    $load=Get-LanhuMeterIndexes 'Through';$grid=Get-LanhuMeterIndexes 'Input';$pcs=Get-LanhuMeterIndexes 'Pcs'
    foreach($item in @(@{Id='load';T='负载侧';M=$load;X=302;Y=176;Image='single-load-meter.png';IX=150;IY=176;IW=145;IH=133},@{Id='grid';T='电源(并机)侧';M=$grid;X=955;Y=169;Image='single-grid-meter.png';IX=807;IY=177;IW=140;IH=132})){
        $id='single-'+$item.Id;$x=$item.X;$y=$item.Y
        $Widgets.Add((New-LanhuImage "$id-image" $item.Image $item.IX $item.IY $item.IW $item.IH))
        $Widgets.Add((Ref-Panel "$id-panel" $x $y 402 159))
        Ref-SingleBand $Widgets "$id-band" $x $y 402 36
        $Widgets.Add((Ref-Text "$id-title" $item.T $x ($y+3) 402 30 18 '#303438' $true 'Center'))
        Ref-SingleSummary $Widgets $id $item.M ($x+23) ($y+44) 357
        Ref-SinglePhases $Widgets $id $item.M ($x+23) ($y+108) 357
    }
    $Widgets.Add((New-Frame 'single-ac-bus' 129 409 1262 6 '#2B6396' 1))
    foreach($line in @(@{Id='load';X=219;Y=342;H=67},@{Id='grid';X=871;Y=319;H=90},@{Id='pcs';X=499;Y=416;H=60},@{Id='battery';X=499;Y=601;H=75})){
        $Widgets.Add((New-Frame "single-line-$($line.Id)" $line.X $line.Y 3 $line.H '#A2BED6' 1))
    }
    Ref-SingleBreaker $Widgets 'single-load-breaker' 999 216 379
    Ref-SingleBreaker $Widgets 'single-grid-breaker' 998 868 357
    Ref-SingleBreaker $Widgets 'single-pcs-breaker' 997 496 447
    # No confirmed DC-contactor feedback or independent bus meter exists in this profile.
    $Widgets.Add((New-Frame 'single-battery-breaker-reserved' 496 638 9 18 '#90979F' 5))
    $Widgets.Add((Ref-Text 'single-bus-title' 'AC母线' 148 423 160 30 18 '#2B6396' $true))
    foreach($i in 0..3){$y=460+$i*31;$Widgets.Add((Ref-Text "single-bus-$i-label" @('频率','Ua','Ub','Uc')[$i] 148 $y 34 28 13 '#2B6396'));Ref-Value $Widgets "single-bus-$i" $null 183 $y 105 28 $(if($i-eq0){'Hz'}else{'V'}) 14}
    $Widgets.Add((New-LanhuImage 'single-pcs-image' 'single-pcs.png' 420 490 160 108))
    $Widgets.Add((New-LanhuImage 'single-battery-image' 'single-battery.png' 420 691 160 104))
    $Widgets.Add((Ref-Panel 'single-pcs-panel' 627 429 730 223))
    Ref-SingleBand $Widgets 'single-pcs-band' 627 429 730 39
    $Widgets.Add((Ref-Text 'single-pcs-title' 'PCS' 880 434 43 28 18 '#303438' $true))
    $bindings=@();$rules=@()
    if((Has-Index 1216)-and(Has-Index 1217)){
        foreach($entry in @(@{I=1216;Other=1217;T='并网';C='#18C773'},@{I=1217;Other=1216;T='离网';C='#F52D59'})){
            $bindings+=New-Binding $entry.I
            $rule=New-StateRule ([string]$entry.I) $entry.T $entry.C $entry.I 'eq' '1' $entry.I
            $rule.conditions+=(New-StateRule '' '' '' $entry.Other 'eq' '0' 0).conditions
            $rules+=$rule
        }
    }
    $Widgets.Add((New-Widget 'single-pcs-grid-state' 'statusCard' '' 928 436 46 24 5 $bindings $rules $null ([ordered]@{qtBackgroundColor='#90979F';qtCornerRadius=2;qtFontSize=17;qtTextColor='#FFFFFF';qtTheme='light';qtHideStatusLamp='true';qtBorderless='true';qtVerticalAlignment='Center';qtTextAlignment='Center';unavailableStateLabel='--'})))
    Monitor-Heading $Widgets 'single-pcs-ac' 'AC' 'monitor-measurement.png' 649 478
    Monitor-Heading $Widgets 'single-pcs-dc' 'DC' 'monitor-measurement.png' 1099 478
    Ref-SingleSummary $Widgets 'single-pcs-ac' $pcs 649 506 390 @(0,162,332) @(110,280)
    Ref-SinglePhases $Widgets 'single-pcs-ac' $pcs 649 570 390 $true @(0,151,304)
    $Widgets.Add((New-Frame 'single-pcs-divider' 1072 479 1 162 '#E0E3E6' 1))
    $Widgets.Add((New-Frame 'single-pcs-dc-divider' 1210 513 1 36 '#E0E3E6' 1))
    foreach($item in @(@{Id='voltage';T='直流电压';I=1244;U='V';X=1099},@{Id='power';T='直流功率';I=1243;U='kW';X=1260})){
        Monitor-Field $Widgets "single-pcs-dc-$($item.Id)" $item.T $item.I $item.U $item.X 506 96 18
    }
    $Widgets.Add((Ref-Panel 'single-battery-panel' 627 700 156 86))
    Ref-SingleBand $Widgets 'single-battery-band' 627 700 156 37
    $Widgets.Add((Ref-Text 'single-battery-title' '电池组' 627 704 156 30 18 '#303438' $true 'Center'))
    $Widgets.Add((Ref-Text 'single-battery-soc-label' 'SOC' 650 748 48 28 14 '#777E85'))
    Ref-Value $Widgets 'single-battery-soc' 1569 707 748 66 28 '%' 18
    foreach($field in $Widgets|Where-Object {$_.widgetId-like'single-*'-and$_.type-eq'qtValue'}){$field.properties.qtFontWeight='Normal'}
}
function Build-RefTopology($Single=$false) {
    $id=if($Single){'Topology-Single'}else{'Topology'};$w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $id 'Topology'
    Ref-Tabs $w @(@{Id='Parallel';T='并机拓扑';Target='Topology'},@{Id='Single';T='单机拓扑';Target='Topology-Single'}) $(if($Single){'Single'}else{'Parallel'}) 562 80 308
    if($Single){Build-RefSingleTopology $w;return New-RefScreen $id $w}
    Ref-TopologyCard $w 'topology-transformer' '变压器 - 防逆流表' 'Output' 'meter.png' 120 141
    Ref-TopologyCard $w 'topology-grid' '并网点' 'Input' 'meter.png' 540 141
    Ref-TopologyCard $w 'topology-diesel' '柴油发电机' $null 'diesel.png' 120 497
    if(Has-Index 1039){foreach($flow in @((New-EnergyFlow 'topology-flow-grid' 1039 'positive' 412 176 220 52))){$flow.properties.flowColor='#A2BED6';$flow.properties.flowIdleColor='#A2BED6';$flow.properties|Add-Member qtTextColor '#2599D6';$flow.properties|Add-Member flowSolidLine 'true';$flow.properties|Add-Member qtHideFlowValue 'true';$w.Add($flow)}}
    $localTags=[ordered]@{}
    $pcs=Get-LanhuMeterIndexes 'Pcs'
    $breaker=@($lanhuBreakers|Where-Object Name -eq '储能断路器'|Select-Object -First 1)
    $localIndexes=[ordered]@{p=$pcs.TotalP;q=$pcs.TotalQ;pa=$pcs.P[0];pb=$pcs.P[1];pc=$pcs.P[2];ua=$pcs.V[0];ub=$pcs.V[1];uc=$pcs.V[2];factor=$pcs.Factor;frequency=$pcs.Frequency;soc=1569;online=1399;bmsOnline=3999;breaker=$(if($breaker.Count){$breaker[0].Position}else{$null})}
    foreach($key in $localIndexes.Keys){if($null-ne$localIndexes[$key]-and(Has-Index $localIndexes[$key])){$localTags[$key]=(Tag-At $localIndexes[$key]).tagId}}
    $w.Add((New-Widget 'topology-vehicles' 'qtParallelTopology' '' 120 141 1230 653 4 @() @() $null ([ordered]@{qtBackgroundColor='transparent';localTelemetryTagsJson=(ConvertTo-Json -InputObject $localTags -Compress)})))
    return New-RefScreen $id $w
}
function Build-RefBlank($Id,$Section) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $Id $Section
    return New-RefScreen $Id $w
}

function Get-LanhuScreens {
    foreach($screenId in $XMindV2ExpectedScreens){
        $screen=@{screenId=$screenId}
        switch -Regex ($screen.screenId){
            '^Guide-(Setup|Deploy)$' { Build-RefGuide $Matches[1];break }
            '^Topology$' { Build-RefTopology;break }
            '^Trends-(.+)$' { Build-RefTrends $Matches[1];break }
            '^Info-Alarms$' { Build-RefAlarms;break }
            '^Meters-Overview$' { Build-RefMeter $screen.screenId 'Output';break }
            '^Meters-Electrical-(.+)$|^Meters-Power-(.+)$' { $source=if($Matches[1]){$Matches[1]}else{$Matches[2]};Build-RefMeter $screen.screenId $source;break }
            '^Control-Pcs$' { Build-RefPcs;break }
            '^Strategy-Overview$' { Build-RefStrategy;break }
            '^Devices-Pcs$' { Build-MonitorPcs;break }
            '^Devices-Bms$' { Build-MonitorBms;break }
            '^Devices-Thermal$|^Devices-Fire$' { Build-MonitorThermal $screen.screenId;break }
            '^Info-Messages$' { Build-MonitorNetwork $screen.screenId;break }
            '^Meters-Energy$' { Build-RefBlank $screen.screenId 'Meters';break }
            '^Control-Dido$|^Strategy-(Charge|Grid|Balance)$' { Build-RefBlank $screen.screenId 'Control';break }
            '^Maintenance$|^Vehicle-Control$' { Build-RefBlank $screen.screenId 'Settings';break }
            default { Build-RefBlank $screen.screenId 'Settings' }
        }
    }
    Build-RefTopology $true
    Build-RefSoe
    foreach($kind in @('Bms','Thermal','Mccb','Diesel')){Build-RefDeviceControl $kind}
    for($page=2;$page-le[math]::Ceiling($lanhuBreakers.Count/9);$page++){Build-RefDeviceControl 'Mccb' $page}
    foreach($meter in $lanhuMeters){if($meter.Target-notin$XMindV2ExpectedScreens){Build-RefMeter $meter.Target $meter.Source}}
    Get-MonitorScreens
}
