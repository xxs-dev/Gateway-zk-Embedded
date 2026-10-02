using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using GatewayDesktop.Core.Entities;
using GatewayDesktop.Core.Interfaces;
using GatewayDesktop.UI.Infrastructure;
using Microsoft.Win32;
using Microsoft.Extensions.Logging;
using UiAppContext = GatewayDesktop.UI.Infrastructure.AppContext;

namespace GatewayDesktop.UI.ViewModels;

/// <summary>
/// 诊断命令 ViewModel。选命令 + 服务名 + 日志行数 → 执行 → 输出。
/// Phase 1 已接 Legacy ExecuteDiagAsync（真实边端诊断）。
/// </summary>
public partial class DiagnosticViewModel : ViewModelBase
{
    private readonly IDiagnosticService _diagnosticService;
    private readonly UiAppContext _appContext;
    private readonly ILogger<DiagnosticViewModel>? _logger;
    private ExecutedDiagnostic? _lastRequest;
    private CommandResult? _lastResult;

    private sealed record ExecutedDiagnostic(string Name, string Command, string Service, int TailLines,
        string MachineCode, string ConnectionMode, string? CmdId = null);

    public ObservableCollection<DiagnosticCommand> Commands { get; } = new();

    public ObservableCollection<DiagnosticInsightRow> InsightRows { get; } = new();

    [ObservableProperty]
    private DiagnosticCommand? _selectedCommand;

    /// <summary>服务名（journal_tail / systemctl_status 需要，如 gateway-services）。</summary>
    [ObservableProperty]
    private string _serviceName = "gateway-services";

    /// <summary>日志尾部行数（journal_tail 用）。</summary>
    [ObservableProperty]
    private int _tailLines = 100;

    /// <summary>诊断输出（stdout / stderr）。</summary>
    [ObservableProperty]
    private string _output = string.Empty;

    [ObservableProperty]
    private string _resultSummary = "尚未执行诊断。";

    [ObservableProperty]
    private DateTime? _lastExecutedAt;

    [ObservableProperty]
    private int _lastExitCode;

    [ObservableProperty]
    private double _lastDurationMs;

    [ObservableProperty]
    private string _prefillSummary = "可从系统状态、服务配置或持续监控带入服务名；也可以手动选择命令执行。";

    public DiagnosticViewModel(
        IDiagnosticService diagnosticService,
        UiAppContext appContext,
        ILogger<DiagnosticViewModel>? logger = null)
    {
        _diagnosticService = diagnosticService;
        _appContext = appContext;
        _logger = logger;

        _ = LoadCommandsAsync();
    }

    private async Task LoadCommandsAsync()
    {
        var commands = await _diagnosticService.GetPredefinedCommandsAsync();
        Commands.Clear();
        foreach (var cmd in commands)
            Commands.Add(cmd);
        if (Commands.Count > 0)
            SelectedCommand = Commands[0];
        ApplyPendingDiagnosticRequest();
    }

    /// <summary>是否需要服务名输入（仅 journal_tail / systemctl_status）。</summary>
    public bool NeedsService => SelectedCommand?.Command is "systemctl_status";

    public bool NeedsTailLines => SelectedCommand?.Command is "journal_tail";

    partial void OnSelectedCommandChanged(DiagnosticCommand? value)
    {
        OnPropertyChanged(nameof(NeedsService));
        OnPropertyChanged(nameof(NeedsTailLines));
    }

