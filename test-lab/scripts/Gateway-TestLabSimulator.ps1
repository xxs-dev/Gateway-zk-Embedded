param(
    [ValidateSet('Start', 'Stop', 'Status', 'Scenario', 'Fault', 'Recover', 'SetValue')]
    [string]$Action = 'Status',
    [string]$Binary = "$PSScriptRoot\gateway-test-lab-sim.exe",
    [string]$RunDirectory = "$PSScriptRoot\..\run-windows",
    [string]$ListenAddress = '0.0.0.0',
    [int]$Port = 15020,
    [ValidateSet('ModbusTcp', 'ModbusRtu', 'Dlt645', 'CanUdp')]
    [string]$Protocol = 'ModbusTcp',
    [string]$SerialDevice = 'COM3',
    [int]$BaudRate = 9600,
    [string]$CanPeerAddress = '127.0.0.1',
    [int]$CanPeerPort = 19011,
    [ValidateSet('N', 'E', 'O')]
    [string]$Parity = 'N',
    [ValidateSet('normal', 'ramp', 'random', 'boundary')]
    [string]$Scenario = 'normal',
    [int]$Slave = 1,
    [ValidateSet('timeout', 'disconnect', 'exception', 'write-timeout', 'write-verify-failed')]
    [string]$FaultMode = 'timeout',
    [ValidateSet('holding', 'input', 'coil', 'discrete')]
    [string]$Area = 'holding',
    [int]$Address = 0,
    [int]$Value = 0
)

$ErrorActionPreference = 'Stop'
$Binary = [IO.Path]::GetFullPath($Binary)
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
$StateFile = Join-Path $RunDirectory 'control.conf'
$StatusFile = Join-Path $RunDirectory 'simulator-status.json'
$PidFile = Join-Path $RunDirectory 'simulator.pid'
$OutLog = Join-Path $RunDirectory 'simulator.out.log'
$ErrorLog = Join-Path $RunDirectory 'simulator.error.log'

function Invoke-Simulator([string[]]$Arguments) {
    if (-not (Test-Path -LiteralPath $Binary)) {
        throw "Simulator binary not found: $Binary"
    }
    & $Binary @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Simulator command failed with exit code $LASTEXITCODE"
    }
}

function Get-OwnedProcess {
    if (-not (Test-Path -LiteralPath $PidFile)) { return $null }
    $processId = [int](Get-Content -LiteralPath $PidFile -Raw)
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $null }
    try {
        if ([IO.Path]::GetFullPath($process.Path) -ne $Binary) { return $null }
    } catch {
        return $null
    }
    return $process
}

New-Item -ItemType Directory -Force -Path $RunDirectory | Out-Null

switch ($Action) {
    'Start' {
        if ($null -ne (Get-OwnedProcess)) { throw 'Simulator is already running.' }
        Invoke-Simulator @('init', '--state-file', $StateFile, '--scenario', $Scenario, '--slaves', '1,2,3')
        if ($Protocol -eq 'ModbusTcp') {
            $arguments = @(
                'serve', '--bind', $ListenAddress, '--port', "$Port",
                '--state-file', $StateFile, '--status-file', $StatusFile
            )
        } elseif ($Protocol -eq 'CanUdp') {
            $arguments = @(
                'serve-can', '--transport', 'udp_test',
                '--udp-bind', $ListenAddress, '--udp-listen-port', "$Port",
                '--udp-peer', $CanPeerAddress, '--udp-peer-port', "$CanPeerPort",
                '--state-file', $StateFile, '--status-file', $StatusFile
            )
        } else {
            $protocolName = if ($Protocol -eq 'ModbusRtu') { 'modbus-rtu' } else { 'dlt645' }
            $arguments = @(
                'serve-serial', '--protocol', $protocolName,
                '--serial-device', $SerialDevice, '--baud-rate', "$BaudRate", '--parity', $Parity,
                '--state-file', $StateFile, '--status-file', $StatusFile
            )
        }
        $process = Start-Process -FilePath $Binary -ArgumentList $arguments -WindowStyle Hidden `
            -RedirectStandardOutput $OutLog -RedirectStandardError $ErrorLog -PassThru
        Set-Content -LiteralPath $PidFile -Value $process.Id -Encoding ascii
        Start-Sleep -Milliseconds 500
        if ($null -eq (Get-OwnedProcess)) { throw "Simulator failed to start. See $ErrorLog" }
        Get-Content -LiteralPath $StatusFile -Raw -ErrorAction SilentlyContinue
    }
    'Stop' {
        $process = Get-OwnedProcess
        if ($null -ne $process) {
            Stop-Process -Id $process.Id
            $process.WaitForExit(5000) | Out-Null
        }
        Remove-Item -LiteralPath $PidFile, $StatusFile -Force -ErrorAction SilentlyContinue
        'simulator=stopped'
    }
    'Status' {
        $process = Get-OwnedProcess
        if ($null -eq $process) { 'simulator=stopped' } else { "simulator=running pid=$($process.Id)" }
        if (Test-Path -LiteralPath $StatusFile) { Get-Content -LiteralPath $StatusFile -Raw }
        if (Test-Path -LiteralPath $StateFile) { Invoke-Simulator @('show', '--state-file', $StateFile) }
    }
    'Scenario' {
        Invoke-Simulator @('scenario', '--state-file', $StateFile, '--name', $Scenario)
    }
    'Fault' {
        Invoke-Simulator @('fault', '--state-file', $StateFile, '--slave', "$Slave", '--mode', $FaultMode)
    }
    'Recover' {
        Invoke-Simulator @('recover', '--state-file', $StateFile, '--slave', "$Slave")
    }
    'SetValue' {
        Invoke-Simulator @(
            'set-value', '--state-file', $StateFile, '--slave', "$Slave",
            '--area', $Area, '--address', "$Address", '--value', "$Value"
        )
    }
}
