# Shared primitives for the approved 1440 x 810 Lanhu boards.
$LanhuExpectedScreens = @($XMindV2ExpectedScreens) + @(
    'Topology-Single', 'Control-Bms', 'Control-Thermal', 'Control-Mccb', 'Control-Diesel'
)

function New-LanhuPanel($Id, $X, $Y, $Width, $Height, $Color = '#FFFFFF', $Z = 0) {
    $widget = New-Frame $Id $X $Y $Width $Height $Color $Z
    $widget.properties | Add-Member qtCornerRadius 8
    return $widget
}

function New-LanhuButton($Id, $Text, $Target, $Active, $X, $Y, $Width, $Height) {
    $widget = New-NavigationButton $Id $Text $Target $Active $X $Y $Width $Height
    if ([string]::IsNullOrWhiteSpace($Target)) { $widget.action = $null }
    $widget.properties.qtBackgroundColor = if ($Active) { '#129AF5' } else { '#FFFFFF' }
    $widget.properties.qtTextColor = if ($Active) { '#FFFFFF' } else { '#303438' }
    $widget.properties.qtFontSize = 16
    $widget.properties.qtTransparent = $false
    $widget.properties | Add-Member qtBorderColor 'transparent'
    $widget.properties | Add-Member qtCornerRadius 8
    $widget.properties | Add-Member qtPressedColor '#D8EBFA'
    return $widget
}

function New-LanhuImage($Id, $File, $X, $Y, $Width, $Height) {
    New-Widget $Id 'qtImage' '' $X $Y $Width $Height 4 @() @() $null ([ordered]@{
        qtImageFile = "assets/lanhu-assets/$File"; qtBackgroundColor = 'transparent'
    })
}

function Add-LanhuValue($Widgets, $Id, $Index, $X, $Y, $Width, $Height, $Unit = '', $Size = 16, $Color = '#303438') {
    if ($null -ne $Index -and [uint32]$Index -gt 0 -and (Has-Index ([uint32]$Index))) {
        $value = New-BoundText $Id ([uint32]$Index) $X $Y $Width $Height $Size $Color 'Center'
        $value.properties | Add-Member valueSuffix $Unit
        $Widgets.Add($value)
    } else {
        $Widgets.Add((New-Text $Id '--' $X $Y $Width $Height $Size '#90979F' 'Center'))
    }
}

function Get-LanhuMeterIndexes($Source) {
    switch ($Source) {
        'Input' { return @{V=@(1130,1131,1132);I=@(1133,1134,1135);P=@(1136,1137,1138);Q=@(1140,1141,1142);S=@(1144,1145,1146);AvgV=1162;AvgI=1164;TotalP=1139;TotalQ=1143;TotalS=1147;Factor=1151;Frequency=1152} }
        'Pcs' { return @{V=@(1220,1221,1222);I=@(1223,1224,1225);P=@(1227,1228,1229);Q=@(1231,1232,1233);S=@(1235,1236,1237);AvgV=0;AvgI=0;TotalP=1230;TotalQ=1234;TotalS=1238;Factor=1242;Frequency=1226} }
        'Through' { return @{V=@(9030,9031,9032);I=@(9033,9034,9035);P=@(9036,9037,9038);Q=@(9040,9041,9042);S=@(9044,9045,9046);AvgV=9062;AvgI=9064;TotalP=9039;TotalQ=9043;TotalS=9047;Factor=9051;Frequency=9052} }
        default { return @{V=@(1030,1031,1032);I=@(1033,1034,1035);P=@(1036,1037,1038);Q=@(1040,1041,1042);S=@(1044,1045,1046);AvgV=1062;AvgI=1064;TotalP=1039;TotalQ=1043;TotalS=1047;Factor=1051;Frequency=1052} }
    }
}
