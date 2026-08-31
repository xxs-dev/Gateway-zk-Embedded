# EMS 2.0 XMind V2 page information architecture.
# This file is dot-sourced by generate-scada.ps1 after point metadata and the
# common widget helpers have been initialized.

$XMindV2ExpectedScreens = @(
    "Guide-Setup", "Guide-Deploy", "Guide-Startup",
    "Meters-Overview", "Meters-Electrical-Output", "Meters-Electrical-Input", "Meters-Electrical-Pcs",
    "Meters-Power-Output", "Meters-Power-Input", "Meters-Power-Pcs", "Meters-Power-Through", "Meters-Energy",
    "Topology", "Vehicle-Control",
    "Trends-Power", "Trends-Voltage", "Trends-Current", "Trends-Factor", "Trends-Dc", "Trends-Cell", "Trends-Custom",
    "Info-Alarms", "Info-Messages",
    "Strategy-Overview", "Strategy-Charge", "Strategy-Grid", "Strategy-Balance",
    "Devices-Pcs", "Devices-Bms", "Devices-Thermal", "Devices-Fire",
    "Control-Pcs", "Control-Dido", "Maintenance"
)

$xmindVehiclePoints = [ordered]@{
    brake = Find-PointIndex -Names @("车辆制动到位", "制动到位", "驻车制动")
    support = Find-PointIndex -Names @("车辆支撑到位", "支撑到位", "支腿到位")
    gps = Find-PointIndex -Names @("GPS 在线", "GPS 定位有效", "GPS上传状态")
    ground = Find-PointIndex -Names @("接地电阻", "车辆接地电阻")
    fieldLight = Find-PointIndex -Names @("场地灯", "场地照明灯") -RequireWritable
    markerLight = Find-PointIndex -Names @("示廓灯", "车辆示廓灯") -RequireWritable
}

function Resolve-XMindTopSection([string]$ActiveSection) {
    if ($ActiveSection -match '^(Guide)') { return "Guide" }
    if ($ActiveSection -match '^(Overview|Meters)') { return "Meters" }
    if ($ActiveSection -match '^(Topology)') { return "Topology" }
    if ($ActiveSection -match '^(Vehicle)') { return "Vehicle" }
    if ($ActiveSection -match '^(Trends)') { return "Trends" }
    if ($ActiveSection -match '^(Alarms|Info)') { return "Info" }
    return "Maintenance"
}

# Override the former compact header. Existing strategy/device/control builders
# automatically join the XMind V2 navigation without duplicating their bodies.
function Add-CompactChrome(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$ActiveSection
) {
    $active = Resolve-XMindTopSection $ActiveSection
    $Widgets.Add((New-Frame "chrome-header" 0 0 1920 104 "#071B26" -10))
    $Widgets.Add((New-Frame "chrome-brand" 26 26 50 50 "#D9F4F7" 1))
    $Widgets.Add((New-Text "chrome-brand-text" "KY" 34 37 34 28 16 "#09202B" "Center" $true 3))
    $Widgets.Add((New-Text "chrome-title" $projectDisplayName 92 19 360 38 24 "#F3F8FA" "Left" $true 3))
    $Widgets.Add((New-Text "chrome-subtitle" $MachineCode 92 59 360 26 16 "#74B9D0" "Left" $false 3))

    $pages = @(
        @{ Id="Guide"; Text="引导"; Target="Guide-Setup" },
        @{ Id="Meters"; Text="仪表"; Target="Meters-Overview" },
        @{ Id="Topology"; Text="拓扑"; Target="Topology" },
        @{ Id="Vehicle"; Text="车辆"; Target="Vehicle-Control" },
        @{ Id="Trends"; Text="曲线"; Target="Trends-Power" },
        @{ Id="Info"; Text="信息"; Target="Info-Alarms" },
        @{ Id="Maintenance"; Text="维护"; Target="Maintenance" }
    )
    $x = 478.0
    foreach ($page in $pages) {
        $button = New-NavigationButton "nav-$($page.Id)" $page.Text $page.Target ($page.Id -eq $active) $x 18 140 70
        $button.properties.qtFontSize = 20
        $Widgets.Add($button)
        $x += 150
    }
    $Widgets.Add((New-Text "chrome-runtime" "● 本地运行" 1544 37 340 30 18 "#55E0AA" "Right" $true 3))
    $Widgets.Add((New-Text "chrome-footer" "共享内存直读 · XMind V2 · 刷新 500 ms" 24 1048 760 24 15 "#74B9D0" "Left" $false 3))
    $Widgets.Add((New-Text "chrome-machine" $MachineCode 1570 1048 326 24 15 "#74B9D0" "Right" $false 3))
}

