# The approved boards use a 1440 x 810 canvas. Coordinates below are design pixels.
function New-RefScreen($Id, $Widgets) {
    $screen=New-Screen $Id $Id $Widgets
    $screen.width=1440; $screen.height=810
    return $screen
}
function Ref-Panel($Id,$X,$Y,$W,$H,$Color='#FFFFFF') { New-LanhuPanel $Id $X $Y $W $H $Color }
function Ref-Text($Id,$Text,$X,$Y,$W,$H,$Size=16,$Color='#303438',$Bold=$false,$Align='Left') {
    $widget=New-Text $Id $Text $X $Y $W $H $Size $Color $Align $Bold
    $widget.properties | Add-Member qtVerticalAlignment 'Center' -Force
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
function Ref-Chrome($Widgets,$Id,$Active) {
    $Widgets.Add((New-Frame 'lanhu-background' 0 0 1440 810 '#E5E9EC' -100))
    $clock=Ref-Text 'lanhu-clock' '' 140 23 300 30 16
    $clock.properties | Add-Member qtClockFormat 'yyyy-MM-dd  HH:mm:ss'
    $Widgets.Add($clock)
    $Widgets.Add((New-LanhuImage 'lanhu-logo' 'logo.png' 472 18 116 36))
    $Widgets.Add((Ref-Text 'lanhu-brand' $projectDisplayName 600 17 208 34 23 '#25292D' $true))
    $Widgets.Add((New-Frame 'lanhu-brand-separator' 792 26 1 16 '#C3CBD1' 1))
    $Widgets.Add((Ref-Text 'lanhu-machine' $MachineCode 816 22 355 28 18))
    $Widgets.Add((New-Frame 'lanhu-header-rule' 132 60 1290 1 '#D4DADF' 1))
    $Widgets.Add((Ref-Button 'lanhu-login' '登  录' 'Control-Pcs' 1272 8 108 48 $true 24))
    $nav=@(@{Id='Guide';T='引导';Target='Guide-Setup'},@{Id='Meters';T='仪表';Target='Meters-Overview'},@{Id='Topology';T='拓扑';Target='Topology'},@{Id='Trends';T='曲线';Target='Trends-Custom'},@{Id='Alarms';T='告警';Target='Info-Alarms'},@{Id='Monitor';T='监测';Target='Devices-Pcs'},@{Id='Control';T='控制';Target='Control-Pcs'},@{Id='Settings';T='设置';Target='Maintenance'})
    for($i=0;$i-lt8;$i++) {
        $n=$nav[$i];$y=42+$i*93
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
    Ref-Tabs $Widgets @(@{Id='Output';T='位置1';Target='Meters-Overview'},@{Id='Input';T='位置2';Target='Meters-Electrical-Input'},@{Id='Pcs';T='位置3';Target='Meters-Electrical-Pcs'},@{Id='Through';T='位置4';Target='Meters-Power-Through'},@{Id='Energy';T='位置5';Target='Meters-Energy'}) $Active 376 80 690
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
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $Id 'Meters';Ref-MeterTabs $w $Source
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
function Ref-ControlChrome($Widgets,$Kind) {
    Ref-Chrome $Widgets "Control-$Kind" 'Control'
    Ref-Tabs $Widgets @(@{Id='Device';T='设备控制';Target='Control-Pcs'},@{Id='Strategy';T='策略控制';Target='Strategy-Overview'}) 'Device' 562 80 308
    $Widgets.Add((Ref-Panel 'control-body' 132 155 1271 614))
    $Widgets.Add((New-Frame 'control-tabs-band' 132 155 1271 51 '#F7F7F7' 1))
    $tabs=@(@{Id='Pcs';T='PCS';X=154},@{Id='Bms';T='BMS';X=272},@{Id='Thermal';T='TMS';X=382},@{Id='Mccb';T='MCCB';X=494},@{Id='Diesel';T='柴油发电机';X=608})
    foreach($t in $tabs){
        $b=Ref-Button "control-tab-$($t.Id)" $t.T "Control-$($t.Id)" $t.X 160 $(if($t.Id-eq'Diesel'){126}else{66}) 42
        $b.properties.qtTransparent=$true;$b.properties.qtTextColor=if($t.Id-eq$Kind){'#129AF5'}else{'#303438'}
        $Widgets.Add($b)
        if($t.Id-eq$Kind){$Widgets.Add((New-Frame 'control-active-line' $t.X 203 $(if($t.Id-eq'Diesel'){86}else{66}) 3 '#129AF5' 21))}
    }
    $Widgets.Add((New-LanhuImage 'control-mode-icon' 'control-mode.png' 162 250 66 66))
    $Widgets.Add((Ref-Text 'control-mode-heading' '模式控制' 252 248 420 32 20 '#303438' $true))
    $Widgets.Add((Ref-Text 'control-mode-title' '当前模式：' 252 289 94 26 18 '#777E85'))
    $modeIndex=if($Kind-eq'Pcs'){1310}elseif($Kind-eq'Thermal'){164}else{$null}
    Ref-Value $Widgets 'control-mode-value' $modeIndex 346 289 200 26 '' 18
    $b=Ref-Button 'control-switch-mode' $(if($Kind-eq'Diesel'){'停机'}else{'切换模式'}) '' 1222 242 150 48 $true 24
    $b.properties | Add-Member qtIconFile $(if($Kind-eq'Diesel'){'assets/lanhu-assets/command-stop.png'}else{'assets/lanhu-assets/command-mode.png'})
    if($Kind-eq'Diesel'){$b.properties.qtBackgroundColor='#F52D59'}
    # Mode enum commands remain reserved until the vendor's allowed values are confirmed.
    $Widgets.Add($b)
}
function Ref-Command($Widgets,$Id,$Title,$Index,$Value,$X,$Y,$Color='#2599D6') {
    $b=Ref-Button $Id $Title '' $X $Y 82 36 $false 18
    $b.properties.qtBackgroundColor='#F2FAFF';$b.properties.qtBorderColor=$Color;$b.properties.qtTextColor=$Color;$b.properties.qtFontSize=14
    $icon=if($Title-in@('停机','停止','快速停机')){'stop'}elseif($Title-eq'运行'-or$Title-eq'启用'){'run'}elseif($Title-eq'复位'){'reset'}else{''}
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
    Ref-Command $w 'control-pcs-start' '运行' 1201 '1' 542 417
    Ref-Command $w 'control-pcs-reset' '复位' 1200 '1' 645 417
    $w.Add((Ref-Text 'pcs-grid-current-title' '当前状态：' 800 419 76 32 14 '#777E85'))
    if((Has-Index 1216)-and(Has-Index 1217)){
        $w.Add((New-Widget 'pcs-grid-current' 'statusCard' '' 876 422 46 24 5 @((New-Binding 1216),(New-Binding 1217)) @((New-StateRule 'grid' '并网' '#18C773' 1216 'eq' '1' 10),(New-StateRule 'offgrid' '离网' '#F52D59' 1217 'eq' '1' 20)) $null ([ordered]@{qtBackgroundColor='transparent';qtFontSize=16;qtTextColor='#FFFFFF';qtTheme='light';qtBorderless='true';qtVerticalAlignment='Center';qtTextAlignment='Center';qtHideStatusLamp='true';unavailableStateLabel='--'})))
    }else{Ref-Value $w 'pcs-grid-current' $null 876 419 46 32}
    Ref-Command $w 'pcs-grid-connect-reserved' '并网' $null '' 1158 417
    Ref-Command $w 'pcs-grid-disconnect-reserved' '离网' $null '' 1264 417 '#EE5078'
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
function Ref-DeviceTile($Widgets,$Id,$Title,$Icon,$Command,$X,$Y,$Color='#2599D6') {
    $panel=Ref-Panel "$Id-panel" $X $Y 388 84
    $panel.properties|Add-Member qtBorderColor '#E6E6E6';$Widgets.Add($panel)
    $Widgets.Add((New-LanhuImage "$Id-icon" $Icon ($X+14) ($Y+18) 42 42))
    $Widgets.Add((Ref-Text "$Id-title" $Title ($X+74) ($Y+10) 178 28 16))
    $Widgets.Add((Ref-Text "$Id-status-title" '当前状态：' ($X+74) ($Y+40) 76 28 14 '#777E85'))
    Ref-Value $Widgets "$Id-feedback" $null ($X+150) ($Y+40) 46 28
    Ref-Command $Widgets "$Id-reserved" $Command $null '' ($X+280) ($Y+24) $Color
    if($Command-eq'快速停机'){$Widgets[$Widgets.Count-1].geometry.width=112;$Widgets[$Widgets.Count-1].geometry.x=$X+258}
}
function Build-RefDeviceControl($Kind) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-ControlChrome $w $Kind
    switch($Kind){
        'Bms' {
            Ref-ControlPanel $w 'bms-control' '运行状态' 162 350 592 123
            $w.Add((Ref-Text 'bms-status-title' '当前状态：' 182 419 76 32 14 '#777E85'));Ref-Value $w 'bms-status' (Find-PointIndex -Names @('BMS工作状态','BMS 工作状态')) 258 419 124 32
            # The vendor's power-command enum has not been confirmed; reserve named actions.
            Ref-Command $w 'bms-reset-reserved' '复位' $null '' 438 417
            Ref-Command $w 'bms-power-on-reserved' '上电' $null '' 542 417
            Ref-Command $w 'bms-power-off-reserved' '下电' $null '' 645 417
        }
        'Thermal' {
            foreach($i in 0..1){$x=162+$i*618;Ref-ControlPanel $w "tms-control-$i" '液冷控制' $x 350 592 123;$w.Add((Ref-Text "tms-status-title-$i" '当前状态：' ($x+20) 419 76 32 14 '#777E85'));Ref-Value $w "tms-status-$i" $(if($i-eq0){163}else{$null}) ($x+96) 419 124 32;Ref-Command $w "tms-stop-$i" '停止' $null '0' ($x+276) 417 '#EE5078';Ref-Command $w "tms-reset-$i" '复位' $null '' ($x+380) 417;Ref-Command $w "tms-enable-$i" '启用' $null '1' ($x+484) 417}
            Ref-ControlPanel $w 'tms-temperature' '液冷温度控制' 162 497 1210 172
            Ref-SettingRow $w 'tms-cool-start-reserved' '制冷启动温度(℃)' $null 180 560 540
            Ref-SettingRow $w 'tms-heat-start-reserved' '制热启动温度(℃)' $null 180 612 540
            Ref-SettingRow $w 'tms-cool-return-reserved' '制冷回差温度(℃)' $null 792 560 540
            Ref-SettingRow $w 'tms-heat-return-reserved' '制热回差温度(℃)' $null 792 612 540
        }
        'Mccb' {
            foreach($i in 0..8){$x=162+($i%3)*412;$y=350+[math]::Floor($i/3)*100;Ref-DeviceTile $w "mccb-$i" ('断路器{0:00}'-f($i+1)) 'control-breaker.png' '合闸' $x $y}
        }
        'Diesel' {
            $items=@(@{Id='diesel-start';T='遥控快速停机';Icon='control-diesel-start.png';Command='快速停机';Color='#EE5078'},@{Id='diesel-breaker';T='遥控市电合分闸';Icon='control-breaker.png';Command='合闸';Color='#2599D6'},@{Id='diesel-mode';T='遥控发电合分闸';Icon='control-mode-small.png';Command='合闸';Color='#2599D6'},@{Id='diesel-reset';T='遥控手/自动';Icon='control-reset.png';Command='自动';Color='#2599D6'})
            for($i=0;$i-lt4;$i++){$s=$items[$i];$x=162+($i%3)*412;$y=350+[math]::Floor($i/3)*100;Ref-DeviceTile $w $s.Id $s.T $s.Icon $s.Command $x $y $s.Color}
        }
    }
    return New-RefScreen "Control-$Kind" $w
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
    return New-RefScreen 'Info-Alarms' $w
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
function Build-RefTopology($Single=$false) {
    $id=if($Single){'Topology-Single'}else{'Topology'};$w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $id 'Topology'
    Ref-Tabs $w @(@{Id='Parallel';T='并机拓扑';Target='Topology'},@{Id='Single';T='单机拓扑';Target='Topology-Single'}) $(if($Single){'Single'}else{'Parallel'}) 562 80 308
    if($Single){return New-RefScreen $id $w}
    Ref-TopologyCard $w 'topology-transformer' '变压器 - 防逆流表' 'Output' 'meter.png' 120 141
    Ref-TopologyCard $w 'topology-grid' '并网点' 'Input' 'meter.png' 540 141
    Ref-TopologyCard $w 'topology-storage-1' '1#储能车' 'Pcs' 'storage.png' 954 141 $true
    Ref-TopologyCard $w 'topology-diesel' '柴油发电机' $null 'diesel.png' 120 497
    if(-not$Single){Ref-TopologyCard $w 'topology-storage-3' '3#储能车' $null 'storage-idle.png' 540 497 $true $true;Ref-TopologyCard $w 'topology-storage-2' '2#储能车' $null 'storage-idle.png' 954 497 $true $true}
    foreach($flow in @((New-EnergyFlow 'topology-flow-grid' 1039 'positive' 412 176 220 52),(New-EnergyFlow 'topology-flow-pcs' 1230 'negative' 816 176 240 52))){$flow.properties.flowColor='#A2BED6';$flow.properties.flowIdleColor='#A2BED6';$flow.properties|Add-Member qtTextColor '#2599D6';$flow.properties|Add-Member flowSolidLine 'true';$flow.properties|Add-Member qtHideFlowValue 'true';$w.Add($flow)}
    $w.Add((New-Widget 'topology-side-link' 'qtPath' '' 1254 204 96 364 1 @() @() $null ([ordered]@{qtStrokeColor='#A2BED6';qtStrokeWidth=3;qtPathPointsJson='[[0,0],[78,0],[94,16],[94,260],[78,276],[-694,276],[-704,286],[-704,350],[-714,362],[-866,362]]'})))
    $w.Add((Ref-Text 'topology-side-power-label' '输出功率' 1353 387 84 32 14 '#777E85'))
    Ref-Value $w 'topology-side-power' $null 1350 416 86 32 'kW' 18
    return New-RefScreen $id $w
}
function Build-RefBlank($Id,$Section) {
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $Id $Section
    return New-RefScreen $Id $w
}

function Get-LanhuScreens {
    foreach($screen in @(Get-XMindV2Screens)){
        switch -Regex ($screen.screenId){
            '^Guide-(Setup|Deploy)$' { Build-RefGuide $Matches[1];break }
            '^Topology$' { Build-RefTopology;break }
            '^Trends-(.+)$' { Build-RefTrends $Matches[1];break }
            '^Info-Alarms$' { Build-RefAlarms;break }
            '^Meters-Overview$' { Build-RefMeter $screen.screenId 'Output';break }
            '^Meters-Electrical-(.+)$|^Meters-Power-(.+)$' { $source=if($Matches[1]){$Matches[1]}else{$Matches[2]};Build-RefMeter $screen.screenId $source;break }
            '^Control-Pcs$' { Build-RefPcs;break }
            '^Strategy-Overview$' { Build-RefStrategy;break }
            '^Devices-|^Info-Messages' { Build-RefBlank $screen.screenId 'Monitor';break }
            '^Meters-Energy$' { Build-RefBlank $screen.screenId 'Meters';break }
            '^Control-Dido$|^Strategy-(Charge|Grid|Balance)$' { Build-RefBlank $screen.screenId 'Control';break }
            '^Maintenance$|^Vehicle-Control$' { Build-RefBlank $screen.screenId 'Settings';break }
            default { Build-RefBlank $screen.screenId 'Settings' }
        }
    }
    Build-RefTopology $true
    foreach($kind in @('Bms','Thermal','Mccb','Diesel')){Build-RefDeviceControl $kind}
}