    [RelayCommand]
    private async Task ExecuteAsync()
    {
        var selected = SelectedCommand;
        if (selected is null)
        {
            StatusMessage = "请先选择诊断命令";
            return;
        }

        var request = new ExecutedDiagnostic(selected.Name, selected.Command,
            selected.Command == "systemctl_status" ? ServiceName.Trim() : string.Empty,
            selected.Command == "journal_tail" ? Math.Clamp(TailLines, 1, 500) : 100,
            _appContext.MachineCode, _appContext.ConnectionMode.ToString());
        var executedCommand = new DiagnosticCommand { Name = request.Name, Command = request.Command };
        IsBusy = true;
        BusyMessage = $"执行诊断 {request.Command}…";
        Output = string.Empty;

        try
        {
            var result = await _diagnosticService.ExecuteAsync(executedCommand, request.Service, request.TailLines);

            request = request with { MachineCode = result.MachineCode ?? request.MachineCode, CmdId = result.CmdId };
            _lastRequest = request;
            _lastResult = result;
            Output = result.Success
                ? result.Output
                : $"[失败 exit={result.ExitCode}]{(string.IsNullOrEmpty(result.Error) ? "" : " " + result.Error)}\n\n{result.Output}";
            LastExecutedAt = result.ExecutedAt;
            LastExitCode = result.ExitCode;
            LastDurationMs = result.Duration.TotalMilliseconds;
            ResultSummary = $"{request.Name} · {(result.Success ? "成功" : "失败")} · exit={result.ExitCode} · {result.Duration.TotalMilliseconds:F0}ms · {result.ExecutedAt:yyyy-MM-dd HH:mm:ss}";
            if (!string.IsNullOrWhiteSpace(result.Reason)) ResultSummary += $" · {result.Reason}";
            RebuildInsights(result, executedCommand);

            StatusMessage = result.Success
                ? $"诊断完成（{result.Duration.TotalMilliseconds:F0}ms）"
                : "诊断失败";
            _appContext.RecordAction("诊断命令", $"{request.MachineCode}/{request.Command}/{request.CmdId}", result.Success, ResultSummary);
        }
        catch (Exception ex)
        {
            _logger?.LogError(ex, "诊断执行异常");
            _lastRequest = request;
            _lastResult = null;
            Output = "[异常] " + ex.Message;
            ResultSummary = $"诊断异常 · {DateTime.Now:yyyy-MM-dd HH:mm:ss}";
            RebuildExceptionInsight(ex);
            StatusMessage = "诊断异常";
            _appContext.RecordAction("诊断命令", $"{request.MachineCode}/{request.Command}/{request.CmdId}", false, ResultSummary + "：" + ex.Message);
        }
        finally
        {
            IsBusy = false;
            BusyMessage = string.Empty;
        }
    }

    public void ApplyPendingDiagnosticRequest()
    {
        var request = _appContext.ConsumeDiagnosticRequest();
        if (request is null)
        {
            return;
        }

        ServiceName = request.ServiceName;
        var command = Commands.FirstOrDefault(item => string.Equals(item.Command, request.Command, StringComparison.OrdinalIgnoreCase));
        if (command is not null)
        {
            SelectedCommand = command;
        }

        PrefillSummary = $"已从「{request.Source}」带入服务 {request.ServiceName}，建议执行 {request.Command}。";
        StatusMessage = PrefillSummary;
    }

    [RelayCommand]
    private void CopyOutput()
    {
        if (string.IsNullOrWhiteSpace(Output))
        {
            StatusMessage = "暂无诊断输出可复制。";
            return;
        }

        Clipboard.SetText(BuildExportText());
        StatusMessage = "诊断输出已复制到剪贴板。";
    }

    [RelayCommand]
    private void ExportOutput()
    {
        if (string.IsNullOrWhiteSpace(Output))
        {
            StatusMessage = "暂无诊断输出可导出。";
            return;
        }

        var dialog = new SaveFileDialog
        {
            Title = "导出诊断输出",
            Filter = "文本文件 (*.txt)|*.txt|日志文件 (*.log)|*.log|所有文件|*.*",
            FileName = $"diagnostic-{_lastRequest?.Command ?? "output"}-{DateTime.Now:yyyyMMddHHmmss}.txt"
        };
        if (dialog.ShowDialog() != true)
        {
            return;
        }

        try
        {
            File.WriteAllText(dialog.FileName, BuildExportText());
            StatusMessage = $"诊断输出已导出：{Path.GetFileName(dialog.FileName)}";
        }
        catch (Exception ex)
        {
            _logger?.LogError(ex, "导出诊断输出失败");
            StatusMessage = "导出失败：" + ex.Message;
        }
    }

    private string BuildExportText() =>
        $"""
        GatewayDesktop Diagnostic
        Command: {_lastRequest?.Name} ({_lastRequest?.Command})
        Service: {_lastRequest?.Service}
        TailLines: {_lastRequest?.TailLines}
        MachineCode: {_lastRequest?.MachineCode}
        ConnectionMode: {_lastRequest?.ConnectionMode}
        CmdId: {_lastResult?.CmdId ?? "unknown"}
        Reason: {_lastResult?.Reason ?? "unknown"}
        Signal: {_lastResult?.Signal?.ToString() ?? "unknown"}
        TimedOut: {_lastResult?.TimedOut?.ToString() ?? "unknown"}
        CleanupConfirmed: {_lastResult?.CleanupConfirmed?.ToString() ?? "unknown"}
        Truncated: {_lastResult?.Truncated?.ToString() ?? "unknown"}
        Summary: {ResultSummary}

        {Output}
        """;

