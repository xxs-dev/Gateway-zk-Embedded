# UI-only integration. Runtime configuration and strategy graphs remain external.
function Assert-LocalControlContract {
    if ($MachineCode -notin @('COMM202600104', 'COMM202600999')) {
        throw 'Local controls are only supported for COMM104 and the offline COMM999 fixture'
    }
    $codes = @('local_control_mode', 'local_manual_a_kw', 'local_manual_b_kw',
        'local_manual_c_kw', 'grid_reserve_status', 'local_control_applied',
        'local_final_active_a', 'local_final_active_b', 'local_final_active_c', 'grid_reserve_phase_kw')
    foreach ($offset in 0..9) {
        $index = [uint32](740100 + $offset)
        if (-not (Has-Index $index) -or -not $runtimePointByIndex.ContainsKey($index)) {
            throw "Local control runtime contract missing: $index"
        }
        $tag = Tag-At $index
        # The historical offline runtime labels these computed outputs by index.
        # Publish the agreed SCADA names without modifying any runtime point.
        if ($offset -in (4..8) -and $tag.pointCode -eq "grid_reserve_$index" -and
            -not (Is-WritableIndex $index)) {
            $oldTagId = [string]$tag.tagId
            $canonicalTagId = "$($tag.meterCode).$($codes[$offset])"
            if ($tagById.ContainsKey($canonicalTagId)) { throw "Local output alias conflict: $index" }
            $tag.pointCode = $codes[$offset]
            $tag.tagId = $canonicalTagId
            $mappingByIndex[$index].tagId = $canonicalTagId
            $tagById.Remove($oldTagId)
            $mappingByTagId.Remove($oldTagId)
            $tagById[$canonicalTagId] = $tag
            $mappingByTagId[$canonicalTagId] = $mappingByIndex[$index]
        }
        if ($tag.pointCode -ne $codes[$offset] -or
            $mappingByIndex[$index].sharedMemoryName -ne 'gateway_point_store_ems_virtual') {
            throw "Local control route conflict: $index"
        }
        $writable = $offset -in @(0,1,2,3,9)
        if ((Is-WritableIndex $index) -ne $writable) { throw "Local control access conflict: $index" }
        if ($offset -in @(0,1,2,3,9)) {
            $point = $runtimePointByIndex[$index].point
            $expectedDefault = if ($offset -eq 9) { 1 } else { 0 }
            if ($null -eq $point.PSObject.Properties['initialValue'] -or
                [double]$point.initialValue -ne $expectedDefault) {
                throw "Local control default must be $expectedDefault : $index"
            }
        }
    }
    foreach ($offset in 0..2) {
        $physical = [uint32](1318 + $offset)
        $virtual = [uint32](740101 + $offset)
        if (-not $runtimePointByIndex.ContainsKey($physical)) { throw "PCS limits missing: $physical" }
        $limit = $runtimePointByIndex[$physical].point.write
        $manual = $runtimePointByIndex[$virtual].point
        foreach ($field in @('min','max')) {
            if ($null -eq $limit.PSObject.Properties[$field] -or
                $null -eq $manual.write.PSObject.Properties[$field] -or
                [double]::IsNaN([double]$limit.$field) -or [double]::IsInfinity([double]$limit.$field) -or
                [double]$limit.$field -ne [double]$manual.write.$field) {
                throw "PCS/manual write limits missing or inconsistent: $physical/$virtual/$field"
            }
        }
        if ($null -eq $manual.write.PSObject.Properties['step'] -or
            [double]::IsNaN([double]$manual.write.step) -or [double]::IsInfinity([double]$manual.write.step) -or
            [double]$manual.write.step -le 0 -or
            ($null -ne $limit.PSObject.Properties['step'] -and [double]$limit.step -ne [double]$manual.write.step)) {
            throw "Manual write step missing or inconsistent with PCS limits: $virtual"
        }
        if ([double]$limit.min -gt 0 -or [double]$limit.max -lt 0 -or
            [double]$limit.min -ge [double]$limit.max) {
            throw "PCS limits invalid: $physical"
        }
    }
}

