# Monitor boards are normalized from the user's 1920 x 1080 exports.
function New-MonitorScreen($Id,$Widgets) {
    foreach($widget in $Widgets){
        if($widget.type-eq'qtValue'){
            $widget.properties.qtFontWeight='Normal'
            if($widget.properties.valueSuffix){$widget.properties.valueSuffix=' '+$widget.properties.valueSuffix}
        }
    }
    return New-RefScreen $Id $Widgets
}
function Monitor-Chrome($Widgets,$Id,$Active='Device',$Kind='') {
    Ref-Chrome $Widgets $Id 'Monitor'
    Ref-Tabs $Widgets @(@{Id='Network';T='网络监测';Target='Monitor-Network'},@{Id='Device';T='设备监测';Target='Devices-Emu'},@{Id='Strategy';T='策略监测';Target='Monitor-Strategy'}) $Active 560 80 440
    if($Active-eq'Device') {
        $Widgets.Add((Ref-Panel 'monitor-body' 132 155 1271 633))
        $Widgets.Add((New-Frame 'monitor-tab-band' 132 155 1271 51 '#F7F7F7' 1))
        $tabs=@(@{Id='Emu';T='EMU';X=158;W=64},@{Id='Pcs';T='PCS';X=272;W=64},@{Id='Bms';T='BMS';X=385;W=64},@{Id='Thermal';T='TMS';X=499;W=64},@{Id='Mccb';T='MCCB';X=613;W=78},@{Id='Meters';T='电表';X=735;W=64},@{Id='Diesel';T='柴油发电机';X=843;W=126})
        foreach($t in $tabs) {
            $b=Ref-Button "monitor-tab-$($t.Id)" $t.T "Devices-$($t.Id)" $t.X 160 $t.W 43
            $b.properties.qtTransparent=$true;$b.properties.qtTextColor=if($Kind-eq$t.Id){'#129AF5'}else{'#303438'}
            $Widgets.Add($b)
            if($Kind-eq$t.Id){$Widgets.Add((New-Frame 'monitor-tab-line' $t.X 203 $t.W 3 '#129AF5' 21))}
        }
    }
}
function Monitor-Box($Widgets,$Id,$X,$Y,$W,$H,$Title='') {
    $p=Ref-Panel "$Id-panel" $X $Y $W $H;$p.properties|Add-Member qtBorderColor '#D9DDE2';$Widgets.Add($p)
    if($Title){
        $Widgets.Add((New-Frame "$Id-title-band" ($X+1) ($Y+1) ($W-2) 43 '#F7F7F7' 1))
        $Widgets.Add((Ref-Text "$Id-title" $Title $X ($Y+4) $W 36 18 '#303438' $true 'Center'))
    }
}
function Monitor-Heading($Widgets,$Id,$Title,$Icon,$X,$Y) {
    $Widgets.Add((New-LanhuImage "$Id-icon" $Icon $X ($Y+2) 22 22))
    $Widgets.Add((Ref-Text "$Id-heading" $Title ($X+34) $Y 250 28 18 '#303438' $true))
}
function Monitor-Field($Widgets,$Id,$Title,$Index,$Unit,$X,$Y,$W,$Size=18) {
    $multiLine=$Title.Contains("`n")
    $labelHeight=if($multiLine){42}else{30}
    $Widgets.Add((Ref-Text "$Id-label" $Title $X $Y $W $labelHeight $(if($multiLine){14}else{16}) '#777E85'))
    Ref-Value $Widgets $Id $Index $X ($Y+$labelHeight+4) $W 28 $Unit $Size
    $Widgets[$Widgets.Count-1].properties.qtTextAlignment='Left'
}
function Monitor-State($Widgets,$Id,$Index,$Entries,$X,$Y,$W=100,$H=30,$Badge=$false) {
    if($null-eq$Index-or-not(Has-Index $Index)){Ref-Value $Widgets $Id $null $X $Y $W $H;return}
    $rules=@();$priority=0
    foreach($e in $Entries){$priority++;$rules+=New-StateRule "$Id-$priority" $e.T $e.C $Index $(if($e.ContainsKey('Op')){$e.Op}else{'eq'}) ([string]$e.V) $priority}
    $s=New-Widget $Id 'statusCard' '' $X $Y $W $H 5 @((New-Binding $Index)) $rules $null ([ordered]@{qtBackgroundColor='transparent';qtFontSize=16;qtTheme='light';qtTextColor='#303438';qtStateTextColor='true';qtBorderless='true';qtHideStatusLamp='true';qtVerticalAlignment='Center';qtTextAlignment=$(if($Badge){'Center'}else{'Left'});unavailableStateLabel='--';stateColorMode='indicator'})
    $Widgets.Add($s)
}
function Monitor-Binary($Widgets,$Id,$Index,$Good,$Bad,$X,$Y,$W=100,$Normal='0') {
    $other=if($Normal-eq'0'){'1'}else{'0'}
    Monitor-State $Widgets $Id $Index @(@{V=$Normal;T=$Good;C='#18C773'},@{V=$other;T=$Bad;C='#F52D59'}) $X $Y $W
}
function Monitor-SignalField($Widgets,$Id,$Title,$Index,$Good,$Bad,$X,$Y,$W,$Normal='0') {
    $Widgets.Add((Ref-Text "$Id-label" $Title $X $Y $W 30 16 '#777E85'))
    Monitor-Binary $Widgets $Id $Index $Good $Bad $X ($Y+34) $W $Normal
}
function Monitor-Alarms($Widgets,$Id,$Indexes,$X,$Y,$W,$H) {
    $devices=@($Indexes|Where-Object {$null-ne$_-and(Has-Index $_)}|ForEach-Object {(Tag-At $_).deviceId}|Sort-Object -Unique)
    $Widgets.Add((New-Widget $Id 'alarmTable' '' $X $Y $W $H 5 @() @() $null ([ordered]@{qtTheme='light';qtAlarmPresentation='summary';qtAlarmDeviceIdsJson=(ConvertTo-Json -InputObject $devices -Compress);qtBackgroundColor='#FFFFFF';qtFontSize=14})))
}
function Monitor-MetricTile($Widgets,$Id,$Title,$Index,$Unit,$Icon,$X,$Y,$W,$H=70,$Color='#E6F2F8') {
    $Widgets.Add((Ref-Panel "$Id-tile" $X $Y $W $H $Color))
    $Widgets.Add((New-LanhuImage "$Id-icon" $Icon ($X+15) ($Y+15) 42 42))
    $Widgets.Add((Ref-Text "$Id-label" $Title ($X+67) ($Y+8) ($W-77) 26 16 '#777E85'))
    Ref-Value $Widgets $Id $Index ($X+67) ($Y+37) ($W-77) 26 $Unit 16
    $Widgets[$Widgets.Count-1].properties.qtTextAlignment='Left'
}
function Build-MonitorNetwork($Id='Monitor-Network') {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w $Id 'Network'
    $items=@(
        @(@{T='链路状态';I=920000003;State=$true},@{T='当前出口地址';I=$null},@{T='信号';I=920000005;U='%'},@{T='本次连接上行流量';I=920000007;U='MiB'},@{T='本次连接下行流量';I=920000006;U='MiB'},@{T='实时上行速率';I=920000009;U='KiB/s'},@{T='实时下行速率';I=920000008;U='KiB/s'}),
        @(@{T='链路状态';I=$null},@{T='LAN口号';I=$null},@{T='IP地址';I=$null},@{T='累计上行流量';I=$null},@{T='累计下行流量';I=$null},@{T='实时上行速率';I=$null},@{T='实时下行速率';I=$null}),
        @(@{T='链路状态';I=$null},@{T='卫星数量';I=$null},@{T='坐标信息';I=$null},@{T='时间信息';I=$null})
    )
    foreach($col in 0..2){
        $left=132+$col*429;$w.Add((Ref-Panel "network-panel-$col" $left 159 414 610))
        $w.Add((New-LanhuImage "network-icon-$col" "monitor-network-$col.png" ($left+21) 181 37 37))
        $w.Add((Ref-Text "network-title-$col" @('4G 网络','有线信号','GPS')[$col] ($left+76) 182 310 38 22 '#303438' $true))
        $w.Add((New-Frame "network-rule-$col" ($left+18) 241 370 1 '#E4E7EA' 1))
        for($row=0;$row-lt$items[$col].Count;$row++){
            $e=$items[$col][$row];$top=269+$row*48
            $w.Add((New-Widget "network-number-bg-$col-$row" 'qtEllipse' '' ($left+21) $top 27 27 1 @() @() $null ([ordered]@{qtBackgroundColor='#F0F7FC'})))
            $w.Add((Ref-Text "network-number-$col-$row" ($row+1) ($left+21) $top 27 27 16 '#303438' $false 'Center'))
            $w.Add((Ref-Text "network-label-$col-$row" $e.T ($left+56) $top 176 30 16))
            if($e.ContainsKey('State')){Ref-State $w "network-value-$col-$row" $e.I '1' '正常' '异常' ($left+319) $top 78 30}
            else{Ref-Value $w "network-value-$col-$row" $e.I ($left+238) $top 158 30 $(if($e.ContainsKey('U')){$e.U}else{''}) 16}
        }
    }
    return New-MonitorScreen $Id $w
}
function Build-MonitorEmu {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Devices-Emu' 'Device' 'Emu'
    $w.Add((New-LanhuImage 'emu-icon' 'monitor-emu.png' 162 233 45 45));$w.Add((Ref-Text 'emu-title' 'EMU' 225 237 200 35 22 '#303438' $true))
    $w.Add((Ref-Panel 'emu-metadata' 162 296 1210 91 '#F7F7F7'))
    $titles=@('序列号','型号','软件版本','远方/就地','云平台','第三方平台')
    foreach($i in 0..5){$left=185+$i*200;$w.Add((New-LanhuImage "emu-info-icon-$i" "monitor-emu-info-$i.png" $left 317 48 48));Monitor-Field $w "emu-info-$i" $titles[$i] $null '' ($left+60) 305 136 18}
    foreach($i in 0..3){$left=162+$i*309;Monitor-Box $w "emu-resource-$i" $left 418 286 120;$w.Add((New-LanhuImage "emu-resource-icon-$i" "monitor-emu-resource-$i.png" ($left+38) 449 54 54));Monitor-Field $w "emu-resource-$i" @('CPU状态','内存状态','硬盘状态','运行温度')[$i] $null '' ($left+114) 438 165 26}
    return New-MonitorScreen 'Devices-Emu' $w
}
function Build-MonitorPcs {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Devices-Pcs' 'Device' 'Pcs'
    Monitor-Box $w 'pcs-overview' 162 222 1210 233 '直流/交流'
    Monitor-Heading $w 'pcs-measurements' '遥测信息' 'monitor-measurement.png' 185 276
    $temps=@(@{T='IGBT 1温度';I=1271},@{T='IGBT 2温度';I=1272},@{T='IGBT 3温度';I=1273},@{T='IGBT 4温度';I=1274},@{T='进风温度';I=1270},@{T='排风温度';I=1276})
    foreach($i in 0..5){$e=$temps[$i];Monitor-Field $w "pcs-temp-$i" $e.T $e.I '℃' (185+($i%3)*106) (305+[math]::Floor($i/3)*75) 100}
    $w.Add((New-Frame 'pcs-divider-1' 497 293 1 149 '#E5E8EB' 1));$w.Add((New-Frame 'pcs-divider-2' 938 293 1 149 '#E5E8EB' 1))
    Monitor-Heading $w 'pcs-signals' '遥信信息' 'monitor-signal.png' 516 276
    Monitor-SignalField $w 'pcs-mode' '控制模式' 1214 '远程' '就地' 519 305 98 '1'
    Monitor-SignalField $w 'pcs-online' '在线状态' 1399 '在线' '离线' 625 305 98 '1'
    $w.Add((Ref-Text 'pcs-run-label' '运行状态' 731 305 98 30 14 '#777E85'))
    $w.Add((New-Widget 'pcs-run' 'statusCard' '' 731 342 98 30 5 @((New-Binding 1209),(New-Binding 1210),(New-Binding 1211)) @((New-StateRule 'stop' '停机' '#90979F' 1209 'eq' '1' 10),(New-StateRule 'standby' '待机' '#F5A623' 1210 'eq' '1' 20),(New-StateRule 'run' '运行' '#18C773' 1211 'eq' '1' 30)) $null ([ordered]@{qtTheme='light';qtBackgroundColor='transparent';qtFontSize=16;qtStateTextColor='true';stateColorMode='indicator';qtHideStatusLamp='true';qtBorderless='true';qtVerticalAlignment='Center';unavailableStateLabel='--'})))
    Monitor-SignalField $w 'pcs-fault' '总故障状态' 1212 '无故障' '故障' 837 305 98
    Monitor-SignalField $w 'pcs-alarm' '总报警状态' 1213 '无告警' '告警' 519 380 98
    Monitor-SignalField $w 'pcs-emergency' '急停输入状态' 1215 '无急停' '急停' 625 380 103
    $w.Add((Ref-Text 'pcs-grid-label' '并离网状态' 731 380 196 30 14 '#777E85'))
    $w.Add((New-Widget 'pcs-grid' 'statusCard' '' 731 417 196 30 5 @((New-Binding 1216),(New-Binding 1217)) @((New-StateRule 'grid' '并网状态' '#18C773' 1216 'eq' '1' 10),(New-StateRule 'offgrid' '离网状态' '#F52D59' 1217 'eq' '1' 20)) $null ([ordered]@{qtTheme='light';qtBackgroundColor='transparent';qtFontSize=16;qtStateTextColor='true';stateColorMode='indicator';qtHideStatusLamp='true';qtBorderless='true';qtVerticalAlignment='Center';unavailableStateLabel='--'})))
    Monitor-Heading $w 'pcs-alarms-heading' '告警信息' 'monitor-alarm.png' 960 276
    Monitor-Alarms $w 'pcs-alarms' @(1399) 960 310 390 132
    Monitor-Box $w 'pcs-dc' 162 471 352 303 '直流侧';Monitor-Box $w 'pcs-ac' 532 471 840 303 '交流侧'
    $dc=@(@{T='直流电压';I=1244;U='V'},@{T='直流电流';I=1245;U='A'},@{T='直流功率';I=1243;U='kW'},@{T='累计放电量';I=1250;U='kWh'},@{T='累计充电量';I=1249;U='kWh'})
    foreach($i in 0..4){$e=$dc[$i];Monitor-MetricTile $w "pcs-dc-$i" $e.T $e.I $e.U "monitor-dc-$i.png" (180+($i%2)*164) (528+[math]::Floor($i/2)*81) $(if($i-eq4){316}else{152}) 69 @('#E6F2F8','#EAF8F6','#FBF4EE','#ECEAF8','#E4F3F8')[$i]}
    $m=Get-LanhuMeterIndexes 'Pcs';$cols=@(@{T='电压';A=$m.V;U='V'},@{T='电流';A=$m.I;U='A'},@{T='有功功率';A=$m.P;U='kW'},@{T='无功功率';A=$m.Q;U='kvar'},@{T='视在功率';A=$m.S;U='kVA'},@{T='下发有功功率';A=@(1318,1319,1320);U='kW'},@{T='下发无功功率';A=@(1321,1322,1323);U='kvar'})
    Monitor-PhaseTable $w 'pcs-ac-table' $cols 550 531 804 176
    $totals=@(@{T='总有功功率';I=1230;U='kW'},@{T='总无功功率';I=1234;U='kvar'},@{T='总视在功率';I=1238;U='kVA'},@{T='累计放电量';I=1248;U='kWh'},@{T='累计充电量';I=1247;U='kWh'})
    foreach($i in 0..4){$e=$totals[$i];Monitor-Field $w "pcs-ac-total-$i" $e.T $e.I $e.U (568+$i*160) 704 151}
    return New-MonitorScreen 'Devices-Pcs' $w
}
function Monitor-PhaseTable($Widgets,$Id,$Columns,$X,$Y,$W,$H) {
    $Widgets.Add((New-Frame "$Id-head" $X $Y $W 43 '#EDF1F9' 1))
    $rowH=($H-43)/3;$labelW=72;$cellW=($W-$labelW)/$Columns.Count
    $Widgets.Add((New-Frame "$Id-stripe" $X ($Y+43+$rowH) $W $rowH '#F5F7FA' 1))
    for($col=0;$col-lt$Columns.Count;$col++){
        $e=$Columns[$col];$left=$X+$labelW+$col*$cellW
        $Widgets.Add((Ref-Text "$Id-head-$col" $e.T $left $Y $cellW 43 14 '#303438' $false 'Center'))
        foreach($row in 0..2){Ref-Value $Widgets "$Id-$col-$row" $e.A[$row] ($left+4) ($Y+43+$row*$rowH) ($cellW-8) $rowH $e.U 18}
    }
    foreach($row in 0..2){$Widgets.Add((Ref-Text "$Id-phase-$row" "$(@('A','B','C')[$row])相" $X ($Y+43+$row*$rowH) $labelW $rowH 14 '#303438' $false 'Center'))}
}
function Build-MonitorBms {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Devices-Bms' 'Device' 'Bms'
    Monitor-Box $w 'bms-signals-panel' 162 222 409 466;Monitor-Box $w 'bms-measurements-panel' 590 222 782 552
    Monitor-Heading $w 'bms-signals' '遥信信息' 'monitor-signal.png' 180 236
    Monitor-Field $w 'bms-charge-state' '充放电指示' 1454 '' 186 267 123
    Monitor-SignalField $w 'bms-online' '在线状态' 3999 '在线' '离线' 318 267 123 '1'
    Monitor-SignalField $w 'bms-fault' 'BMS故障' 1462 '无故障' '故障' 450 267 105
    foreach($i in 0..2){
        $left=186+$i*132;$index=Find-PointIndex -Names @("$(@('一级','二级','三级')[$i])报警") -SharedMemoryPattern 'ttySP3'
        if($null-ne$index-and(Tag-At $index).deviceId-ne(Tag-At 3999).deviceId){$index=$null}
        $w.Add((Ref-Text "bms-level-$i-label" "簇$(@('一级','二级','三级')[$i])告警" $left 337 123 30 16 '#777E85'))
        Monitor-State $w "bms-level-$i" $index @(@{V='0';T='无告警';C='#18C773'},@{V='0';Op='ne';T='告警';C='#F52D59'}) $left 374 123
    }
    Monitor-Heading $w 'bms-alarm-heading' '告警信息' 'monitor-alarm.png' 180 414
    Monitor-Alarms $w 'bms-alarms' @(3999) 179 452 377 226
    $b=Ref-Button 'bms-cells-link' '单体信息' 'Devices-Cells' 162 703 409 71;$b.properties.qtBackgroundColor='#E4F3F8';$w.Add($b)
    $w.Add((New-LanhuImage 'bms-cells-icon' 'monitor-cell-link.png' 203 721 42 42))
    $w[$w.Count-1].zIndex=21
    Monitor-Heading $w 'bms-measurements' '遥测信息' 'monitor-measurement.png' 609 236
    $top=@(@{T='实时功率';I=$null;U='kW'},@{T='SOC';I=1569;U='%'},@{T='SOH';I=1570;U='%'},@{T='总电压';I=1566;U='V'},@{T='电流';I=1567;U='A'},@{T='绝缘电阻';I=1571;U='kΩ'})
    $limits=@(@{T="簇允许充电`n最大功率";I=$null;U='kW'},@{T="簇允许充电`n最大电压";I=$null;U='V'},@{T="簇允许充电`n最大电流";I=1556;U='A'},@{T="簇允许放电`n最大功率";I=$null;U='kW'},@{T="簇允许放电`n最大电压";I=$null;U='V'},@{T="簇允许放电`n最大电流";I=1557;U='A'})
    foreach($i in 0..5){foreach($row in 0..1){$e=if($row-eq0){$top[$i]}else{$limits[$i]};Monitor-Field $w "bms-measurement-$row-$i" $e.T $e.I $e.U (616+$i*123) (261+$row*77) 118}}
    $w.Add((Ref-Panel 'bms-extrema-panel' 607 430 747 112 '#F3F8FC'))
    $ext=@(@{T='最高单体电压';I=1574;U='mV';N=1576},@{T='最低单体电压';I=1577;U='mV';N=1579},@{T='最高单体温度';I=1580;U='℃';N=1582},@{T='最低单体温度';I=1583;U='℃';N=1585})
    foreach($i in 0..3){$e=$ext[$i];$left=627+$i*184;Monitor-Field $w "bms-extrema-$i" $e.T $e.I $e.U $left 442 168;Ref-Value $w "bms-extrema-number-$i" $e.N $left 508 168 27 '号电芯' 12}
    Monitor-Heading $w 'bms-energy-heading' '电能统计' 'monitor-energy.png' 607 564
    $energy=@(@{T='可充电量';I=1590},@{T='可放电量';I=1591},@{T='最近一次充电量';I=1588},@{T='最近一次放电量';I=1589})
    foreach($i in 0..3){$e=$energy[$i];Monitor-MetricTile $w "bms-energy-$i" $e.T $e.I 'kWh' "monitor-energy-$i.png" (607+$i*190) 600 176 69 @('#E6F2F8','#EAF8F6','#FBF4EE','#ECEAF8')[$i]}
    $w.Add((Ref-Panel 'bms-energy-total-panel' 607 684 747 73 '#E4F3F8'))
    foreach($i in 0..2){Monitor-MetricTile $w "bms-energy-total-$i" @('累计充电量','累计放电量','充放电循环')[$i] $null $(if($i-eq2){'次'}else{'kWh'}) "monitor-energy-total-$i.png" (628+$i*242) 688 233 66 '#E4F3F8'}
    return New-MonitorScreen 'Devices-Bms' $w
}
function Build-MonitorCells($Page=1,$Cluster=1) {
    $id=if($Cluster-eq2){'Devices-Cells-2'}elseif($Page-eq1){'Devices-Cells'}else{"Devices-Cells-Page$Page"}
    $w=[System.Collections.Generic.List[object]]::new();Ref-Chrome $w $id 'Monitor'
    Ref-Tabs $w @(@{Id='1';T='1#电池簇';Target='Devices-Cells'},@{Id='2';T='2#电池簇';Target='Devices-Cells-2'}) ([string]$Cluster) 613 80 308
    $back=Ref-Button 'cells-back' '返回' 'Devices-Bms' 1320 87 82 36;$back.properties.qtTextColor='#129AF5';$back.properties.qtBorderColor='#129AF5';$w.Add($back)
    $totals=@(@{T='BMS总电压';I=1566;U='V'},@{T='BMS总电流';I=1567;U='A'},@{T='平均温度';I=1573;U='℃'},@{T='簇实时充放电功率';I=$null;U='kW'})
    foreach($i in 0..3){$e=$totals[$i];$left=132+$i*322;$w.Add((Ref-Panel "cells-summary-$i-panel" $left 158 307 92));$w.Add((New-LanhuImage "cells-summary-icon-$i" "monitor-cells-$i.png" ($left+21) 174 60 60));Monitor-Field $w "cells-summary-$i" $e.T $(if($Cluster-eq1){$e.I}else{$null}) $e.U ($left+100) 166 204 22}
    Monitor-Heading $w 'cells-heading' '电芯电压与温度' 'power-heading.png' 132 263
    $selector=Ref-Button 'cells-page-selector' "全部电芯 · 第$Page 页" $(if($Cluster-eq1){'Devices-Cells'}else{''}) 1185 262 218 31
    if($Cluster-eq1){$menu=@();foreach($p in 1..9){$menu+=@{text="全部电芯 · 第$p 页";screen=$(if($p-eq1){'Devices-Cells'}else{"Devices-Cells-Page$p"})}};$selector.properties|Add-Member qtMenuScreensJson (ConvertTo-Json -InputObject $menu -Compress)}
    $w.Add($selector)
    $w.Add((Ref-Panel 'cells-body' 132 302 1271 486));$w.Add((Ref-Panel 'cells-extrema' 150 319 1234 84 '#F3F8FC'))
    $ext=@(@{T='电芯最大电压';I=1574;U='mV';N=1576},@{T='电芯最小电压';I=1577;U='mV';N=1579},@{T='电芯最高温度';I=1580;U='℃';N=1582},@{T='电芯最低温度';I=1583;U='℃';N=1585})
    foreach($i in 0..3){$e=$ext[$i];$left=175+$i*309;Monitor-Field $w "cells-extrema-$i" $e.T $(if($Cluster-eq1){$e.I}else{$null}) $e.U $left 328 156 18;Monitor-Field $w "cells-extrema-number-$i" '电芯序号' $(if($Cluster-eq1){$e.N}else{$null}) '' ($left+168) 328 99 18}
    foreach($slot in 0..31){
        $number=($Page-1)*32+$slot+1;$left=150+($slot%4)*308.5;$top=419+[math]::Floor($slot/4)*41.25
        $border=New-Frame "cell-$slot-box" $left $top 308.5 41.25 '#FFFFFF' 1;$border.properties|Add-Member qtBorderColor '#D3D9E1';$w.Add($border)
        $w.Add((New-Frame "cell-$slot-number-bg" ($left+1) ($top+1) 74 39.25 '#F0F7FC' 2))
        $w.Add((Ref-Text "cell-$slot-number" $(if($number-le260){[string]$number}else{''}) $left $top 75 41.25 14 '#303438' $false 'Center'))
        foreach($offset in @(75,191)){ $w.Add((New-Frame "cell-$slot-divider-$offset" ($left+$offset) $top 1 41.25 '#D3D9E1' 3)) }
        $w.Add((New-Frame "cell-$slot-top-rule" $left $top 308.5 1 '#D3D9E1' 3))
        if($slot-ge28){$w.Add((New-Frame "cell-$slot-bottom-rule" $left ($top+40.25) 308.5 1 '#D3D9E1' 3))}
        $voltage=$null;$temperature=$null
        if($Cluster-eq1-and$number-le260){
            $candidateV=2000+$number;$candidateT=3000+$number
            $owner=if(Has-Index 1566){(Tag-At 1566).deviceId}else{''}
            if((Has-Index $candidateV)-and(Tag-At $candidateV).deviceId-eq$owner){$voltage=$candidateV}
            if((Has-Index $candidateT)-and(Tag-At $candidateT).deviceId-eq$owner){$temperature=$candidateT}
        }
        Ref-Value $w "cell-$slot-voltage" $voltage ($left+76) $top 114 41.25 'mV' 16
        Ref-Value $w "cell-$slot-temperature" $temperature ($left+192) $top 115 41.25 '℃' 16
    }
    $w.Add((New-Frame 'cells-left-rule' 150 419 1 330 '#D3D9E1' 3))
    $w.Add((New-Frame 'cells-right-rule' 1383 419 1 330 '#D3D9E1' 3))
    return New-MonitorScreen $id $w
}
function Build-MonitorThermal($Id='Devices-Thermal') {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w $Id 'Device' 'Thermal'
    Monitor-Box $w 'thermal-details' 162 222 783 552;Monitor-Box $w 'thermal-alarm-panel' 963 222 409 552
    Monitor-Heading $w 'thermal-liquid' '液冷' 'monitor-liquid.png' 180 237
    Monitor-SignalField $w 'liquid-online' '在线状态' 127 '在线' '离线' 186 263 124 '1'
    Monitor-Field $w 'liquid-fault' '故障代码' 153 '' 320 263 124
    Monitor-Field $w 'liquid-mode' '运行模式反馈' 128 '' 470 263 200
    Monitor-Field $w 'liquid-water-out' '出水温度' 131 '℃' 688 263 126
    Monitor-Field $w 'liquid-pressure-out' '出水压力' 141 'Bar' 823 263 108
    foreach($i in 0..3){$e=@(@{T='回水温度';I=132;U='℃'},@{T='回水压力';I=142;U='Bar'},@{T='设置温度';I=166;U='℃'},@{T='环境温度';I=135;U='℃'})[$i];Monitor-Field $w "liquid-second-$i" $e.T $e.I $e.U (186+$i*182) 331 165}
    Monitor-Heading $w 'thermal-dehumidifier' '除湿' 'monitor-dehumidifier.png' 180 416
    Monitor-SignalField $w 'dehumidifier-online' '在线状态' 188 '在线' '离线' 186 444 124 '1'
    Monitor-SignalField $w 'dehumidifier-fault' '运行状态' $dehumidifierPoints.fault '正常' '告警' 320 444 124
    Monitor-Field $w 'dehumidifier-work' '工作状态' $dehumidifierPoints.workState '' 470 444 160
    Monitor-Field $w 'dehumidifier-humidity' '柜内湿度' $dehumidifierPoints.cabinetHumidity '%RH' 688 444 126
    Monitor-Field $w 'dehumidifier-temperature' '柜内温度' $dehumidifierPoints.cabinetTemperature '℃' 823 444 108
    Monitor-Heading $w 'thermal-fan' '风机' 'monitor-fan.png' 180 521
    foreach($i in 0..2){$left=186+$i*254;$w.Add((Ref-Text "fan-$i-label" $(if($i-eq0){'风机继电器'}else{"$($i+1)#风机"}) $left 558 112 32 14 '#777E85'));Monitor-State $w "fan-$i-state" $(if($i-eq0){991}else{$null}) @(@{V='0';T='继电器断开';C='#90979F'},@{V='1';T='继电器闭合';C='#129AF5'}) ($left+112) 558 125}
    Monitor-Heading $w 'thermal-fire' '消防' 'monitor-fire.png' 180 612
    Monitor-SignalField $w 'fire-online' '在线状态' 310000 '在线' '离线' 186 638 134 '1'
    Monitor-SignalField $w 'fire-alarm' '综合报警' 310012 '无告警' '告警' 338 638 134
    Monitor-Field $w 'fire-smoke' '烟雾减光率' 310102 '%' 507 638 150
    Monitor-Field $w 'fire-voc' 'VOC浓度' 310105 'ppm' 672 638 150
    Monitor-Field $w 'fire-co' 'CO浓度' 310103 'ppm' 816 638 115
    Monitor-Field $w 'fire-hydrogen' '氢气浓度' 310104 'ppm' 186 706 150
    Monitor-Field $w 'fire-temperature' '柜内温度' 310101 '℃' 338 706 150
    Monitor-Heading $w 'thermal-alarms-heading' '告警信息' 'monitor-alarm.png' 982 237
    Monitor-Alarms $w 'thermal-alarms' @(127,188,310000,991) 980 270 375 490
    return New-MonitorScreen $Id $w
}
function Build-MonitorMccb {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Devices-Mccb' 'Device' 'Mccb'
    foreach($i in 0..7){
        $left=162+($i%3)*412;$top=233+[math]::Floor($i/3)*102;Monitor-Box $w "mccb-$i" $left $top 385 84
        $w.Add((New-LanhuImage "mccb-$i-icon" 'monitor-breaker.png' ($left+17) ($top+20) 42 43))
        $w.Add((Ref-Text "mccb-$i-title" @('储能断路器','并网断路器','负荷断路器','断路器04','断路器05','断路器06','断路器07','断路器08')[$i] ($left+73) ($top+12) 270 25 16 '#303438' $true))
        $w.Add((Ref-Text "mccb-$i-online-label" '当前状态：' ($left+73) ($top+40) 76 30 14 '#777E85'));Ref-Value $w "mccb-$i-online" $null ($left+149) ($top+40) 48 30
        $w.Add((Ref-Text "mccb-$i-position-label" '开关位置：' ($left+228) ($top+40) 76 30 14 '#777E85'))
        Monitor-State $w "mccb-$i-position" $(if($i-lt3){@(997,998,999)[$i]}else{$null}) @(@{V='1';T='合闸';C='#F52D59'},@{V='0';T='分闸';C='#18C773'}) ($left+304) ($top+40) 64 30
    }
    Monitor-Pager $w 'mccb' '' ''
    return New-MonitorScreen 'Devices-Mccb' $w
}
function Monitor-Pager($Widgets,$Id,$Previous,$Next) {
    foreach($i in 0..1){$b=Ref-Button "$Id-page-$i" @('上一页','下一页')[$i] @($Previous,$Next)[$i] (1155+$i*116) 716 102 38;$b.properties.qtTextColor='#129AF5';$b.properties.qtBackgroundColor='#E3F3FF';$b.properties.qtCornerRadius=19;$Widgets.Add($b)}
}
function Build-MonitorMeters($Page=1) {
    $id=if($Page-eq1){'Devices-Meters'}else{'Devices-Meters-Page2'};$w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w $id 'Device' 'Meters'
    foreach($side in 0..1){
        $left=162+$side*618;$source=if($Page-eq1){@('Input','Output')[$side]}else{@('Through','Reserved')[$side]}
        $known=$source-ne'Reserved';$m=if($known){Get-LanhuMeterIndexes $source}else{@{V=@($null,$null,$null);I=@($null,$null,$null);P=@($null,$null,$null);Q=@($null,$null,$null);S=@($null,$null,$null);TotalP=$null;TotalQ=$null;TotalS=$null;Factor=$null;Frequency=$null}}
        Monitor-Box $w "meter-$side" $left 219 592 489
        $w.Add((New-LanhuImage "meter-$side-icon" 'monitor-meter.png' ($left+18) 234 54 54))
        $w.Add((Ref-Text "meter-$side-title" $(switch($source){'Input'{'并网点电表'}'Output'{'防逆流表'}'Through'{'负荷电表'}default{'电表预留'}}) ($left+87) 234 480 30 18 '#303438' $true))
        $w.Add((Ref-Text "meter-$side-online-label" '在线状态：' ($left+87) 264 71 30 14 '#777E85'))
        Monitor-Binary $w "meter-$side-online" $(switch($source){'Input'{1199}'Output'{1099}'Through'{9099}default{$null}}) '在线' '离线' ($left+158) 264 54 '1'
        $w.Add((Ref-Text "meter-$side-ratio" 'PT变比： --    CT变比： --' ($left+216) 264 300 30 14 '#777E85'))
        $cols=@(@{T='电压';A=$m.V;U='V'},@{T='电流';A=$m.I;U='A'},@{T='有功功率';A=$m.P;U='kW'},@{T='无功功率';A=$m.Q;U='kvar'},@{T='视在功率';A=$m.S;U='kVA'})
        Monitor-PhaseTable $w "meter-$side-table" $cols ($left+18) 313 556 197
        $w.Add((New-Frame "meter-$side-energy-band" ($left+1) 526 590 36 '#F7F7F7' 1));$w.Add((Ref-Text "meter-$side-energy-heading" '功率和电量' $left 526 592 36 18 '#303438' $true 'Center'))
        $total=@(@{T='总有功功率';I=$m.TotalP;U='kW'},@{T='总无功功率';I=$m.TotalQ;U='kvar'},@{T='总视在功率';I=$m.TotalS;U='kVA'},@{T='功率因数';I=$m.Factor;U=''},@{T='频率';I=$m.Frequency;U='Hz'})
        foreach($i in 0..4){$e=$total[$i];Monitor-Field $w "meter-$side-total-$i" $e.T $e.I $e.U ($left+16+$i*116) 565 111}
        $base=switch($source){'Input'{1100}'Output'{1000}'Through'{9000}default{$null}}
        foreach($i in 0..3){Monitor-Field $w "meter-$side-energy-$i" @('正向有功总电量','反向有功总电量','正向无功总电量','反向无功总电量')[$i] $(if($null-ne$base){$base+@(5,10,20,25)[$i]}else{$null}) $(if($i-lt2){'kWh'}else{'kvarh'}) ($left+16+$i*145) 634 139}
    }
    Monitor-Pager $w 'meter' $(if($Page-eq2){'Devices-Meters'}else{''}) $(if($Page-eq1){'Devices-Meters-Page2'}else{''})
    return New-MonitorScreen $id $w
}
function Build-MonitorDiesel {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Devices-Diesel' 'Device' 'Diesel'
    $w.Add((New-LanhuImage 'diesel-icon' 'monitor-diesel.png' 162 226 66 66));$w.Add((Ref-Text 'diesel-title' '柴油发电机' 252 230 190 32 18 '#303438' $true));Ref-Value $w 'diesel-running' $null 444 230 96 32;Ref-Value $w 'diesel-enabled' $null 546 230 96 32
    foreach($i in 0..4){$left=252+$i*224;$w.Add((Ref-Text "diesel-meta-label-$i" @('转速(r/min)','充电机电压(V)','电池电压(V)','机油温度(℃)','剩余油量(L)')[$i] $left 259 154 26 14 '#777E85'));Ref-Value $w "diesel-meta-$i" $null ($left+160) 259 56 26}
    Monitor-Box $w 'diesel-generation' 162 291 731 330 '发电信息';Monitor-Box $w 'diesel-mains' 913 291 459 165 '市电信息'
    $cols=@();foreach($e in @(@{T='电压';U='V'},@{T='电流';U='A'},@{T='有功功率';U='kW'},@{T='无功功率';U='kvar'},@{T='视在功率';U='kVA'},@{T='功率因数';U=''})){$cols+=@{T=$e.T;A=@($null,$null,$null);U=$e.U}}
    Monitor-PhaseTable $w 'diesel-table' $cols 177 372 700 169
    foreach($i in 0..5){Monitor-Field $w "diesel-total-$i" @('总有功功率','总无功功率','视在功率','功率因数','频率','相位')[$i] $null '' (177+$i*116) 548 110}
    $w.Add((Ref-Panel 'diesel-mains-phase-band' 928 369 430 42 '#F3F8FC'))
    foreach($i in 0..2){$left=938+$i*140;$w.Add((Ref-Text "diesel-mains-label-$i" "$(@('A','B','C')[$i])相" $left 369 40 42 14));Ref-Value $w "diesel-mains-$i" $null ($left+44) 369 86 42 'V'}
    foreach($i in 0..1){$w.Add((Ref-Text "diesel-mains-summary-label-$i" @('频率：','相位：')[$i] (1008+$i*140) 417 70 30 14 '#777E85'));Ref-Value $w "diesel-mains-summary-$i" $null (1070+$i*140) 417 74 30}
    Monitor-Box $w 'diesel-emergency-panel' 913 472 459 44;Monitor-Heading $w 'diesel-emergency-heading' '急停输入' 'monitor-alarm.png' 930 480;Ref-Value $w 'diesel-emergency' $null 1279 480 78 28
    Monitor-Box $w 'diesel-accumulated' 162 637 731 117 '累计信息'
    foreach($i in 0..4){Monitor-Field $w "diesel-accumulated-$i" @('累计燃油消耗','累计运行时间','累计开机次数','累计电能','维护剩余时间')[$i] $null '' (177+$i*140) 684 134}
    Monitor-Box $w 'diesel-alarm-panel' 913 532 459 222 '告警信息';Monitor-Alarms $w 'diesel-alarms' @() 928 588 430 157
    return New-MonitorScreen 'Devices-Diesel' $w
}
function Build-MonitorStrategy {
    $w=[System.Collections.Generic.List[object]]::new();Monitor-Chrome $w 'Monitor-Strategy' 'Strategy';$w.Add((Ref-Panel 'monitor-strategy-body' 132 155 1271 614))
    $cards=@(
        @{Id='reverse';T='防逆流治理';I=$null;Icon=0;X=162;Y=180;Fields=@(@{T='扫描时间';I=$null;U='sec'},@{T='充电开启时刻';I=$null;U=''},@{T='充电结束时刻';I=$null;U=''},@{T='光伏跟踪功率';I=$null;U='kW'})},
        @{Id='schedule';T='定时充放电';I=18;Icon=5;X=162;Y=324;Fields=@(@{T='扫描时间';I=$null;U='sec'},@{T='放电电压上限';I=463;U='V'},@{T='充电电压下限';I=464;U='V'})},
        @{Id='voltage';T='电压治理';I=$null;Icon=6;X=162;Y=468;Fields=@(@{T='低压上限';I=545;U='V'},@{T='低压下限';I=544;U='V'},@{T='高压上限';I=547;U='V'},@{T='高压下限';I=546;U='V'})},
        @{Id='emergency';T='应急保电';I=$null;Icon=3;X=162;Y=612;Fields=@(@{T='保电SOC';I=$null;U='%'},@{T='充电功率';I=$null;U='kW'})},
        @{Id='capacity';T='动态增容';I=24;Icon=1;X=780;Y=180;Fields=@(@{T='动态增容保留功率';I=588;U='kW'})},
        @{Id='balance';T='三相不平衡治理';I=20;Icon=4;X=780;Y=324;Fields=@(@{T='治理间隔时间';I=$null;U='sec'},@{T='不平衡度限制';I=562;U='%'})},
        @{Id='reactive';T='无功治理';I=8;Icon=2;X=780;Y=468;Fields=@(@{T='扫描时间';I=$null;U='sec'},@{T='目标功率因数';I=514;U=''})}
    )
    foreach($c in $cards){
        Monitor-Box $w "strategy-$($c.Id)" $c.X $c.Y 592 128
        $w.Add((New-LanhuImage "strategy-$($c.Id)-icon" "strategy-$($c.Icon).png" ($c.X+19) ($c.Y+16) 36 36));$w.Add((Ref-Text "strategy-$($c.Id)-title" $c.T ($c.X+68) ($c.Y+15) 160 36 16 '#303438' $true))
        if($c.Id-eq'voltage'){
            $inactive=New-StateRule 'idle' '未执行' '#90979F' 10 'eq' '0' 10;$inactive.conditions+=(New-StateRule 'idle-high' '' '' 12 'eq' '0' 10).conditions
            $running=New-StateRule 'run-low' '执行中' '#18C773' 10 'eq' '1' 20;$running.match='any';$running.conditions+=(New-StateRule 'run-high' '' '' 12 'eq' '1' 20).conditions
            $w.Add((New-Widget 'strategy-voltage-state' 'statusCard' '' ($c.X+238) ($c.Y+21) 120 25 5 @((New-Binding 10),(New-Binding 12)) @($inactive,$running) $null ([ordered]@{qtTheme='light';qtBackgroundColor='transparent';qtFontSize=16;qtStateTextColor='true';stateColorMode='indicator';qtHideStatusLamp='true';qtBorderless='true';qtVerticalAlignment='Center';unavailableStateLabel='--'})))
        }else{Monitor-State $w "strategy-$($c.Id)-state" $c.I @(@{V='1';T='执行中';C='#18C773'},@{V='0';T='未执行';C='#90979F'}) ($c.X+238) ($c.Y+21) 120 25}
        $detail=Ref-Button "strategy-$($c.Id)-detail-reserved" '›' '' ($c.X+552) ($c.Y+17) 24 30;$detail.properties.qtTextColor='#129AF5';$w.Add($detail)
        $fieldW=556/$c.Fields.Count
        for($i=0;$i-lt$c.Fields.Count;$i++){$e=$c.Fields[$i];Monitor-Field $w "strategy-$($c.Id)-field-$i" $e.T $e.I $e.U ($c.X+18+$i*$fieldW) ($c.Y+58) ($fieldW-6)}
    }
    return New-MonitorScreen 'Monitor-Strategy' $w
}
function Get-MonitorScreens {
    Build-MonitorNetwork;Build-MonitorStrategy;Build-MonitorEmu;Build-MonitorMccb;Build-MonitorMeters;Build-MonitorMeters 2;Build-MonitorDiesel
    Build-MonitorCells;Build-MonitorCells 1 2
    foreach($page in 2..9){Build-MonitorCells $page}
}