function Add-XMindTabsAt(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Prefix,
    [object[]]$Tabs,
    [string]$ActiveId,
    [double]$Y = 120,
    [double]$Height = 58
) {
    $gap = 12.0
    $width = (1872.0 - (($Tabs.Count - 1) * $gap)) / $Tabs.Count
    for ($i = 0; $i -lt $Tabs.Count; $i++) {
        $tab = $Tabs[$i]
        $button = New-NavigationButton "$Prefix-$($tab.Id)" $tab.Text $tab.Target ($tab.Id -eq $ActiveId) `
            (24 + $i * ($width + $gap)) $Y $width $Height
        $button.properties.qtFontSize = 19
        $Widgets.Add($button)
    }
}

function Add-XMindStaticCard(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    [string]$Value,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height,
    [string]$Accent = "#71808A"
) {
    $Widgets.Add((New-Frame "$Id-frame" $X $Y $Width $Height "#0B2633" 0))
    $Widgets.Add((New-Frame "$Id-accent" $X $Y 5 $Height $Accent 1))
    $Widgets.Add((New-Text "$Id-title" $Title ($X + 18) ($Y + 16) ($Width - 36) 30 21 "#9FC0CC" "Center" $false 4))
    $Widgets.Add((New-Text "$Id-value" $Value ($X + 18) ($Y + 54) ($Width - 36) ($Height - 66) 28 "#F3F8FA" "Center" $true 5))
}

function Add-XMindOptionalBinary(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    $Index,
    [string]$NormalValue,
    [string]$NormalLabel,
    [string]$AbnormalLabel,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    if ($null -ne $Index -and [uint32]$Index -gt 0 -and (Has-Index ([uint32]$Index))) {
        Add-CompactBinaryCard $Widgets $Id $Title ([uint32]$Index) $NormalValue $NormalLabel $AbnormalLabel $X $Y $Width $Height "#D9A441"
    } else {
        Add-XMindStaticCard $Widgets $Id $Title "未配置点位" $X $Y $Width $Height
    }
}

function Resolve-XMindNormalValue($Index, [string]$DefaultValue) {
    if ($null -ne $Index -and [uint32]$Index -gt 0 -and (Has-Index ([uint32]$Index))) {
        return Normal-ValueFor ([uint32]$Index) $DefaultValue
    }
    return $DefaultValue
}

function Add-XMindMetricGrid(
    [System.Collections.Generic.List[object]]$Widgets,
    [object[]]$Items,
    [int]$Columns,
    [double]$X,
    [double]$Y,
    [double]$Width,
    [double]$Height
) {
    $gap = 14.0
    $rows = [math]::Ceiling($Items.Count / [double]$Columns)
    $cardWidth = ($Width - ($Columns - 1) * $gap) / $Columns
    $cardHeight = ($Height - ($rows - 1) * $gap) / $rows
    for ($i = 0; $i -lt $Items.Count; $i++) {
        $item = $Items[$i]
        $col = $i % $Columns
        $row = [math]::Floor($i / $Columns)
        $itemX = $X + $col * ($cardWidth + $gap)
        $itemY = $Y + $row * ($cardHeight + $gap)
        $accent = if ($null -ne $item.PSObject.Properties["Accent"]) { [string]$item.Accent } else { "#55D8E8" }
        $index = if ($null -ne $item.PSObject.Properties["Index"] -and $null -ne $item.Index) { [uint32]$item.Index } else { [uint32]0 }
        if ($index -gt 0 -and (Has-Index $index)) {
            $unit = if ($null -ne $item.PSObject.Properties["Unit"]) { [string]$item.Unit } else { "" }
            Add-CompactMetricCard $Widgets $item.Id $item.Title $index $itemX $itemY $cardWidth $cardHeight $unit $accent
        } else {
            Add-XMindStaticCard $Widgets $item.Id $item.Title "未配置点位" $itemX $itemY $cardWidth $cardHeight $accent
        }
    }
}

function Add-XMindGuideTabs([System.Collections.Generic.List[object]]$Widgets, [string]$Active) {
    Add-XMindTabsAt $Widgets "guide-tab" @(
        @{Id="Setup";Text="功能设置";Target="Guide-Setup"},
        @{Id="Deploy";Text="设备部署";Target="Guide-Deploy"},
        @{Id="Startup";Text="启动流程";Target="Guide-Startup"}
    ) $Active
}

function Add-XMindMeterTabs(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Category,
    [string]$Source = ""
) {
    Add-XMindTabsAt $Widgets "meter-category" @(
        @{Id="Overview";Text="运行总览";Target="Meters-Overview"},
        @{Id="Electrical";Text="电气参数";Target="Meters-Electrical-Output"},
        @{Id="Power";Text="功率参数";Target="Meters-Power-Output"},
        @{Id="Energy";Text="能量参数";Target="Meters-Energy"}
    ) $Category 116 56
    if ($Category -eq "Electrical") {
        Add-XMindTabsAt $Widgets "meter-source" @(
            @{Id="Output";Text="输出监测";Target="Meters-Electrical-Output"},
            @{Id="Input";Text="输入监测";Target="Meters-Electrical-Input"},
            @{Id="Pcs";Text="PCS 监测";Target="Meters-Electrical-Pcs"}
        ) $Source 182 58
    } elseif ($Category -eq "Power") {
        Add-XMindTabsAt $Widgets "meter-source" @(
            @{Id="Output";Text="输出监测";Target="Meters-Power-Output"},
            @{Id="Input";Text="输入监测";Target="Meters-Power-Input"},
            @{Id="Pcs";Text="PCS 监测";Target="Meters-Power-Pcs"},
            @{Id="Through";Text="穿越监测";Target="Meters-Power-Through"}
        ) $Source 182 58
    }
}

function Build-XMindGuideSetupScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Guide"
    Add-XMindGuideTabs $widgets "Setup"
    $widgets.Add((New-Frame "guide-setup-topology-panel" 24 202 596 826 "#081E29" 0))
    $widgets.Add((New-Text "guide-setup-topology-title" "拓扑与接入" 48 224 520 36 25 "#EAF6FA" "Left" $true 3))
    Add-XMindStaticCard $widgets "guide-setup-role" "主从机角色" "本机运行" 48 282 548 154 "#55D8E8"
    Add-XMindStaticCard $widgets "guide-setup-peer" "从机自动连接主机" "未配置点位" 48 454 548 154
    Add-XMindStaticCard $widgets "guide-setup-diesel" "柴油机接入方式" "未接入" 48 626 548 154
    Add-XMindStaticCard $widgets "guide-setup-data" "运行数据源" "共享内存直读" 48 798 548 154 "#55E0AA"

    $widgets.Add((New-Frame "guide-setup-mode-panel" 636 202 596 826 "#081E29" 0))
    $widgets.Add((New-Text "guide-setup-mode-title" "并网 / 离网" 660 224 520 36 25 "#EAF6FA" "Left" $true 3))
    Add-XMindOptionalBinary $widgets "guide-grid-state" "并网状态" 1216 "1" "已并网" "未并网" 660 282 548 174
    Add-XMindOptionalBinary $widgets "guide-offgrid-state" "离网状态" 1217 "1" "已离网" "未离网" 660 474 548 174
    Add-XMindMetricGrid $widgets @(
        [pscustomobject]@{Id="guide-grid-frequency";Title="并网频率";Index=1052;Unit="Hz"},
        [pscustomobject]@{Id="guide-grid-voltage";Title="并网平均电压";Index=1062;Unit="V"}
    ) 2 660 666 548 158
    $openControl = New-NavigationButton "guide-open-pcs-control" "打开 PCS 并/离网设置" "Control-Pcs" $false 660 848 548 104
    $openControl.properties.qtFontSize = 22
    $widgets.Add($openControl)

    $widgets.Add((New-Frame "guide-setup-cellular-panel" 1248 202 648 826 "#081E29" 0))
    $widgets.Add((New-Text "guide-setup-cellular-title" "4G 功能" 1272 224 560 36 25 "#EAF6FA" "Left" $true 3))
    Add-XMindOptionalBinary $widgets "guide-cellular-enabled" "4G 监测使能" 920000001 "1" "已启用" "已关闭" 1272 282 600 154
    $widgets.Add((New-Frame "guide-cellular-link-frame" 1272 454 600 154 "#0B2633" 0))
    $widgets.Add((New-Text "guide-cellular-link-title" "4G 连接测试" 1292 470 560 30 21 "#9FC0CC" "Center" $false 4))
    $widgets.Add((New-CellularLinkStatus "guide-cellular-link" 1292 510 560 80))
    $widgets.Add((New-CellularSignalCard "guide-cellular-signal" 1272 626 600 154))
    Add-XMindOptionalBinary $widgets "guide-cellular-route" "当前网络出口" 920000004 "1" "4G 出口" "有线出口" 1272 798 600 154
    return New-Screen "Guide-Setup" "引导 - 功能设置" $widgets
}

function Build-XMindGuideDeployScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Guide"
    Add-XMindGuideTabs $widgets "Deploy"
    $widgets.Add((New-Text "guide-deploy-title" "设备部署检查" 24 208 600 36 25 "#EAF6FA" "Left" $true 3))
    Add-XMindOptionalBinary $widgets "guide-deploy-brake" "车辆制动" $xmindVehiclePoints.brake "1" "已制动" "未制动" 24 266 452 196
    Add-XMindOptionalBinary $widgets "guide-deploy-support" "车辆支撑" $xmindVehiclePoints.support "1" "已支撑" "未支撑" 492 266 452 196
    Add-XMindOptionalBinary $widgets "guide-deploy-gps" "GPS 上传与验证" $xmindVehiclePoints.gps "1" "定位有效" "定位无效" 960 266 452 196
    Add-XMindOptionalBinary $widgets "guide-deploy-ground" "接地检测" $xmindVehiclePoints.ground "1" "接地正常" "接地异常" 1428 266 468 196
    Add-XMindOptionalBinary $widgets "guide-phase-voltage" "并网电压相序" 322024 "0" "相序正常" "相序错误" 24 486 608 196
    Add-XMindOptionalBinary $widgets "guide-phase-current" "并网电流相序" 322025 "0" "相序正常" "相序错误" 648 486 608 196
    Add-XMindOptionalBinary $widgets "guide-emergency" "急停回路" $dioPoints.emergency (Resolve-XMindNormalValue $dioPoints.emergency "0") "正常" "动作" 1272 486 624 196
    Add-XMindOptionalBinary $widgets "guide-door" "柜门状态" $dioPoints.frontDoor (Resolve-XMindNormalValue $dioPoints.frontDoor "1") "关闭" "打开" 24 706 608 196
    Add-XMindOptionalBinary $widgets "guide-surge" "浪涌保护" $dioPoints.surge (Resolve-XMindNormalValue $dioPoints.surge "0") "正常" "动作" 648 706 608 196
    Add-XMindOptionalBinary $widgets "guide-water" "水浸检测" $dioPoints.water (Resolve-XMindNormalValue $dioPoints.water "0") "正常" "动作" 1272 706 624 196
    return New-Screen "Guide-Deploy" "引导 - 设备部署" $widgets
}

function Build-XMindGuideStartupScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Guide"
    Add-XMindGuideTabs $widgets "Startup"
    $widgets.Add((New-Text "guide-start-title" "启动流程" 24 208 600 36 25 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Text "guide-start-step-1" "1 设备状态" 24 260 420 34 22 "#55D8E8" "Center" $true 3))
    $widgets.Add((New-Text "guide-start-step-2" "2 电压检测" 500 260 420 34 22 "#55D8E8" "Center" $true 3))
    $widgets.Add((New-Text "guide-start-step-3" "3 SOC 检测" 976 260 420 34 22 "#55D8E8" "Center" $true 3))
    $widgets.Add((New-Text "guide-start-step-4" "4 策略启动" 1452 260 420 34 22 "#55D8E8" "Center" $true 3))
    Add-XMindOptionalBinary $widgets "guide-start-pcs" "PCS 通讯" 1399 "1" "在线" "离线" 24 316 420 154
    Add-XMindOptionalBinary $widgets "guide-start-bms" "BMS 通讯" 3999 "1" "在线" "离线" 24 488 420 154
    Add-XMindOptionalBinary $widgets "guide-start-fire" "消防通讯" 310000 "1" "在线" "离线" 24 660 420 154
    Add-XMindMetricGrid $widgets @(
        [pscustomobject]@{Id="guide-start-va";Title="A 相电压";Index=1030;Unit="V"},
        [pscustomobject]@{Id="guide-start-vb";Title="B 相电压";Index=1031;Unit="V"},
        [pscustomobject]@{Id="guide-start-vc";Title="C 相电压";Index=1032;Unit="V"}
    ) 1 500 316 420 498
    $widgets.Add((New-ProgressCard "guide-start-soc" "电池 SOC" 1569 976 316 420 326 38))
    Add-CompactMetricCard $widgets "guide-start-soh" "电池 SOH" 1570 976 660 420 154 "%" "#55D8E8"
    Add-XMindOptionalBinary $widgets "guide-start-schedule" "计划曲线" 18 "1" "运行" "待机" 1452 316 420 154
    Add-CompactMultiCard $widgets "guide-start-cycle" "充放电阶段" 17 (Compact-PhaseStates) 1452 488 420 154
    Add-XMindOptionalBinary $widgets "guide-start-balance" "三相平衡" 20 "1" "运行" "待机" 1452 660 420 154
    $openStrategy = New-NavigationButton "guide-open-strategy" "查看策略执行" "Strategy-Overview" $false 1452 838 420 104
    $openStrategy.properties.qtFontSize = 22
    $widgets.Add($openStrategy)
    return New-Screen "Guide-Startup" "引导 - 启动流程" $widgets
}

function Build-XMindMetersOverviewScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Meters"
    Add-XMindMeterTabs $widgets "Overview"
    $kpiWidth = 361.6
    Add-CompactBinaryCard $widgets "meters-overview-pcs-state" "PCS 运行状态" 1211 "1" "运行" "停机" 24 194 $kpiWidth 132 "#D9A441"
    Add-CompactMetricCard $widgets "meters-overview-grid-power" "并网有功" 1039 401.6 194 $kpiWidth 132 "kW" "#55D8E8"
    Add-CompactMetricCard $widgets "meters-overview-soc" "电池 SOC" 1569 779.2 194 $kpiWidth 132 "%" "#55E0AA"
    Add-CompactMetricCard $widgets "meters-overview-charge" "今日充电" 1615 1156.8 194 $kpiWidth 132 "kWh" "#55D8E8"
    Add-CompactMetricCard $widgets "meters-overview-discharge" "今日放电" 1616 1534.4 194 $kpiWidth 132 "kWh" "#F9CC44"

    $widgets.Add((New-Frame "meters-overview-flow-panel" 24 344 1180 684 "#081E29" 0))
    $widgets.Add((New-Text "meters-overview-flow-title" "能量流与运行参数" 48 362 500 34 24 "#EAF6FA" "Left" $true 3))
    Add-CompactMetricCard $widgets "meters-overview-flow-grid" "并网功率" 1039 54 420 280 142 "kW" "#55D8E8"
    $widgets.Add((New-EnergyFlow "meters-overview-grid-to-pcs" 1039 "positive" 352 462 100 60 30))
    Add-CompactBinaryCard $widgets "meters-overview-flow-pcs" "PCS 状态" 1211 "1" "运行" "停机" 470 420 280 142 "#D9A441"
    $widgets.Add((New-EnergyFlow "meters-overview-pcs-to-battery" 1230 "negative" 768 462 100 60 30))
    Add-CompactMetricCard $widgets "meters-overview-flow-battery" "电池 SOC" 1569 886 420 280 142 "%" "#55E0AA"
    Add-XMindMetricGrid $widgets @(
        [pscustomobject]@{Id="meters-overview-pcs-power";Title="PCS 总有功";Index=1230;Unit="kW"},
        [pscustomobject]@{Id="meters-overview-battery-voltage";Title="电池总电压";Index=1566;Unit="V"},
        [pscustomobject]@{Id="meters-overview-battery-current";Title="电池总电流";Index=1567;Unit="A"},
        [pscustomobject]@{Id="meters-overview-grid-frequency";Title="电网频率";Index=1052;Unit="Hz"},
        [pscustomobject]@{Id="meters-overview-cabinet-temperature";Title="柜内温度";Index=$dehumidifierPoints.cabinetTemperature;Unit="℃";Accent="#F9CC44"},
        [pscustomobject]@{Id="meters-overview-cabinet-humidity";Title="柜内湿度";Index=$dehumidifierPoints.cabinetHumidity;Unit="%RH";Accent="#F9CC44"}
    ) 3 48 590 1132 410

    $widgets.Add((New-Frame "meters-overview-health-panel" 1220 344 676 404 "#081E29" 0))
    $widgets.Add((New-Text "meters-overview-health-title" "设备健康" 1244 362 300 34 24 "#EAF6FA" "Left" $true 3))
    $health = @(
        @{Id="pcs";T="PCS";I=1399},@{Id="bms";T="BMS";I=3999},
        @{Id="liquid";T="液冷机";I=127},@{Id="dehumidifier";T="除湿机";I=188},
        @{Id="fire";T="消防";I=310000}
    )
    for ($i = 0; $i -lt $health.Count; $i++) {
        $item = $health[$i]
        Add-CompactBinaryCard $widgets "meters-overview-health-$($item.Id)" $item.T $item.I "1" "在线" "离线" `
            (1244 + ($i % 2) * 314) (412 + [math]::Floor($i / 2) * 104) 298 92
    }
    $widgets.Add((New-CellularSignalCard "meters-overview-cellular" 1558 620 298 92))
    $widgets.Add((New-Widget "meters-overview-active-alarms" "alarmTable" "活动设备告警" 1220 764 676 264 2 @() @() $null ([ordered]@{
        qtBackgroundColor="#071A2D";qtTextColor="#E8F0F2";qtFontSize=18
    })))
    return New-Screen "Meters-Overview" "仪表 - 运行总览" $widgets
}

