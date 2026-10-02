using System.Text.Json.Serialization;
using GatewayDesktop.Core.Entities;

namespace GatewayDesktop.Core.Devices;

public sealed record DeviceHealth(
    bool Success,
    string Status,
    long Ts,
    string Message
);

public sealed record DeviceIdentity(
    string MachineCode,
    string Imei,
    string SerialNumber,
    string Model,
    string HardwareVersion,
    string FirmwareVersion
);

public sealed record OnlineDevice(
    string MachineCode,
    string SourceTopic,
    long LastSeenTs
);

public sealed record DirectAuthState(
    bool Paired,
    bool UsingDefaultPassword,
    bool BootstrapMode,
    bool SensitiveApiEnabled
);

public sealed record RealtimePoint(
    uint Index,
    string MachineCode,
    string MeterCode,
    string PointCode,
    [property: JsonPropertyName("name")]
    string? Name,
    [property: JsonPropertyName("pointName")]
    string? PointName,
    double? Value,
    int Quality,
    long Ts,
    bool Stale
)
{
    [JsonIgnore]
    public string DisplayName =>
        !string.IsNullOrWhiteSpace(Name)
            ? Name
            : PointName ?? "";
}

public sealed record RealtimePointSnapshot(
    long Ts,
    int Count,
    IReadOnlyList<RealtimePoint> Points
);

public sealed record FullTelemetrySnapshot(
    long Ts,
    int Count,
    string Topic,
    string RawPayload,
    IReadOnlyList<RealtimePoint> Points
);

public sealed record OtaCapabilities(
    IReadOnlyList<string> SupportedPackageTypes,
    bool DirectUpload,
    string Message
);

public sealed record OtaJobRequest(
    [property: JsonPropertyName("jobId")]
    string JobId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("packageType")]
    string PackageType,
    [property: JsonPropertyName("artifactUrl")]
    string ArtifactUrl,
    [property: JsonPropertyName("version")]
    string Version,
    [property: JsonPropertyName("sha256")]
    string Sha256,
    [property: JsonPropertyName("size")]
    ulong Size,
    [property: JsonPropertyName("upgradeMode")]
    string UpgradeMode,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256,
    [property: JsonPropertyName("signature")]
    string Signature
)
{
    public string? ValidateForEndpoint(DeviceEndpoint endpoint) =>
        new SignedOtaJob(JobId, MachineCode, PackageType, Version, Sha256, Size, ArtifactUrl, Signature)
            .Validate(endpoint.MachineCode);
}

public sealed record OtaJobResponse(
    bool Success,
    string JobId,
    string PackageType,
    string Message
);

public sealed record OtaStatusItem(
    string JobId,
    string MachineCode,
    string Stage,
    int Progress,
    ulong DownloadedBytes,
    ulong TotalBytes,
    string Message,
    long Ts
);

public sealed record OtaStatusSnapshot(
    long Ts,
    int Count,
    IReadOnlyList<OtaStatusItem> Items
);

public sealed record DeviceServiceStatus(
    string Name,
    string State,
    string Message
);

public sealed record CellularStatus(
    bool Enabled,
    bool Present,
    bool Registered,
    bool Connected,
    bool SimReady,
    bool ToolsAvailable,
    string Operator,
    string AccessTech,
    string InterfaceName,
    string IpAddress,
    string Gateway,
    string Dns,
    string SimStatus,
    string Imei,
    string Imsi,
    string Iccid,
    string ModemDevice,
    string LastError,
    double SignalPercent,
    double RssiDbm,
    double RsrpDbm,
    double RsrqDb,
    double SinrDb,
    ulong RxBytes,
    ulong TxBytes,
    double RxRateBps,
    double TxRateBps,
    long Ts
);

public sealed record SystemStatusSnapshot(
    string MachineCode,
    double CpuUsage,
    double MemUsage,
    double DiskUsage,
    double Load1,
    int ProcessCount,
    CellularStatus Cellular,
    IReadOnlyList<DeviceServiceStatus> Services,
    long Ts
);

public sealed record ConfigPullRequest(
    [property: JsonPropertyName("requestId")]
    string RequestId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256,
    [property: JsonPropertyName("scope")]
    string Scope = "config"
);

public sealed record ConfigSnapshotFile(
    [property: JsonPropertyName("path")]
    string Path,
    [property: JsonPropertyName("sizeBytes")]
    ulong SizeBytes,
    [property: JsonPropertyName("modifiedAtMs")]
    long ModifiedAtMs,
    [property: JsonPropertyName("content")]
    string Content,
    [property: JsonPropertyName("encoding")]
    string Encoding = "utf8"
);