    private void RebuildInsights(CommandResult result, DiagnosticCommand command)
    {
        InsightRows.Clear();
        InsightRows.Add(new DiagnosticInsightRow("执行结果", result.Success ? "通过" : "阻断", $"exit={result.ExitCode}，耗时 {result.Duration.TotalMilliseconds:F0}ms"));
        if (result.Reason is not null || result.CleanupConfirmed is not null || result.Truncated is not null)
            InsightRows.Add(new DiagnosticInsightRow("远端分类", result.CleanupConfirmed == false ? "阻断" : "建议",
                $"reason={result.Reason ?? "unknown"}, signal={result.Signal?.ToString() ?? "unknown"}, timedOut={result.TimedOut?.ToString() ?? "unknown"}, cleanup={result.CleanupConfirmed?.ToString() ?? "unknown"}, truncated={result.Truncated?.ToString() ?? "unknown"}"));

        var text = (result.Output + Environment.NewLine + result.Error).Trim();
        if (string.IsNullOrWhiteSpace(text))
        {
            InsightRows.Add(new DiagnosticInsightRow("输出", "建议", "诊断没有返回输出，可能是命令无结果或设备未采集到数据。"));
            return;
        }

        var lines = text.Split('\n').Select(line => line.Trim()).Where(line => !string.IsNullOrWhiteSpace(line)).ToList();
        var errorLines = lines.Where(line => ContainsAny(line, "error", "failed", "failure", "exception", "denied", "失败", "错误")).Take(5).ToList();
        foreach (var line in errorLines)
        {
            InsightRows.Add(new DiagnosticInsightRow("错误线索", "阻断", line));
        }

        var warningLines = lines.Where(line => ContainsAny(line, "warn", "inactive", "degraded", "timeout", "超时", "警告")).Take(5).ToList();
        foreach (var line in warningLines)
        {
            InsightRows.Add(new DiagnosticInsightRow("风险线索", "建议", line));
        }

        if (command.Command == "df_root")
        {
            AddDiskInsight(lines);
        }
        if (command.Command == "free_m")
        {
            AddMemoryInsight(lines);
        }
        if (command.Command == "systemctl_status")
        {
            AddServiceInsight(lines);
        }

        if (InsightRows.Count == 1)
        {
            InsightRows.Add(new DiagnosticInsightRow("风险扫描", "通过", "未发现明显 error/failed/warn/inactive 关键词。"));
        }
    }

    private void RebuildExceptionInsight(Exception ex)
    {
        InsightRows.Clear();
        InsightRows.Add(new DiagnosticInsightRow("异常", "阻断", ex.Message));
    }

    private void AddDiskInsight(IEnumerable<string> lines)
    {
        foreach (var line in lines)
        {
            var percent = ExtractPercent(line);
            if (percent is null) continue;
            var status = percent >= 90 ? "阻断" : percent >= 80 ? "建议" : "通过";
            InsightRows.Add(new DiagnosticInsightRow("磁盘使用率", status, $"{percent}% · {line}"));
            return;
        }
    }

    private void AddMemoryInsight(IEnumerable<string> lines)
    {
        var line = lines.FirstOrDefault(item => item.StartsWith("Mem:", StringComparison.OrdinalIgnoreCase));
        if (line is not null)
        {
            InsightRows.Add(new DiagnosticInsightRow("内存摘要", "通过", line));
        }
    }

    private void AddServiceInsight(IEnumerable<string> lines)
    {
        var active = lines.FirstOrDefault(line => line.Contains("Active:", StringComparison.OrdinalIgnoreCase));
        if (active is null) return;
        var status = ContainsAny(active, "active (running)") ? "通过" : "建议";
        InsightRows.Add(new DiagnosticInsightRow("服务状态", status, active));
    }

    private static int? ExtractPercent(string line)
    {
        foreach (var token in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            if (token.EndsWith('%') && int.TryParse(token.TrimEnd('%'), out var value))
            {
                return value;
            }
        }
        return null;
    }

    private static bool ContainsAny(string text, params string[] needles) =>
        needles.Any(needle => text.Contains(needle, StringComparison.OrdinalIgnoreCase));
}

public sealed record DiagnosticInsightRow(string Item, string Status, string Detail);