function Get-XMindElectricalItems([string]$Source) {
    switch ($Source) {
        "Output" {
            $v=@(1030,1031,1032,1062);$c=@(1033,1034,1035,1064);$a=@(1065,1066,1067,0);$f=1052
        }
        "Input" {
            $v=@(1130,1131,1132,1162);$c=@(1133,1134,1135,1164);$a=@(1165,1166,1167,0);$f=1152
        }
        default {
            $v=@(1220,1221,1222,0);$c=@(1223,1224,1225,0);$a=@(0,0,0,0);$f=1226
        }
    }
    $items = [System.Collections.Generic.List[object]]::new()
    $phase=@("A","B","C","平均")
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="electrical-v-$i";Title="电压 $($phase[$i])";Index=$v[$i];Unit="V"})}
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="electrical-i-$i";Title="电流 $($phase[$i])";Index=$c[$i];Unit="A";Accent="#55E0AA"})}
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="electrical-angle-$i";Title="相角 $($phase[$i])";Index=$a[$i];Unit="°";Accent="#F9CC44"})}
    $items.Add([pscustomobject]@{Id="electrical-frequency";Title="频率";Index=$f;Unit="Hz";Accent="#D9A441"})
    return @($items)
}

function Build-XMindElectricalScreen([string]$Source) {
    $widgets=[System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Meters"
    Add-XMindMeterTabs $widgets "Electrical" $Source
    Add-XMindMetricGrid $widgets (Get-XMindElectricalItems $Source) 4 24 262 1872 740
    return New-Screen "Meters-Electrical-$Source" "仪表 - 电气参数 - $Source" $widgets
}

function Get-XMindPowerItems([string]$Source) {
    [int]$base = 9000
    if ($Source -eq "Output") { $base = 1000 }
    elseif ($Source -eq "Input") { $base = 1100 }
    elseif ($Source -eq "Pcs") { $base = 1200 }
    if($Source -eq "Pcs"){$p=@(1227,1228,1229,1230);$q=@(1231,1232,1233,1234);$s=@(1235,1236,1237,1238);$pf=@(1239,1240,1241,1242)}
    else {
        $p = @(($base + 36), ($base + 37), ($base + 38), ($base + 39))
        $q = @(($base + 40), ($base + 41), ($base + 42), ($base + 43))
        $s = @(($base + 44), ($base + 45), ($base + 46), ($base + 47))
        $pf = @(($base + 48), ($base + 49), ($base + 50), ($base + 51))
    }
    $items=[System.Collections.Generic.List[object]]::new();$phase=@("A","B","C","总")
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="power-p-$i";Title="有功 $($phase[$i])";Index=$p[$i];Unit="kW"})}
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="power-q-$i";Title="无功 $($phase[$i])";Index=$q[$i];Unit="kvar";Accent="#55E0AA"})}
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="power-s-$i";Title="视在 $($phase[$i])";Index=$s[$i];Unit="kVA";Accent="#F9CC44"})}
    # The XMind requests power angle here. The current acquisition catalog only
    # has power factor, so keep the requested role visible without fabricating a
    # calculated angle. Real power factor remains available on Trends-Factor.
    for($i=0;$i-lt4;$i++){$items.Add([pscustomobject]@{Id="power-angle-$i";Title="功率角 $($phase[$i])";Index=0;Unit="°";Accent="#D9A441"})}
    return @($items)
}

