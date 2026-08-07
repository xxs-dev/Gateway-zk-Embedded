param(
    [string]$DeviceAddress = '192.168.22.16',
    [int]$Port = 19443,
    [switch]$Watch,
    [ValidateRange(200, 60000)]
    [int]$IntervalMs = 1000,
    [string]$MeterCode = '',
    [string]$PointCode = ''
)

$ErrorActionPreference = 'Stop'
$baseUri = "http://${DeviceAddress}:$Port"

do {
    $health = Invoke-RestMethod -Uri "$baseUri/api/v1/health" -TimeoutSec 5 -NoProxy
    if (-not $health.success) {
        throw "Test-lab SystemMonitor is not healthy: $($health.message)"
    }

    $reply = Invoke-RestMethod -Uri "$baseUri/api/v1/realtime/points" -TimeoutSec 5 -NoProxy
    $points = @($reply.points)
    if ($MeterCode) {
        $points = @($points | Where-Object meterCode -eq $MeterCode)
    }
    if ($PointCode) {
        $points = @($points | Where-Object pointCode -like "*$PointCode*")
    }

    if ($Watch) { Clear-Host }
    Write-Output "测试实验室：$DeviceAddress`:$Port  点数：$($points.Count)  刷新：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff')"
    $points |
        Sort-Object meterCode, index |
        Select-Object index, meterCode, pointCode, value, quality, stale,
            @{Name='更新时间'; Expression={
                [DateTimeOffset]::FromUnixTimeMilliseconds([long]$_.ts).ToLocalTime().ToString('HH:mm:ss.fff')
            }} |
        Format-Table -AutoSize

    if ($Watch) { Start-Sleep -Milliseconds $IntervalMs }
} while ($Watch)
