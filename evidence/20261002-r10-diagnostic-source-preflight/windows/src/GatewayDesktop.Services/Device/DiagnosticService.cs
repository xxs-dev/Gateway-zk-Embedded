using System.Diagnostics;
using GatewayDesktop.Core.Entities;
using GatewayDesktop.Core.Interfaces;
using GatewayDesktop.Infrastructure.Security;
using Microsoft.Extensions.Logging;
using Legacy = GatewayDesktop.Core.Devices;

namespace GatewayDesktop.Services.Device;

/// <summary>
/// 诊断命令服务实现。通过 Legacy <see cref="Legacy.IDeviceTransport.ExecuteDiagAsync"/>
/// 在边端执行预定义诊断命令（uptime/free_m/df_root/top_once/ps_gateway/journal_tail/systemctl_status/cellular_status）。
/// 直连模式附带固定维护口令 SHA256；MQTT 模式传空。
/// </summary>
public class DiagnosticService : IDiagnosticService
{
    private readonly IConnectionService _connectionService;
    private readonly ILogger<DiagnosticService>? _logger;

    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(25);

    public DiagnosticService(
        IConnectionService connectionService,
        ILogger<DiagnosticService>? logger = null)
    {
        _connectionService = connectionService;
        _logger = logger;
    }

    public Task<List<DiagnosticCommand>> GetPredefinedCommandsAsync(CancellationToken cancellationToken = default)
        => Task.FromResult(BuildPredefinedCommands());

    public async Task<CommandResult> ExecuteAsync(
        DiagnosticCommand command,
        string service,
        int tailLines,
        CancellationToken cancellationToken = default)
    {
        var stopwatch = Stopwatch.StartNew();

        var (transport, endpoint) = ResolveLegacy();
        if (transport is null || endpoint is null)
        {
            return Failed("未连接设备，请先在【连接】模块建立连接。", stopwatch.Elapsed);
        }

        var request = new Legacy.DiagRequest(
            $"DIAG_{Guid.NewGuid():N}",
            endpoint.MachineCode,
            Environment.UserName,
            command.Command,
            command.Command == "systemctl_status" ? service?.Trim() ?? string.Empty : string.Empty,
            command.Command == "journal_tail" ? Math.Clamp(tailLines, 1, 500) : 100,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            MaintenanceAuth.ForEndpoint(endpoint));

        try
        {
            _logger?.LogInformation("执行诊断: {Command} service={Service} tailLines={Tail}",
                command.Command, request.Service, request.TailLines);

            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cts.CancelAfter(DefaultTimeout);

            var result = await transport.ExecuteDiagAsync(endpoint, request, cts.Token);
            if (cts.IsCancellationRequested)
                return Failed(cancellationToken.IsCancellationRequested
                    ? "诊断请求已取消；远端是否仍在执行未知。"
                    : "25s 内未取得诊断结果；远端是否仍在执行未知。",
                    stopwatch.Elapsed, request.CmdId, request.MachineCode);

            if (!result.Success || result.Value is null)
            {
                _logger?.LogWarning("诊断失败: {Message}", result.Message);
                return Failed(result.Message, stopwatch.Elapsed, request.CmdId, request.MachineCode);
            }

            var resp = result.Value;
            var expectedArg = request.Command == "journal_tail" ? request.TailLines.ToString() :
                request.Command == "systemctl_status" ? request.Service : string.Empty;
            if (!string.Equals(resp.CmdId, request.CmdId, StringComparison.Ordinal) ||
                !string.Equals(resp.MachineCode, request.MachineCode, StringComparison.Ordinal) ||
                !string.Equals(resp.Command, request.Command, StringComparison.Ordinal) ||
                !string.Equals(resp.Operator, request.Operator, StringComparison.Ordinal) ||
                resp.RequestTs != request.RequestTs ||
                (resp.Arg is not null && !string.Equals(resp.Arg, expectedArg, StringComparison.Ordinal)) ||
                (expectedArg.Length > 0 && resp.Arg is null) ||
                (resp.ReplyKind is not null && resp.ReplyKind != "terminal"))
                return Failed("诊断回复与已发送请求不匹配；远端结果未知。", stopwatch.Elapsed,
                    request.CmdId, request.MachineCode);
            return new CommandResult
            {
                Success = resp.Success,
                Output = resp.Stdout,
                Error = resp.Stderr,
                ExitCode = resp.ExitCode,
                Duration = stopwatch.Elapsed,
                CmdId = request.CmdId,
                MachineCode = request.MachineCode,
                Reason = resp.Reason,
                Signal = resp.Signal,
                TimedOut = resp.TimedOut,
                CleanupConfirmed = resp.CleanupConfirmed,
                Truncated = resp.Truncated
            };
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return Failed("诊断请求已取消；远端是否仍在执行未知。", stopwatch.Elapsed,
                request.CmdId, request.MachineCode);
        }
        catch (OperationCanceledException)
        {
            return Failed("25s 内未取得诊断结果；远端是否仍在执行未知。", stopwatch.Elapsed,
                request.CmdId, request.MachineCode);
        }
        catch (Exception ex)
        {
            _logger?.LogError(ex, "诊断异常: {Command}", command.Command);
            return Failed(ex.Message, stopwatch.Elapsed, request.CmdId, request.MachineCode);
        }
    }

    private (Legacy.IDeviceTransport? Transport, Legacy.DeviceEndpoint? Endpoint) ResolveLegacy()
    {
        if (_connectionService.GetTransport() is Infrastructure.Transport.Real.RealDeviceTransport real)
        {
            return (real.LegacyTransport, real.Endpoint);
        }
        return (null, null);
    }

    private static CommandResult Failed(string message, TimeSpan duration, string? cmdId = null, string? machineCode = null) => new()
    {
        Success = false,
        Error = message,
        ExitCode = -1,
        Duration = duration,
        CmdId = cmdId,
        MachineCode = machineCode
    };

    /// <summary>旧项目 8 种网关诊断命令（edge 端 diag 协议，非 shell 命令）。</summary>
    private static List<DiagnosticCommand> BuildPredefinedCommands() => new()
    {
        New("uptime", "运行时间与负载", CommandCategory.System),
        New("free_m", "内存使用", CommandCategory.System),
        New("df_root", "磁盘使用", CommandCategory.Disk),
        New("top_once", "进程 CPU 占用快照", CommandCategory.Process),
        New("ps_gateway", "网关进程列表", CommandCategory.Process),
        New("journal_tail", "系统日志尾部（需行数）", CommandCategory.Log),
        New("systemctl_status", "服务状态（需服务名）", CommandCategory.Log),
        New("cellular_status", "蜂窝网络状态", CommandCategory.Network),
    };

    private static DiagnosticCommand New(string command, string description, CommandCategory category) => new()
    {
        Name = command,
        Command = command,
        Description = description,
        Category = category
    };
}