function Build-XMindPowerScreen([string]$Source) {
    $widgets=[System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Meters"
    Add-XMindMeterTabs $widgets "Power" $Source
    Add-XMindMetricGrid $widgets (Get-XMindPowerItems $Source) 4 24 262 1872 740
    return New-Screen "Meters-Power-$Source" "仪表 - 功率参数 - $Source" $widgets
}

function Build-XMindEnergyScreen {
    $widgets=[System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Meters"
    Add-XMindMeterTabs $widgets "Energy"
    $widgets.Add((New-Frame "energy-storage-1" 24 196 920 832 "#081E29" 0))
    $widgets.Add((New-Text "energy-storage-1-title" "移动储能 1" 48 218 400 36 25 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-ProgressCard "energy-storage-1-soc" "SOC" 1569 48 278 872 208 38))
    Add-XMindMetricGrid $widgets @(
        [pscustomobject]@{Id="energy-storage-1-soh";Title="SOH";Index=1570;Unit="%"},
        [pscustomobject]@{Id="energy-storage-1-charge-left";Title="可充电量";Index=1590;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-discharge-left";Title="可放电量";Index=1591;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-last-charge";Title="本次充电";Index=1588;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-today-charge";Title="今日充电";Index=1615;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-total-charge";Title="累计充电";Index=1586;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-last-discharge";Title="本次放电";Index=1589;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-today-discharge";Title="今日放电";Index=1616;Unit="kWh"},
        [pscustomobject]@{Id="energy-storage-1-total-discharge";Title="累计放电";Index=1587;Unit="kWh"}
    ) 3 48 514 872 486
    $widgets.Add((New-Frame "energy-other-panel" 960 196 936 832 "#081E29" 0))
    $widgets.Add((New-Text "energy-other-title" "扩展能源设备" 984 218 500 36 25 "#EAF6FA" "Left" $true 3))
    Add-XMindStaticCard $widgets "energy-storage-2-soc" "移动储能 2 · SOC" "未接入" 984 278 888 196
    Add-XMindStaticCard $widgets "energy-storage-2-time" "移动储能 2 · 剩余时间" "未配置点位" 984 492 888 196
    Add-XMindStaticCard $widgets "energy-diesel-fuel" "柴油发电机 · 油量" "未接入" 984 706 432 196
    Add-XMindStaticCard $widgets "energy-diesel-time" "柴油发电机 · 剩余时间" "未配置点位" 1432 706 440 196
    return New-Screen "Meters-Energy" "仪表 - 能量参数" $widgets
}

function Build-XMindTopologyScreen {
    $widgets=[System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Topology"
    $widgets.Add((New-Text "topology-title" "设备拓扑与实时潮流" 24 126 700 40 27 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Frame "topology-main-panel" 24 184 1872 510 "#081E29" 0))
    Add-CompactMetricCard $widgets "topology-grid" "电网 / 并网侧" 1039 60 302 280 174 "kW" "#55D8E8"
    $widgets.Add((New-EnergyFlow "topology-flow-grid-pcs" 1039 "positive" 358 358 120 60 30))
    Add-XMindOptionalBinary $widgets "topology-grid-breaker" "并网断路器" $dioPoints.gridBreaker "1" "合位" "分位" 496 302 250 174
    $widgets.Add((New-EnergyFlow "topology-flow-breaker-pcs" 1039 "positive" 764 358 120 60 30))
    Add-CompactMetricCard $widgets "topology-pcs" "PCS" 1230 902 302 280 174 "kW" "#55E0AA"
    $widgets.Add((New-EnergyFlow "topology-flow-pcs-battery" 1230 "negative" 1200 358 120 60 30))
    $widgets.Add((New-ProgressCard "topology-battery" "电池 SOC" 1569 1338 302 300 174 30))
    $widgets.Add((New-EnergyFlow "topology-flow-grid-load" 9039 "positive" 654 546 120 60 30))
    Add-CompactMetricCard $widgets "topology-load" "负荷" 9039 792 510 280 146 "kW" "#F9CC44"
    Add-XMindOptionalBinary $widgets "topology-load-breaker" "负荷断路器" $dioPoints.loadBreaker "1" "合位" "分位" 1090 510 250 146
    Add-CompactMetricCard $widgets "topology-storage-meter" "储能侧电表" 1139 1358 510 280 146 "kW" "#D9A441"

    $widgets.Add((New-Frame "topology-strategy-panel" 24 714 1240 314 "#081E29" 0))
    $widgets.Add((New-Text "topology-strategy-title" "策略运行" 48 734 300 34 24 "#EAF6FA" "Left" $true 3))
    Add-XMindOptionalBinary $widgets "topology-strategy-schedule" "计划曲线" 18 "1" "运行" "待机" 48 788 278 190
    Add-XMindOptionalBinary $widgets "topology-strategy-grid" "电压治理" 10 "1" "运行" "待机" 342 788 278 190
    Add-XMindOptionalBinary $widgets "topology-strategy-balance" "三相平衡" 20 "1" "运行" "待机" 636 788 278 190
    Add-XMindOptionalBinary $widgets "topology-strategy-cos" "无功补偿" 8 "1" "运行" "待机" 930 788 286 190
    $widgets.Add((New-Frame "topology-network-panel" 1280 714 616 314 "#081E29" 0))
    $widgets.Add((New-Text "topology-network-title" "4G 网络" 1304 734 300 34 24 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-CellularSignalCard "topology-cellular-signal" 1304 788 276 190))
    Add-XMindOptionalBinary $widgets "topology-cellular-route" "当前出口" 920000004 "1" "4G 出口" "有线出口" 1596 788 276 190
    return New-Screen "Topology" "拓扑" $widgets
}

function Add-XMindLightControl(
    [System.Collections.Generic.List[object]]$Widgets,
    [string]$Id,
    [string]$Title,
    $Index,
    [double]$X
) {
    if ($null -eq $Index -or [uint32]$Index -eq 0 -or -not (Has-Index ([uint32]$Index))) {
        Add-XMindStaticCard $Widgets $Id $Title "未配置点位" $X 286 896 536
        return
    }
    Add-CompactBinaryCard $Widgets "$Id-state" "$Title 状态" ([uint32]$Index) "1" "已开启" "已关闭" $X 286 896 212 "#71808A"
    if (Is-WritableIndex ([uint32]$Index)) {
        $Widgets.Add((New-ControlButton "$Id-on" "开启" ([uint32]$Index) "writeSetpoint" "1" ($X + 24) 544 412 216 $false "#124536"))
        $Widgets.Add((New-ControlButton "$Id-off" "关闭" ([uint32]$Index) "writeSetpoint" "0" ($X + 460) 544 412 216 $false "#273C46"))
    } else {
        Add-XMindStaticCard $Widgets "$Id-readonly" "$Title 控制" "点位只读" ($X + 24) 544 848 216
    }
}

function Build-XMindVehicleControlScreen {
    $widgets=[System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets "Vehicle"
    $widgets.Add((New-Text "vehicle-title" "车辆灯光控制" 24 126 700 40 27 "#EAF6FA" "Left" $true 3))
    $widgets.Add((New-Text "vehicle-security" "本页操作需登录" 1510 130 360 34 19 "#F9CC44" "Right" $true 3))
    Add-XMindLightControl $widgets "vehicle-field-light" "场地灯" $xmindVehiclePoints.fieldLight 24
    Add-XMindLightControl $widgets "vehicle-marker-light" "示廓灯" $xmindVehiclePoints.markerLight 1000
    Add-XMindOptionalBinary $widgets "vehicle-brake" "车辆制动" $xmindVehiclePoints.brake "1" "已制动" "未制动" 24 850 596 152
    Add-XMindOptionalBinary $widgets "vehicle-support" "车辆支撑" $xmindVehiclePoints.support "1" "已支撑" "未支撑" 636 850 596 152
    Add-XMindOptionalBinary $widgets "vehicle-gps" "GPS" $xmindVehiclePoints.gps "1" "定位有效" "定位无效" 1248 850 648 152
    return New-Screen "Vehicle-Control" "车辆控制" $widgets
}

function Add-XMindTrendTabs([System.Collections.Generic.List[object]]$Widgets,[string]$Active) {
    Add-XMindTabsAt $Widgets "trend-tab" @(
        @{Id="Power";Text="功率";Target="Trends-Power"},@{Id="Voltage";Text="电压";Target="Trends-Voltage"},
        @{Id="Current";Text="电流";Target="Trends-Current"},@{Id="Factor";Text="功率因数";Target="Trends-Factor"},
        @{Id="Dc";Text="直流";Target="Trends-Dc"},@{Id="Cell";Text="电芯";Target="Trends-Cell"},
        @{Id="Custom";Text="自定义";Target="Trends-Custom"}
    ) $Active
}

function Add-XMindChartPanel(
    [System.Collections.Generic.List[object]]$Widgets,[string]$Id,[string]$Title,
    [uint32[]]$Indexes,[string[]]$Names,[string[]]$Units,[string[]]$Colors,
    [double]$X,[double]$Y,[double]$Width,[double]$Height
) {
    $Widgets.Add((New-Frame "$Id-panel" $X $Y $Width $Height "#081E29" 0))
    $Widgets.Add((New-Text "$Id-title" "$Title · 最近 1 小时" ($X+20) ($Y+14) ($Width-40) 32 22 "#EAF6FA" "Left" $true 3))
    $Widgets.Add((New-Chart "$Id-chart" $Title $Indexes $Names $Units $Colors ($X+12) ($Y+54) ($Width-24) ($Height-66) @() 60 1 1000))
}

function Build-XMindTrendScreen([string]$Kind) {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Trends";Add-XMindTrendTabs $widgets $Kind
    switch($Kind){
        "Power" { Add-XMindChartPanel $widgets "trend-power" "输入 / 输出 / PCS / 穿越有功" ([uint32[]]@(1039,1139,1230,9039)) ([string[]]@("输出","输入","PCS","穿越")) ([string[]]@("kW","kW","kW","kW")) ([string[]]@("#55D8E8","#F9CC44","#55E0AA","#D9A441")) 24 198 1872 830 }
        "Voltage" { Add-XMindChartPanel $widgets "trend-voltage" "三相电压" ([uint32[]]@(1030,1031,1032,1130,1131,1132,1220,1221,1222)) ([string[]]@("输出A","输出B","输出C","输入A","输入B","输入C","PCS A","PCS B","PCS C")) ([string[]]@("V","V","V","V","V","V","V","V","V")) ([string[]]@("#55D8E8","#78B6CC","#246B84","#F9CC44","#D9A441","#8E6E2E","#55E0AA","#37B47F","#23795A")) 24 198 1872 830 }
        "Current" { Add-XMindChartPanel $widgets "trend-current" "三相电流" ([uint32[]]@(1033,1034,1035,1133,1134,1135,1223,1224,1225)) ([string[]]@("输出A","输出B","输出C","输入A","输入B","输入C","PCS A","PCS B","PCS C")) ([string[]]@("A","A","A","A","A","A","A","A","A")) ([string[]]@("#55D8E8","#78B6CC","#246B84","#F9CC44","#D9A441","#8E6E2E","#55E0AA","#37B47F","#23795A")) 24 198 1872 830 }
        "Factor" { Add-XMindChartPanel $widgets "trend-factor" "总功率因数" ([uint32[]]@(1051,1151,1242,9051)) ([string[]]@("输出","输入","PCS","穿越")) ([string[]]@("","","","")) ([string[]]@("#55D8E8","#F9CC44","#55E0AA","#D9A441")) 24 198 1872 830 }
        "Dc" {
            Add-XMindChartPanel $widgets "trend-dc-soc" "直流 SOC" ([uint32[]]@(1569)) ([string[]]@("SOC")) ([string[]]@("%")) ([string[]]@("#55E0AA")) 24 198 920 398
            Add-XMindChartPanel $widgets "trend-dc-voltage" "直流电压" ([uint32[]]@(1566,1244)) ([string[]]@("BMS总压","PCS输入")) ([string[]]@("V","V")) ([string[]]@("#55D8E8","#246B84")) 960 198 936 398
            Add-XMindChartPanel $widgets "trend-dc-current" "直流电流" ([uint32[]]@(1567,1245)) ([string[]]@("BMS总流","PCS输入")) ([string[]]@("A","A")) ([string[]]@("#F9CC44","#D9A441")) 24 612 920 416
            Add-XMindChartPanel $widgets "trend-dc-power" "直流功率" ([uint32[]]@(1243)) ([string[]]@("PCS输入功率")) ([string[]]@("kW")) ([string[]]@("#55E0AA")) 960 612 936 416
        }
        "Cell" {
            Add-XMindChartPanel $widgets "trend-cell-voltage" "单体最高 / 最低电压" ([uint32[]]@(1574,1577)) ([string[]]@("最高电压","最低电压")) ([string[]]@("mV","mV")) ([string[]]@("#F9CC44","#55D8E8")) 24 198 1872 398
            Add-XMindChartPanel $widgets "trend-cell-temperature" "单体最高 / 最低温度" ([uint32[]]@(1580,1583)) ([string[]]@("最高温度","最低温度")) ([string[]]@("℃","℃")) ([string[]]@("#E45858","#55E0AA")) 24 612 1872 416
        }
        default { Add-XMindChartPanel $widgets "trend-custom" "工程自定义功率对比" ([uint32[]]@(1039,1139,1230,9039)) ([string[]]@("输出","输入","PCS","穿越")) ([string[]]@("kW","kW","kW","kW")) ([string[]]@("#55D8E8","#F9CC44","#55E0AA","#D9A441")) 24 198 1872 830 }
    }
    return New-Screen "Trends-$Kind" "曲线 - $Kind" $widgets
}

function Add-XMindInfoTabs([System.Collections.Generic.List[object]]$Widgets,[string]$Active){
    Add-XMindTabsAt $Widgets "info-tab" @(
        @{Id="Alarms";Text="告警";Target="Info-Alarms"},@{Id="Messages";Text="消息与状态";Target="Info-Messages"}
    ) $Active
}

function Build-XMindInfoAlarmsScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Info";Add-XMindInfoTabs $widgets "Alarms"
    $summary=@(
        @{Id="pcs";T="PCS 故障";I=1212;N="0";G="正常";B="故障"},@{Id="bms";T="BMS 故障";I=1462;N="0";G="正常";B="故障"},
        @{Id="liquid";T="液冷故障";I=153;N="0";G="正常";B="故障"},@{Id="fire";T="消防报警";I=310012;N="0";G="正常";B="报警"}
    )
    for($i=0;$i-lt$summary.Count;$i++){$item=$summary[$i];Add-CompactBinaryCard $widgets "info-alarm-$($item.Id)" $item.T $item.I $item.N $item.G $item.B (24+$i*468) 204 452 140}
    $widgets.Add((New-Widget "info-alarm-table" "alarmTable" "活动设备告警" 24 364 1872 664 2 @() @() $null ([ordered]@{qtBackgroundColor="#071A2D";qtTextColor="#E8F0F2";qtFontSize=20})))
    return New-Screen "Info-Alarms" "信息 - 告警" $widgets
}

function Build-XMindInfoMessagesScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Info";Add-XMindInfoTabs $widgets "Messages"
    Add-XMindOptionalBinary $widgets "info-grid-state" "并网状态" 1216 "1" "已并网" "未并网" 24 212 452 174
    Add-XMindOptionalBinary $widgets "info-offgrid-state" "离网状态" 1217 "1" "已离网" "未离网" 492 212 452 174
    Add-XMindOptionalBinary $widgets "info-cellular-state" "4G 网络" 920000003 "1" "已连接" "未连接" 960 212 452 174
    Add-XMindOptionalBinary $widgets "info-cellular-route" "当前出口" 920000004 "1" "4G 出口" "有线出口" 1428 212 468 174
    Add-XMindOptionalBinary $widgets "info-schedule" "计划曲线" 18 "1" "运行" "待机" 24 410 452 174
    Add-CompactMultiCard $widgets "info-cycle" "充放电阶段" 17 (Compact-PhaseStates) 492 410 452 174
    Add-XMindOptionalBinary $widgets "info-balance" "三相平衡" 20 "1" "运行" "待机" 960 410 452 174
    Add-XMindOptionalBinary $widgets "info-cos" "无功补偿" 8 "1" "运行" "待机" 1428 410 468 174
    Add-XMindOptionalBinary $widgets "info-storage-breaker" "储能断路器" $dioPoints.storageBreaker "1" "合位" "分位" 24 608 608 174
    Add-XMindOptionalBinary $widgets "info-grid-breaker" "并网断路器" $dioPoints.gridBreaker "1" "合位" "分位" 648 608 608 174
    Add-XMindOptionalBinary $widgets "info-load-breaker" "负荷断路器" $dioPoints.loadBreaker "1" "合位" "分位" 1272 608 624 174
    $widgets.Add((New-Frame "info-message-panel" 24 806 1872 196 "#081E29" 0))
    $widgets.Add((New-Text "info-message-title" "当前状态" 48 828 260 34 22 "#9FC0CC" "Left" $false 4))
    $widgets.Add((New-Text "info-message-value" "设备运行消息由实时状态卡持续更新" 48 876 1824 58 30 "#F3F8FA" "Center" $true 5))
    return New-Screen "Info-Messages" "信息 - 消息与状态" $widgets
}

# Replace the old information-only maintenance page with the XMind maintenance
# landing page. Detailed device, strategy and control screens remain focused.
function Build-CompactMaintenanceScreen {
    $widgets=[System.Collections.Generic.List[object]]::new();Add-CompactChrome $widgets "Maintenance"
    $widgets.Add((New-Text "maintenance-title" "维护中心" 24 126 700 40 27 "#EAF6FA" "Left" $true 3))
    $links=@(
        @{Id="devices";T="设备监测";Target="Devices-Pcs";Color="#14576B"},
        @{Id="strategy";T="策略监测";Target="Strategy-Overview";Color="#205344"},
        @{Id="pcs";T="PCS 控制";Target="Control-Pcs";Color="#493E22"},
        @{Id="dido";T="DI / DO 控制";Target="Control-Dido";Color="#493E22"}
    )
    for($i=0;$i-lt$links.Count;$i++){$it=$links[$i];$b=New-NavigationButton "maintenance-link-$($it.Id)" $it.T $it.Target $false (24+$i*468) 194 452 160;$b.properties.qtFontSize=24;$b.properties.qtBackgroundColor=$it.Color;$b.properties.qtTransparent=$false;$widgets.Add($b)}
    $widgets.Add((New-Frame "maintenance-device-panel" 24 376 920 652 "#081E29" 0))
    $widgets.Add((New-Text "maintenance-device-title" "设备通讯" 48 398 360 34 24 "#EAF6FA" "Left" $true 3))
    $comms=@(@{Id="pcs";T="PCS";I=1399},@{Id="bms";T="BMS";I=3999},@{Id="liquid";T="液冷机";I=127},@{Id="dehumidifier";T="除湿机";I=188},@{Id="fire";T="消防";I=310000})
    for($i=0;$i-lt$comms.Count;$i++){$it=$comms[$i];Add-CompactBinaryCard $widgets "maintenance-$($it.Id)" $it.T $it.I "1" "在线" "离线" (48+($i%2)*436) (452+[math]::Floor($i/2)*174) 420 158}
    $widgets.Add((New-CellularSignalCard "maintenance-cellular-quick" 484 800 420 158))
    $widgets.Add((New-Frame "maintenance-info-panel" 960 376 936 652 "#081E29" 0))
    $widgets.Add((New-Text "maintenance-info-title" "工程与网络" 984 398 420 34 24 "#EAF6FA" "Left" $true 3))
    $info=@(@{L="工程名称";V=$projectDisplayName},@{L="工程版本";V=$PackageVersion},@{L="Machine Code";V=$MachineCode},@{L="运行拓扑";V="边端一体化"},@{L="数据源";V="共享内存直读"})
    for($i=0;$i-lt$info.Count;$i++){$y=458+$i*72;$widgets.Add((New-Text "maintenance-label-$i" $info[$i].L 984 $y 220 32 19 "#78B6CC" "Left" $false 3));$widgets.Add((New-Text "maintenance-value-$i" $info[$i].V 1210 $y 638 34 21 "#F3F8FA" "Right" $true 3))}
    $widgets.Add((New-Frame "maintenance-cellular-link-frame" 984 828 416 150 "#0B2633" 0))
    $widgets.Add((New-Text "maintenance-cellular-link-title" "4G 连接" 1004 844 376 28 20 "#9FC0CC" "Center" $false 4))
    $widgets.Add((New-CellularLinkStatus "maintenance-cellular-link" 1004 882 376 78))
    Add-XMindOptionalBinary $widgets "maintenance-cellular-route" "当前出口" 920000004 "1" "4G 出口" "有线出口" 1416 828 432 150
    return New-Screen "Maintenance" "维护中心" $widgets
}

function Get-XMindV2Screens {
    Build-XMindGuideSetupScreen
    Build-XMindGuideDeployScreen
    Build-XMindGuideStartupScreen
    Build-XMindMetersOverviewScreen
    Build-XMindElectricalScreen "Output"
    Build-XMindElectricalScreen "Input"
    Build-XMindElectricalScreen "Pcs"
    Build-XMindPowerScreen "Output"
    Build-XMindPowerScreen "Input"
    Build-XMindPowerScreen "Pcs"
    Build-XMindPowerScreen "Through"
    Build-XMindEnergyScreen
    Build-XMindTopologyScreen
    Build-XMindVehicleControlScreen
    foreach($kind in @("Power","Voltage","Current","Factor","Dc","Cell","Custom")){Build-XMindTrendScreen $kind}
    Build-XMindInfoAlarmsScreen
    Build-XMindInfoMessagesScreen
    Build-CompactStrategyOverviewScreen
    Build-CompactStrategyChargeScreen
    Build-CompactStrategyGridScreen
    Build-CompactStrategyBalanceScreen
    Build-CompactDeviceScreen "Pcs"
    Build-CompactDeviceScreen "Bms"
    Build-CompactDeviceScreen "Thermal"
    Build-CompactDeviceScreen "Fire"
    Build-CompactControlPcsScreen
    Build-CompactControlDidoScreen
    Build-CompactMaintenanceScreen
}
