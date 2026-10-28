[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../../common/powershell/Repository.ps1')
$root=Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$temp=Join-Path ([IO.Path]::GetTempPath()) ('gateway-controls-'+[guid]::NewGuid().ToString('N'))
$devices=New-Item -ItemType Directory -Path (Join-Path $temp 'runtime/devices')
$password=ConvertTo-SecureString 'fixture-only' -AsPlainText -Force
function Point($I,$Name,$Address,$Function=6,$Writable=$true){
    return @{index=$I;pointCode="point_$I";name=$Name;address=$Address;enabled=$true;read=@{enable=$true;function=4;dataType='uint16';scale=1;unit='';intervalMs=1000};write=@{enable=$Writable;function=$Function;dataType='uint16';scale=1;offset=0;length=1}}
}
function Generate($Name,$Bau=''){
    $config|ConvertTo-Json -Depth 30|Set-Content (Join-Path $devices.FullName 'fixture.json') -Encoding utf8
    & (Join-Path $root 'products/ems/mobile/2.0/tools/generate-scada.ps1') -SourceProjectDirectory (Join-Path $root 'products/ems/mobile/2.0/scada/base-project') -RuntimeConfigDirectory (Join-Path $temp 'runtime') -OutputProjectDirectory (Join-Path $temp $Name) -OutputPackage (Join-Path $temp "$Name.kyscada") -LocalOperatorPassword $password -BauMeterCode $Bau|Out-Host
    $script:screens=@(Get-ChildItem (Join-Path $temp "$Name/screens") -Filter '*.json'|ForEach-Object {Get-Content $_ -Raw|ConvertFrom-Json})
}
function Widget($Screen,$Id){return ($screens|Where-Object screenId -eq $Screen).widgets|Where-Object widgetId -eq $Id}
try{
    $pcs=@{meterCode='MTR20250600657';deviceName='PCS';enabled=$true;points=@((Point 1310 '运行模式选择' 301),(Point 1315 '并离网设置' 306))}
    $liquid=@{meterCode='FIXTURE_LIQUID_COOLING';deviceName='液冷';enabled=$true;points=@((Point 128 '液冷控制模式' 200 6 $false),(Point 164 '液冷模式设定' 200),(Point 163 '机组开关机使能' 201),(Point 166 '机组设定温度' 205))}
    $dehumid=@{meterCode='FIXTURE_DEHUMIDIFIER';deviceName='除湿';enabled=$true;points=@(220..225|ForEach-Object {$p=Point $_ "setting_$_" ($_-210) 16;$p.write.minValue=-10;$p.write.maxValue=25;$p})}
    $dio=@{meterCode='DIO';deviceName='DIO';enabled=$true;points=@((Point 998 '并网断路器合位' 1 6 $false))}
    $chart=@{meterCode='BMS';deviceName='BMS';enabled=$true;points=@(1577,1574,1583,1580|ForEach-Object {Point $_ "chart_$_" $_ 6 $false})}
    $trip=Point 200011 '远程跳闸' 0;$trip.pointCode='breaker_remote_trip'
    $close=Point 200013 '远程合闸' 0;$close.pointCode='breaker_remote_close'
    $breaker=@{meterCode='FIXTURE_BREAKER';deviceName='并网断路器';enabled=$true;points=@($trip,$close)}
    $config=@{enabled=$true;memoryStore=@{sharedMemoryName='fixture_controls'};meters=@($pcs,$liquid,$dehumid,$dio,$breaker,$chart)}
    $used=@($config.meters|ForEach-Object {$_.points.index})
    $baseTags=Get-Content (Join-Path $root 'products/ems/mobile/2.0/scada/base-project/tags.json') -Raw|ConvertFrom-Json
    foreach($group in @($baseTags|Where-Object indexFallback -notin $used|Group-Object deviceId)){
        $config.meters+=@{meterCode=('READ_ONLY_'+$group.Name);deviceName=('Fixture '+$group.Name);enabled=$true;points=@($group.Group|ForEach-Object {Point $_.indexFallback $_.displayName $_.indexFallback 6 $false})}
    }
    Generate 'valid'
    $mode=Widget 'Control-Pcs' 'control-switch-mode'
    if($mode.action.tagId-ne'MTR20250600657.point_1310'-or@($mode.properties.controlOptionsJson|ConvertFrom-Json).Count-ne5){throw 'PCS mode not bound to enum'}
    foreach($op in @(@{Id='pcs-grid-connect';V='0'},@{Id='pcs-grid-disconnect';V='1'})){
        $b=Widget 'Control-Pcs' $op.Id
        if($b.action.tagId-ne'MTR20250600657.point_1315'-or$b.action.value-ne$op.V){throw 'PCS grid control wrong register or value'}
    }
    $options=@((Widget 'Control-Thermal' 'control-switch-mode').properties.controlOptionsJson|ConvertFrom-Json)
    if(($options|Where-Object value -eq 0).feedbackValue-ne4-or($options|Where-Object value -eq 3).feedbackValue-ne1){throw 'Liquid mode read/write enums conflated'}
    foreach($op in @(@{Id='open';Tag='breaker_remote_trip';Feedback='0'},@{Id='close';Tag='breaker_remote_close';Feedback='1'})){
        $b=Widget 'Control-Mccb' "mccb-0-$($op.Id)"
        if($b.action.tagId-ne"FIXTURE_BREAKER.$($op.Tag)"-or$b.action.value-ne'0'-or$b.properties.controlFeedbackTag-ne'DIO.point_998'-or$b.properties.controlFeedbackValue-ne$op.Feedback){throw 'Breaker command or physical feedback mismatch'}
        if(-not$b.action.requiresConfirmation){throw 'Missing breaker confirmation'}
    }
    $setting=Widget 'Control-Thermal' 'tms-setting-220'
    if($setting.properties.controlMin-ne'-10'-or$setting.properties.controlMax-ne'25'-or$setting.properties.controlDecimals-ne'0'){throw 'Numeric setting limits missing'}
    foreach($screen in $screens){foreach($w in $screen.widgets){$g=$w.geometry;if($g.x-lt0-or$g.y-lt0-or$g.x+$g.width-gt1440-or$g.y+$g.height-gt810){throw 'Control geometry overflow'}}}
    $pcs.points[0].address=324;$pcs.points[1].write.enable=$false;$liquid.enabled=$false;$breaker.enabled=$false
    Generate 'disabled'
    foreach($entry in @(@{S='Control-Pcs';I='control-switch-mode'},@{S='Control-Pcs';I='pcs-grid-connect'},@{S='Control-Thermal';I='control-switch-mode'},@{S='Control-Thermal';I='tms-enable-0'},@{S='Control-Mccb';I='mccb-0-close'})){
        if($null-ne(Widget $entry.S $entry.I).action){throw "Disabled or different-protocol point enabled: $($entry.I)"}
    }
    # BAU role is site-confirmed, not inferred from slave 1 or legacy BCU indexes.
    $bau=@{meterCode='SITE_BAU';deviceName='BAU';sourceCom='COM3';protocolType='modbus_rtu';slave=2;enabled=$true;points=@(
        (Point 34100 'BAU上下电' 16385),(Point 34103 'BAU重启' 16386),
        (Point 34154 '充放电指示' 8205 6 $false),(Point 34150 '运行状态' 8193 6 $false),
        (Point 34500 '总正接触器状态' 8195 6 $false),(Point 34502 '主负接触器状态' 8195 6 $false))}
    $pack=@{meterCode='PACK_BCU';deviceName='Pack BCU';sourceCom='COM3';protocolType='modbus_rtu';slave=3;enabled=$true;points=@((Point 44100 'BCU上下电' 16385),(Point 44103 'BCU重启' 16386))}
    $config.meters+=@($bau,$pack)
    Generate 'bau-with-pack' 'SITE_BAU'
    foreach($op in @(@{Id='bau-power-on';I=34100;V='2'},@{Id='bau-power-off';I=34100;V='1'},@{Id='bau-reboot';I=34103;V='1'})){
        $b=Widget 'Control-Bms' $op.Id
        if($b.action.tagId-ne"SITE_BAU.point_$($op.I)"-or$b.action.value-ne$op.V-or-not$b.action.requiresConfirmation){throw 'BAU command escaped its explicit route or protocol value'}
        if($null-ne$b.properties.PSObject.Properties['controlFeedbackTag']){throw 'Command-register echo must not certify physical BMS completion'}
    }
    $bms=($screens|Where-Object screenId -eq 'Control-Bms').widgets
    if(@($bms|Where-Object {$null-ne$_.action-and$_.action.type-eq'writeSetpoint'}).Count-ne3){throw 'Unexpected subordinate or extra BMS commands'}
    if((Widget 'Control-Bms' 'bms-status').bindings[0].tagId-ne'SITE_BAU.point_34150'){throw 'BAU status borrowed BCU data'}
    $bau.points[0].write.enable=$false;$bau.points[1].address=16387
    Generate 'bau-invalid-route' 'SITE_BAU'
    foreach($id in @('bau-power-on','bau-power-off','bau-reboot')){
        if($null-ne(Widget 'Control-Bms' $id).action){throw 'Disabled/mismatched BAU command fell back to Pack BCU'}
    }
    Write-Host 'PASS: protocol controls, semantic enums, numeric bounds, breaker feedback, disabled routes'
}finally{
    if($temp.StartsWith([IO.Path]::GetTempPath())-and(Test-Path -LiteralPath $temp)){Remove-Item -LiteralPath $temp -Recurse -Force}
}
