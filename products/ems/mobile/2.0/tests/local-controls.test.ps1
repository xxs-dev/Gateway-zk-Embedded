[CmdletBinding()]
param([string]$ArtifactDirectory, [string]$RendererPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\..\common\powershell\Repository.ps1')
$repo = Find-GatewayRepositoryRoot -StartPath $PSScriptRoot
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('gateway-local-ui-' + [Guid]::NewGuid().ToString('N'))
$baseline = Join-Path $repo 'products\ems\mobile\2.0\scada\base-project'
function Save-Json($Path, $Value) {
    [void](New-Item -ItemType Directory -Path (Split-Path $Path) -Force)
    ConvertTo-Json -InputObject $Value -Depth 60 | Set-Content -LiteralPath $Path -Encoding utf8
}
function Require($Condition, $Message) { if (-not $Condition) { throw $Message } }
try {
    $source = Join-Path $fixture 'source'
    Copy-Item -LiteralPath $baseline -Destination $source -Recurse
    $sourcePermissions = Get-Content (Join-Path $source 'permissions.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $salt = '0123456789abcdef0123456789abcdef'
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($salt+':offline-test-only'))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
    $existingAccess = [pscustomobject]@{sessionTimeoutSeconds=777;protectedScreenPrefixes=@('Control-');
        users=@(@{username='operator';salt=$salt;passwordSha256=$hash;roles=@('operator')},
            @{username='service';salt=$salt;passwordSha256=$hash;roles=@('maintainer')})}
    $sourcePermissions | Add-Member -NotePropertyName localAccess -NotePropertyValue $existingAccess -Force
    Save-Json (Join-Path $source 'permissions.json') $sourcePermissions
    # Synthetic offline metadata, never used for runtime/strategy deployment.
    $tags = Get-Content (Join-Path $baseline 'tags.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $routes = Get-Content (Join-Path $baseline 'runtime-map.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $tagById = @{}; foreach ($tag in $tags) { $tagById[$tag.tagId] = $tag }
    $devices = Join-Path $fixture 'runtime\devices'
    $allPoints = @{}
    foreach ($group in ($routes | Group-Object sharedMemoryName)) {
        $meters = @()
        foreach ($meter in ($group.Group | Group-Object { $tagById[$_.tagId].meterCode })) {
            $points = @()
            foreach ($route in $meter.Group) {
                $tag = $tagById[$route.tagId]
                $write = @{enable=[bool]$route.writable; dataType='float64'}
                if ([uint32]$route.index -in (1318..1320)) {
                    $write.min=-41.6; $write.max=41.6; $write.step=0.1
                }
                $point = [pscustomobject]@{index=[uint32]$route.index; pointCode=$tag.pointCode;
                    name=$tag.displayName; enabled=$true;
                    read=@{enable=$true;dataType='float64';unit=$tag.unit}; write=[pscustomobject]$write}
                $points += $point; $allPoints[[uint32]$route.index] = $point
            }
            $meters += @{meterCode=$meter.Name;points=$points}
        }
        Save-Json (Join-Path $devices ($group.Name+'.json')) @{memoryStore=@{sharedMemoryName=$group.Name};meters=$meters}
    }
    $codes = @('local_control_mode','local_manual_a_kw','local_manual_b_kw','local_manual_c_kw',
        'grid_reserve_status','local_control_applied','local_final_active_a','local_final_active_b',
        'local_final_active_c','grid_reserve_phase_kw')
    $localPoints = @(foreach ($i in 0..9) {
        $write = @{enable=($i -in @(0,1,2,3,9));dataType='float64';min=0;max=4;step=1}
        if ($i -in 1..3) { $write.min=-41.6; $write.max=41.6; $write.step=0.1 }
        [pscustomobject]@{index=740100+$i;pointCode=$(if($i -in 4..8){"grid_reserve_$(740100+$i)"}else{$codes[$i]});name=$codes[$i];enabled=$true;
            initialValue=$(if($i -eq 9){1}else{0});
            read=@{enable=$true;dataType='float64';unit=''};write=[pscustomobject]$write}
    })
    $localPath = Join-Path $devices 'local-contract.json'
    $localConfig = @{memoryStore=@{sharedMemoryName='gateway_point_store_ems_virtual'};
        meters=@(@{meterCode='EMS_CORE';points=$localPoints})}
    Save-Json $localPath $localConfig
    $args = @{SourceProjectDirectory=$source;RuntimeConfigDirectory=(Join-Path $fixture 'runtime');
        OutputProjectDirectory=(Join-Path $fixture 'project');OutputPackage=(Join-Path $fixture 'candidate.kyscada');
        MachineCode='COMM202600104';LocalOperatorPassword=(ConvertTo-SecureString 'offline-test-only' -AsPlainText -Force);
        EnableLocalControls=$true}
    $generator = Join-Path $repo 'products\ems\mobile\2.0\tools\generate-scada.ps1'
    & $generator @args
    & (Join-Path $repo 'tools\release\validate-ems-package.ps1') -Package $args.OutputPackage -Product mobile -ExpectedMachineCode COMM202600104
    $screens = @(Get-ChildItem (Join-Path $args.OutputProjectDirectory 'screens') -Filter '*.json' | ForEach-Object {
        Get-Content $_.FullName -Raw -Encoding utf8 | ConvertFrom-Json
    })
    Require ($screens.Count -eq 35) 'Expected 35 screens with explicit opt-in'
    $page = @($screens | Where-Object screenId -eq 'LocalControl')[0]
    $outputRoutes=Get-Content (Join-Path $args.OutputProjectDirectory 'runtime-map.json') -Raw -Encoding utf8 | ConvertFrom-Json
    foreach($i in 4..8) {
        $route=@($outputRoutes | Where-Object index -eq (740100+$i))[0]
        Require ($route.tagId -eq "EMS_CORE.$($codes[$i])" -and -not $route.writable) 'Historical output alias broke the fixed read-only route'
    }
    Require ($page.width -eq 1920 -and $page.height -eq 1080) 'Wrong viewport'
    $permissions = Get-Content (Join-Path $args.OutputProjectDirectory 'permissions.json') -Raw -Encoding utf8 | ConvertFrom-Json
    Require ('LocalControl' -in $permissions.localAccess.protectedScreenPrefixes) 'LocalControl is unprotected'
    Require (($permissions.localAccess.users | ConvertTo-Json -Depth 10 -Compress) -eq
        ($existingAccess.users | ConvertTo-Json -Depth 10 -Compress)) 'Existing accounts or roles changed'
    Require ($permissions.localAccess.sessionTimeoutSeconds -eq 777) 'Existing session timeout changed'
    $buttons = @($page.widgets | Where-Object widgetId -like 'local-mode-?' )
    Require ($buttons.Count -eq 4) 'Four mode actions required'
    foreach ($button in $buttons) {
        Require ($button.action.tagId -eq 'EMS_CORE.local_control_mode' -and
            $button.action.requiresConfirmation -and -not $button.action.highPriority) 'Mode action bypasses normal confirmation'
    }
    $inputs = @($page.widgets | Where-Object type -eq 'qtInput')
    Require ($inputs.Count -eq 3) 'Three editable phase inputs required'
    foreach ($input in $inputs) {
        Require ($input.properties.controlPauseGuard -eq 'true' -and
            $input.properties.pauseGuardModeTag -eq 'EMS_CORE.local_control_mode' -and
            $input.properties.pauseGuardAppliedTag -eq 'EMS_CORE.local_control_applied' -and
            $input.action.requiresConfirmation -and -not $input.action.highPriority) 'Manual input lacks pause/confirmation guard'
    }
    Require (@($screens.widgets | Where-Object type -eq 'pcsPhasePowerControl').Count -eq 0) 'Direct PCS power controller remains'
    $routeByTag = @{}; foreach ($r in $routes) { $routeByTag[$r.tagId] = $r.index }
    foreach ($widget in $screens.widgets) {
        if ($null -ne $widget.action -and $widget.action.type -in @('writeSetpoint','pulse','toggle')) {
            Require ($routeByTag[[string]$widget.action.tagId] -notin (1318..1323)) 'Direct power action remains'
        }
    }
    $pcs = @($screens | Where-Object screenId -eq 'Control-Pcs')[0]
    Require (@($pcs.widgets | Where-Object widgetId -eq 'control-pcs-stop').Count -eq 1) 'Independent PCS stop removed'
    $stop = @($pcs.widgets | Where-Object widgetId -eq 'control-pcs-stop')[0]
    Require ($stop.properties.controlPauseGuard -eq 'true' -and
        $stop.properties.pauseGuardModeTag -eq 'EMS_CORE.local_control_mode' -and
        $stop.properties.pauseGuardAppliedTag -eq 'EMS_CORE.local_control_applied') 'PCS stop lacks pause guard'
    foreach ($widget in $page.widgets) {
        $g = $widget.geometry
        Require ($g.x -ge 0 -and $g.y -ge 0 -and ($g.x+$g.width) -le 1920 -and ($g.y+$g.height) -le 1080) "Out-of-bounds widget: $($widget.widgetId)"
    }
    $foreground = @($page.widgets | Where-Object { $_.geometry.y -ge 104 -and $_.type -ne 'qtFrame' })
    for ($i=0; $i -lt $foreground.Count; $i++) {
        for ($j=$i+1; $j -lt $foreground.Count; $j++) {
            $a=$foreground[$i].geometry; $b=$foreground[$j].geometry
            $overlap=$a.x -lt ($b.x+$b.width) -and ($a.x+$a.width) -gt $b.x -and
                $a.y -lt ($b.y+$b.height) -and ($a.y+$a.height) -gt $b.y
            Require (-not $overlap) "Overlapping content: $($foreground[$i].widgetId)/$($foreground[$j].widgetId)"
        }
    }
    if ($ArtifactDirectory) {
        Require (-not (Test-Path -LiteralPath $ArtifactDirectory)) 'Artifact directory already exists'
        [void](New-Item -ItemType Directory -Path $ArtifactDirectory)
        Copy-Item -LiteralPath $args.OutputProjectDirectory -Destination (Join-Path $ArtifactDirectory 'scada-project') -Recurse
        Copy-Item -LiteralPath $args.OutputPackage -Destination (Join-Path $ArtifactDirectory 'offline-candidate.kyscada')
        Save-Json (Join-Path $ArtifactDirectory 'validation.json') @{kind='offline-synthetic-ui-only';
            runtimeDeployable=$false;requiresQtPauseGuard=$true;modeDefault=0;reservePhaseDefault=1}
    }
    if ($RendererPath) {
        Require ([bool]$ArtifactDirectory) 'Renderer requires an artifact directory'
        $savedPlatform=$env:QT_QPA_PLATFORM; $savedUser=$env:KY_SCADA_RENDER_USERNAME; $savedPassword=$env:KY_SCADA_RENDER_PASSWORD
        $savedFonts=$env:QT_QPA_FONTDIR
        try {
            $fonts=Join-Path $fixture 'fonts'
            [void](New-Item -ItemType Directory -Path $fonts)
            Copy-Item -LiteralPath (Join-Path $env:WINDIR 'Fonts\simhei.ttf') -Destination $fonts
            $env:QT_QPA_FONTDIR=$fonts
            $env:QT_QPA_PLATFORM='offscreen';$env:KY_SCADA_RENDER_USERNAME='operator';$env:KY_SCADA_RENDER_PASSWORD='offline-test-only'
            foreach ($screenId in @('LocalControl','Control-Pcs','Strategy-Overview')) {
                $png = Join-Path $ArtifactDirectory ($screenId+'.png')
                & $RendererPath --project-dir $args.OutputProjectDirectory --machine-code COMM202600104 --screen-id $screenId --screenshot $png
                Require ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $png)) "Qt render failed: $screenId"
            }
        } finally {
            $env:QT_QPA_PLATFORM=$savedPlatform;$env:KY_SCADA_RENDER_USERNAME=$savedUser;$env:KY_SCADA_RENDER_PASSWORD=$savedPassword
            $env:QT_QPA_FONTDIR=$savedFonts
        }
    }
    # Generation must fail before publishing a package with unsafe defaults/limits.
    $localPoints[0].initialValue = 1
    Save-Json $localPath $localConfig
    $rejected=$false; try { & $generator @args } catch { $rejected=$_.Exception.Message -like '*default*' }
    Require $rejected 'Enabled mode default was accepted'
    $localPoints[0].initialValue=0; $localPoints[1].write.min=-99
    Save-Json $localPath $localConfig
    $rejected=$false; try { & $generator @args } catch { $rejected=$_.Exception.Message -like '*limits*' }
    Require $rejected 'Inconsistent physical/manual limits were accepted'
    $localPoints[1].write.min=-41.6
    $localPoints[1].write.PSObject.Properties.Remove('max')
    Save-Json $localPath $localConfig
    $rejected=$false; try { & $generator @args } catch { $rejected=$_.Exception.Message -like '*limits*' }
    Require $rejected 'Unknown manual limits were accepted'
    Write-Host 'local-controls.test passed (Qt pause guard required)'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolved -Leaf) -like 'gateway-local-ui-*' -and (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
