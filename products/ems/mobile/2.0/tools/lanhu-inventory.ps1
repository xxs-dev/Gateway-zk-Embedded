# Inventory is configuration-based: an offline device remains in the UI.
function Get-LanhuDevicePoint($Points,$Name) {
    $found=@($Points|Where-Object displayName -eq $Name)
    if($found.Count-gt1){throw "Ambiguous device point: $Name"}
    if($found.Count-eq1){return [uint32]$found[0].indexFallback}
    return $null
}
function Get-LanhuCellScreenId($Cluster=1,$Page=1) {
    $id=if($Cluster-eq1){'Devices-Cells'}else{"Devices-Cells-$Cluster"}
    if($Page-gt1){$id+="-Page$Page"}
    return $id
}
function Get-LanhuPageId($Base,$Page) {
    if($Page-eq1){return $Base};return "$Base-Page$Page"
}
function Get-LanhuBauPoint($Address,$Writable=$false,$Name='') {
    if($null-eq$script:lanhuBau){return $null}
    $matches=@($script:lanhuBau.points|Where-Object {
        $null-ne$_.PSObject.Properties['address']-and$_.address-eq$Address-and
        ($Name-eq''-or$_.name-eq$Name)
    })
    if($matches.Count-ne1){return $null}
    $p=$matches[0];$index=[uint32]$p.index
    if($null-ne$p.PSObject.Properties['enabled']-and-not$p.enabled){return $null}
    if(-not(Has-Index $index)-or$script:runtimePointByIndex[$index].meterCode-ne$BauMeterCode){return $null}
    if($Writable){
        if(-not(Is-WritableIndex $index)-or$p.write.function-ne6-or$p.write.dataType-ne'uint16'-or
            $p.write.scale-ne1-or$p.write.offset-ne0-or$p.write.length-ne1){return $null}
    }elseif(-not$p.read.enable-or$p.read.function-ne4){return $null}
    return $index
}
function Initialize-LanhuInventory {
    $script:lanhuClusters=@();$script:lanhuMeters=@();$script:lanhuBreakers=@()
    $script:lanhuBau=$null
    # Site-confirmed role is explicit. Never infer BAU from a slave address or legacy index.
    if(-not[string]::IsNullOrWhiteSpace($BauMeterCode)){
        if(-not$runtimeDeviceByCode.ContainsKey($BauMeterCode)){throw "Configured BAU meter is missing: $BauMeterCode"}
        $bau=$runtimeDeviceByCode[$BauMeterCode]
        if($bau.protocolType-ne'modbus_rtu'-or$bau.slave-lt1-or$bau.slave-gt247){throw 'BAU requires an explicit Modbus RTU route'}
        $script:lanhuBau=$bau
    }
    foreach($group in @($tags|Group-Object deviceId|Sort-Object Name)){
        $points=@($group.Group);$device=$null
        if($runtimeDeviceByCode.ContainsKey($group.Name)){$device=$runtimeDeviceByCode[$group.Name]}
        $name=$group.Name
        if($null-ne$device-and$null-ne$device.PSObject.Properties['deviceName']){$name=[string]$device.deviceName}
        $cells=@{};$probes=@{}
        foreach($point in $points){
            if($point.displayName-match'^单体电压0*([1-9][0-9]*)$'){$cells[[int]$Matches[1]]=[uint32]$point.indexFallback}
            if($point.displayName-match'^单体温度0*([1-9][0-9]*)$'){$probes[[int]$Matches[1]]=[uint32]$point.indexFallback}
        }
        if($cells.Count){
            $count=[int]($cells.Keys|Measure-Object -Maximum).Maximum
            $script:lanhuClusters+=@{DeviceId=$group.Name;Points=$points;Cells=$cells;Probes=$probes;Count=$count;Pages=[int][math]::Ceiling($count/32)}
        }
        $isBreaker=$name-match'断路器'-or$group.Name-match'_BREAKER$'
        if($isBreaker){
            $position=Get-LanhuDevicePoint $tags ($name+'合位')
            $trip=@($points|Where-Object pointCode -eq 'breaker_remote_trip')
            $close=@($points|Where-Object pointCode -eq 'breaker_remote_close')
            $script:lanhuBreakers+=@{Name=$name;DeviceId=$group.Name;Position=$position;Online=(Get-LanhuDevicePoint $points '断路器通讯状态');Trip=$(if($trip.Count-eq1){$trip[0].indexFallback}else{$null});Close=$(if($close.Count-eq1){$close[0].indexFallback}else{$null})}
        }
        # This mobile meter profile is identified by its energy register, not by being online.
        if(-not$isBreaker-and$null-ne(Get-LanhuDevicePoint $points '当前组合有功总电能')){
            $source='Meter'+($script:lanhuMeters.Count+1);$target="Meters-$source"
            foreach($legacy in @(@{I=1030;S='Output';T='Meters-Overview';N='防逆流表'},@{I=1130;S='Input';T='Meters-Electrical-Input';N='并网点电表'},@{I=9030;S='Through';T='Meters-Power-Through';N='负荷电表'})){
                if($legacy.I-in$points.indexFallback){$source=$legacy.S;$target=$legacy.T;$name=$legacy.N;break}
            }
            $indexes=@{}
            foreach($quantity in @(@{K='V';N='电压'},@{K='I';N='电流'},@{K='P';N='有功功率'},@{K='Q';N='无功功率'},@{K='S';N='视在功率'})){
                $indexes[$quantity.K]=@(foreach($phase in @('A','B','C')){Get-LanhuDevicePoint $points ($phase+'相'+$quantity.N)})
            }
            foreach($entry in @(@{K='AvgV';N='平均相电压'},@{K='AvgI';N='平均相电流'},@{K='TotalP';N='总有功功率'},@{K='TotalQ';N='总无功功率'},@{K='TotalS';N='总视在功率'},@{K='Factor';N='总功率因数'},@{K='Frequency';N='频率'})){
                $indexes[$entry.K]=Get-LanhuDevicePoint $points $entry.N
            }
            $energy=@(foreach($n in @('当前正向总有功电能','当前反向总有功电能','当前正向总无功电能','当前反向总无功电能')){Get-LanhuDevicePoint $points $n})
            $script:lanhuMeters+=@{DeviceId=$group.Name;Name=$name;Source=$source;Target=$target;Indexes=$indexes;Energy=$energy;Online=(Get-LanhuDevicePoint $points '在线状态')}
        }
    }
    # DIO-only breaker configurations have no separate communication device.
    foreach($point in @($tags|Where-Object displayName -match '^.+断路器合位$')){
        $name=$point.displayName-replace'合位$',''
        if($name-notin@($script:lanhuBreakers|ForEach-Object {$_.Name})){
            $script:lanhuBreakers+=@{Name=$name;DeviceId=$point.deviceId;Position=[uint32]$point.indexFallback;Online=$null;Trip=$null;Close=$null}
        }
    }
    $script:lanhuBreakers=@($script:lanhuBreakers|Sort-Object Name)
    $script:lanhuMeters=@($script:lanhuMeters|Sort-Object Source)
}
function Get-LanhuExpectedScreens {
    $LanhuExpectedScreens|Where-Object {$_-notlike'Devices-Cells*'-and$_-ne'Devices-Meters-Page2'}
    for($c=1;$c-le[math]::Max(1,$lanhuClusters.Count);$c++){
        $pages=if($lanhuClusters.Count){$lanhuClusters[$c-1].Pages}else{1}
        for($p=1;$p-le$pages;$p++){Get-LanhuCellScreenId $c $p}
    }
    for($p=2;$p-le[math]::Ceiling($lanhuMeters.Count/2);$p++){Get-LanhuPageId 'Devices-Meters' $p}
    for($p=2;$p-le[math]::Ceiling($lanhuBreakers.Count/9);$p++){Get-LanhuPageId 'Devices-Mccb' $p;Get-LanhuPageId 'Control-Mccb' $p}
    foreach($meter in $lanhuMeters){if($meter.Target-notin$LanhuExpectedScreens){$meter.Target}}
}