public sealed record ConfigSnapshot(
    [property: JsonPropertyName("requestId")]
    string RequestId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("success")]
    bool Success,
    [property: JsonPropertyName("message")]
    string Message,
    [property: JsonPropertyName("fileCount")]
    int FileCount,
    [property: JsonPropertyName("skippedFiles")]
    int SkippedFiles,
    [property: JsonPropertyName("totalBytes")]
    ulong TotalBytes,
    [property: JsonPropertyName("ts")]
    long Ts,
    [property: JsonPropertyName("files")]
    IReadOnlyList<ConfigSnapshotFile> Files,
    [property: JsonPropertyName("scope")]
    string Scope = "config"
);

public sealed record ConfigPullProgress(
    string Stage,
    string Message,
    int? Percent,
    int ReceivedChunks,
    int TotalChunks
);

public sealed record DiagRequest(
    [property: JsonPropertyName("cmdId")]
    string CmdId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("operator")]
    string Operator,
    [property: JsonPropertyName("command")]
    string Command,
    [property: JsonPropertyName("service")]
    string Service,
    [property: JsonPropertyName("tailLines")]
    int TailLines,
    [property: JsonPropertyName("requestTs")]
    long RequestTs,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256
);

public sealed record DiagResponse(
    string CmdId,
    string MachineCode,
    string Operator,
    string Command,
    bool Success,
    int ExitCode,
    string Stdout,
    string Stderr,
    long RequestTs,
    long Ts,
    string? Arg = null,
    string? Reason = null,
    int? Signal = null,
    bool? TimedOut = null,
    bool? CleanupConfirmed = null,
    bool? Truncated = null,
    string? ReplyKind = null
);

public sealed record ConfigApplyFile(
    [property: JsonPropertyName("path")]
    string Path,
    [property: JsonPropertyName("content")]
    string Content,
    [property: JsonPropertyName("encoding")]
    string Encoding = "utf8"
);

public sealed record ConfigApplyRequest(
    [property: JsonPropertyName("requestId")]
    string RequestId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("dryRun")]
    bool DryRun,
    [property: JsonPropertyName("restartServices")]
    bool RestartServices,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256,
    [property: JsonPropertyName("files")]
    IReadOnlyList<ConfigApplyFile> Files
);

public sealed record ConfigApplyFileResult(
    string Path,
    bool Success,
    bool Changed,
    string Message
);

public sealed record ConfigApplyResponse(
    string RequestId,
    string MachineCode,
    bool Success,
    bool DryRun,
    int AppliedFiles,
    int ChangedFiles,
    string Message,
    IReadOnlyList<ConfigApplyFileResult> Files,
    long Ts
);

public sealed record ConfigFileOperationRequest(
    [property: JsonPropertyName("requestId")]
    string RequestId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("path")]
    string Path,
    [property: JsonPropertyName("dryRun")]
    bool DryRun,
    [property: JsonPropertyName("restartServices")]
    bool RestartServices,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256
);

public sealed record ConfigFileOperationResponse(
    string RequestId,
    string MachineCode,
    bool Success,
    bool DryRun,
    string Operation,
    string Path,
    bool Changed,
    string Message,
    string BackupPath,
    long Ts
);

public sealed record ControlCommandItem(
    [property: JsonPropertyName("cmdId")]
    string CmdId,
    [property: JsonPropertyName("index")]
    uint Index,
    [property: JsonPropertyName("meterCode")]
    string MeterCode,
    [property: JsonPropertyName("pointCode")]
    string PointCode,
    [property: JsonPropertyName("value")]
    double Value,
    [property: JsonPropertyName("source")]
    string Source,
    [property: JsonPropertyName("ts")]
    long Ts,
    [property: JsonPropertyName("highPriority")]
    bool HighPriority = false
);

public sealed record BatchControlRequest(
    [property: JsonPropertyName("requestId")]
    string RequestId,
    [property: JsonPropertyName("machineCode")]
    string MachineCode,
    [property: JsonPropertyName("operator")]
    string Operator,
    [property: JsonPropertyName("passwordSha256")]
    string PasswordSha256,
    [property: JsonPropertyName("commands")]
    IReadOnlyList<ControlCommandItem> Commands
);

public sealed record ControlCommandResult(
    string CmdId,
    string MachineCode,
    string MeterCode,
    string PointCode,
    uint Index,
    double Value,
    bool Success,
    string Message,
    long Ts,
    string Stage = "",
    long RequestedAt = 0,
    long AcceptedAt = 0,
    long WriteStartedAt = 0,
    long WriteCompletedAt = 0,
    long QueueDelayMs = 0,
    long DeviceWriteMs = 0,
    long EdgeElapsedMs = 0,
    long TotalElapsedMs = 0,
    bool VerifyAttempted = false,
    bool VerifyPassed = false,
    bool HighPriority = false
);

public sealed record BatchControlResponse(
    string RequestId,
    string MachineCode,
    bool Success,
    int Total,
    int Accepted,
    int Failed,
    string Message,
    IReadOnlyList<ControlCommandResult> Results,
    long Ts
);
