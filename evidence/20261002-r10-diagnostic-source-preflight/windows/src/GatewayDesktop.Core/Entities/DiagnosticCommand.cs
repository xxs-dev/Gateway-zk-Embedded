namespace GatewayDesktop.Core.Entities;

/// <summary>
/// 诊断命令
/// </summary>
public class DiagnosticCommand
{
    public string Id { get; set; } = Guid.NewGuid().ToString();
    public string Name { get; set; } = string.Empty;
    public string Command { get; set; } = string.Empty;
    public string Description { get; set; } = string.Empty;
    public CommandCategory Category { get; set; }
    public bool RequiresConfirmation { get; set; }
    public List<CommandParameter> Parameters { get; set; } = new();
}

/// <summary>
/// 命令分类
/// </summary>
public enum CommandCategory
{
    System,
    Network,
    Process,
    Disk,
    Log,
    Custom
}

/// <summary>
/// 命令参数
/// </summary>
public class CommandParameter
{
    public string Name { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;
    public string DefaultValue { get; set; } = string.Empty;
    public bool Required { get; set; }
}

/// <summary>
/// 命令执行结果
/// </summary>
public class CommandResult
{
    public bool Success { get; set; }
    public string Output { get; set; } = string.Empty;
    public string Error { get; set; } = string.Empty;
    public int ExitCode { get; set; }
    public DateTime ExecutedAt { get; set; } = DateTime.Now;
    public TimeSpan Duration { get; set; }
    public string? CmdId { get; set; }
    public string? MachineCode { get; set; }
    public string? Reason { get; set; }
    public int? Signal { get; set; }
    public bool? TimedOut { get; set; }
    public bool? CleanupConfirmed { get; set; }
    public bool? Truncated { get; set; }
}
