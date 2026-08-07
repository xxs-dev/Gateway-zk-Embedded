param(
    [string]$PackageDirectory = "$PSScriptRoot\..\dist-windows"
)

$ErrorActionPreference = 'Stop'
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$runner = Join-Path $PackageDirectory 'Gateway-TestLabSimulator.ps1'
$checksumFile = Join-Path $PackageDirectory 'SHA256SUMS.txt'
$runDirectory = Join-Path $env:TEMP "gateway-test-lab-smoke-$([Guid]::NewGuid().ToString('N'))"

function Read-Exact([IO.Stream]$Stream, [byte[]]$Buffer, [int]$Offset, [int]$Count) {
    $received = 0
    while ($received -lt $Count) {
        $current = $Stream.Read($Buffer, $Offset + $received, $Count - $received)
        if ($current -le 0) { throw 'Modbus connection closed before a complete frame was received.' }
        $received += $current
    }
}

function Read-Modbus([int]$Port, [int]$TransactionId) {
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $client.ReceiveTimeout = 2000
        $client.SendTimeout = 2000
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $request = [byte[]]@(
            [byte]($TransactionId -shr 8), [byte]$TransactionId,
            0, 0, 0, 6, 1, 3, 0, 0, 0, 2
        )
        $stream.Write($request, 0, $request.Length)

        $header = New-Object byte[] 7
        Read-Exact $stream $header 0 $header.Length
        $remaining = (([int]$header[4] -shl 8) -bor [int]$header[5]) - 1
        if ($remaining -lt 2 -or $remaining -gt 254) {
            throw "Invalid Modbus response length: $remaining"
        }
        $payload = New-Object byte[] $remaining
        Read-Exact $stream $payload 0 $remaining
        return [byte[]]($header + $payload)
    } finally {
        $client.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $runner)) { throw "Runner not found: $runner" }
if (-not (Test-Path -LiteralPath $checksumFile)) { throw "Checksum file not found: $checksumFile" }

$checksums = Get-Content -LiteralPath $checksumFile
if ($checksums | Where-Object { $_ -match '\sSHA256SUMS\.txt$' }) {
    throw 'SHA256SUMS.txt must not contain a checksum for itself.'
}
foreach ($line in $checksums) {
    if ($line -notmatch '^([0-9a-f]{64})\s{2}(.+)$') { throw "Invalid checksum line: $line" }
    $path = Join-Path $PackageDirectory $Matches[2]
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) { throw "Checksum mismatch: $path" }
}

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

try {
    & $runner -Action Start -Protocol ModbusTcp -Port $port -RunDirectory $runDirectory | Out-Null

    $initial = Read-Modbus $port 1
    if ($initial.Length -lt 13 -or $initial[7] -ne 3) {
        throw "Unexpected initial Modbus response: $($initial -join ',')"
    }

    & $runner -Action SetValue -Slave 1 -Area holding -Address 0 -Value 4321 `
        -RunDirectory $runDirectory | Out-Null
    Start-Sleep -Milliseconds 400
    $changed = Read-Modbus $port 2
    $value = (([int]$changed[9] -shl 8) -bor [int]$changed[10])
    if ($value -ne 4321) { throw "Expected 4321, got $value" }

    & $runner -Action Fault -Slave 1 -FaultMode exception -RunDirectory $runDirectory | Out-Null
    Start-Sleep -Milliseconds 400
    $faulted = Read-Modbus $port 3
    if ($faulted[7] -ne 0x83) {
        throw "Expected Modbus exception response, got $($faulted -join ',')"
    }

    & $runner -Action Recover -Slave 1 -RunDirectory $runDirectory | Out-Null
    Start-Sleep -Milliseconds 400
    $recovered = Read-Modbus $port 4
    if ($recovered[7] -ne 3) {
        throw "Expected recovered response, got $($recovered -join ',')"
    }

    & $runner -Action Stop -RunDirectory $runDirectory | Out-Null

    $canReceiver = [Net.Sockets.UdpClient]::new(0)
    $canSimulatorProbe = [Net.Sockets.UdpClient]::new(0)
    try {
        $canReceiver.Client.ReceiveTimeout = 3000
        $canPeerPort = ([Net.IPEndPoint]$canReceiver.Client.LocalEndPoint).Port
        $canSimulatorPort = ([Net.IPEndPoint]$canSimulatorProbe.Client.LocalEndPoint).Port
        $canSimulatorProbe.Dispose()
        $canSimulatorProbe = $null

        & $runner -Action Start -Protocol CanUdp -ListenAddress '127.0.0.1' `
            -Port $canSimulatorPort -CanPeerAddress '127.0.0.1' -CanPeerPort $canPeerPort `
            -RunDirectory $runDirectory | Out-Null

        $source = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        $telemetry = $canReceiver.Receive([ref]$source)
        if ($telemetry.Length -ne 20 -or
            [Text.Encoding]::ASCII.GetString($telemetry, 0, 4) -ne 'GCAN' -or
            $telemetry[5] -band 1 -eq 0) {
            throw "Unexpected virtual CAN telemetry: $($telemetry -join ',')"
        }

        $writeFrame = [byte[]]@(
            0x47, 0x43, 0x41, 0x4E, 1, 0, 1, 0,
            0, 0, 3, 0x21, 1
        )
        $canReceiver.Send($writeFrame, $writeFrame.Length, '127.0.0.1', $canSimulatorPort) | Out-Null
        Start-Sleep -Milliseconds 400
        $canStatus = & $runner -Action Status -RunDirectory $runDirectory | Out-String
        if ($canStatus -notmatch '"receivedFrames":([1-9]|[1-9][0-9]+)') {
            throw "Virtual CAN simulator did not receive the write frame: $canStatus"
        }
    } finally {
        if ($null -ne $canReceiver) { $canReceiver.Dispose() }
        if ($null -ne $canSimulatorProbe) { $canSimulatorProbe.Dispose() }
    }

    Write-Output "gateway_test_lab_windows_smoke passed modbusPort=$port value=$value canUdp=true"
} finally {
    & $runner -Action Stop -RunDirectory $runDirectory -ErrorAction SilentlyContinue | Out-Null
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if ([IO.Path]::GetFullPath($runDirectory).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $runDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