function New-LocalControlScreen {
    $widgets = [System.Collections.Generic.List[object]]::new()
    Add-CompactChrome $widgets 'Control'
    $widgets.Add((New-Text 'local-title' '策略与三相功率' 32 126 1100 48 32 '#F3F8FA' 'Left' $true 4))
    $widgets.Add((New-NavigationButton 'local-back' 'PCS 启停与监测' 'Control-Pcs' $false 1450 126 438 58))
    $widgets[$widgets.Count-1].properties.qtFontSize = 24
    $modeMap = '{"0":"暂停归零","1":"原有自动","2":"手动三相","3":"并网保电"}'
    $statusMap = '{"0":"停用","1":"补电中","2":"零功率待机","3":"等待并网","4":"数据/安全闭锁"}'
    $modes = @('暂停策略并归零','启用原有自动','启用手动三相','启用并网保电')
    for ($i=0; $i -lt 4; $i++) {
        $button = New-ControlButton "local-mode-$i" $modes[$i] 740100 'writeSetpoint' ([string]$i) (32+$i*470) 204 446 76 $false '#24483F'
        $button.properties.qtFontSize = 26
        $widgets.Add($button)
    }
    Add-CompactMetricCard $widgets 'local-mode' '设置模式' 740100 32 302 604 142 '' '#55D8E8' $modeMap
    Add-CompactMetricCard $widgets 'local-applied' '计算侧已读取模式' 740105 658 302 604 142 '' '#55E0AA' $modeMap
    Add-CompactMetricCard $widgets 'local-reserve' '并网保电状态' 740104 1284 302 604 142 '' '#F9CC44' $statusMap
    $widgets.Add((New-Text 'local-manual-title' '三相手动设定 (kW，正充负放)' 32 470 1300 40 28 '#F3F8FA' 'Left' $true 4))
    for ($i=0; $i -lt 3; $i++) {
        $x = 32 + $i*626
        $phase = @('A','B','C')[$i]
        $widgets.Add((New-Text "local-manual-$phase-title" "$phase 相手动设定 (kW)" $x 518 604 36 24 '#9FC0CC' 'Center' $false 4))
        $index = [uint32](740101+$i)
        $input = New-Widget "local-manual-$phase-input" 'qtInput' "$phase 相手动设定" $x 566 604 104 5 `
            @((New-Binding $index 'value')) @() (New-Action 'writeSetpoint' '' $index '' $true $false) `
            ([ordered]@{qtText='';qtTextAlignment='Center';qtVerticalAlignment='Center';
                qtFontSize=38;qtTextColor='#F3F8FA';qtBackgroundColor='#123847';qtTransparent=$false;
                controlPauseGuard='true';pauseGuardModeTag=(Tag-At 740100).tagId;
                pauseGuardAppliedTag=(Tag-At 740105).tagId})
        $widgets.Add($input)
        Add-CompactMetricCard $widgets "local-final-$phase" "$phase 相安全门后指令" ([uint32](740106+$i)) $x 696 604 144 'kW' '#55E0AA'
    }
    $widgets.Add((New-Text 'local-input-guard' '手动功率：先暂停归零，设置三相后启用手动' 32 856 1500 36 24 '#F9CC44' 'Left' $true 4))
    $widgets.Add((New-Text 'local-reserve-rule' '并网保电：SOC < 99% 开始补电，持续至满电后 0 功率待机' 32 906 1440 38 26 '#F3F8FA' 'Left' $false 4))
    $widgets.Add((New-Text 'local-reserve-power-label' '补电每相 kW' 1494 898 394 30 22 '#9FC0CC' 'Center' $false 4))
    $widgets.Add((New-BoundText 'local-reserve-power' 740109 1494 934 394 54 36 '#F3F8FA' 'Center' ''))
    $widgets.Add((New-Text 'local-standby-note' '满电保持启用，无需再次开机；PCS 停机为独立操作' 32 956 1440 38 26 '#F3F8FA' 'Left' $false 4))
    return New-Screen 'LocalControl' '策略与三相功率' $widgets
}

function Add-LocalControlScreens([object[]]$Screens) {
    foreach ($screen in $Screens) {
        $screen.widgets = @($screen.widgets | Where-Object {
            if ($_.type -eq 'pcsPhasePowerControl') { return $false }
            if ($null -ne $_.action -and $_.action.type -in @('writeSetpoint','pulse','toggle')) {
                $route = $mappingByTagId[[string]$_.action.tagId]
                if ($null -ne $route -and [uint32]$route.index -in (1318..1323)) { return $false }
            }
            return $true
        })
        if ($screen.screenId -eq 'Control-Pcs') {
            foreach ($widget in $screen.widgets) {
                if ($widget.widgetId -eq 'control-pcs-stop') {
                    $widget.properties | Add-Member -NotePropertyName controlPauseGuard -NotePropertyValue 'true' -Force
                    $widget.properties | Add-Member -NotePropertyName pauseGuardModeTag -NotePropertyValue (Tag-At 740100).tagId -Force
                    $widget.properties | Add-Member -NotePropertyName pauseGuardAppliedTag -NotePropertyValue (Tag-At 740105).tagId -Force
                }
            }
            $screen.widgets = @($screen.widgets | Where-Object {
                $_.geometry.y -lt 376 -or $_.geometry.y -ge 1040
            })
            $screen.widgets += New-NavigationButton 'local-controls-entry' '策略 / 三相手动功率' 'LocalControl' $false 32 390 1856 112
            $screen.widgets[-1].properties.qtFontSize = 30
            $screen.widgets[-1].properties.qtBackgroundColor = '#14576B'
            $screen.widgets[-1].properties.qtTransparent = $false
            $screen.widgets += New-Text 'local-pcs-note' '0 功率待机不等于 PCS 停机' 32 538 1856 60 32 '#F3F8FA' 'Center' $true 4
            $feedback = [System.Collections.Generic.List[object]]::new()
            Add-CompactMetricCard $feedback 'local-pcs-actual' 'PCS 实际总有功' 1230 32 644 916 220 'kW'
            Add-CompactMetricCard $feedback 'local-pcs-soc' '电池 SOC' 1569 972 644 916 220 '%'
            $screen.widgets += @($feedback)
        }
        if ($screen.screenId -eq 'Strategy-Overview') {
            foreach ($widget in $screen.widgets) {
                if ($widget.widgetId -eq 'strategy-current-plan-title') {
                    $widget.title = '策略候选功率'
                    $widget.properties.qtText = '策略候选功率'
                }
            }
            $screen.widgets += New-NavigationButton 'local-strategy-entry' '策略启停' 'LocalControl' $false 1570 118 318 62
            $screen.widgets[-1].properties.qtFontSize = 24
            $screen.widgets[-1].properties.qtBackgroundColor = '#14576B'
            $screen.widgets[-1].properties.qtTransparent = $false
        }
        $screen
    }
    New-LocalControlScreen
}
