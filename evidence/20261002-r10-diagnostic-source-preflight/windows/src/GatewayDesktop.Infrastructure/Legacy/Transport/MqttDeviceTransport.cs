using System.Buffers.Binary;
using System.Globalization;
using System.Security.Authentication;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using GatewayDesktop.Core.Devices;
using GatewayDesktop.Transport;

namespace GatewayDesktop.Transport.Mqtt;

public sealed class MqttDeviceTransport : IDeviceTransport
{
    private static readonly TimeSpan ConfigPullTotalTimeout = TimeSpan.FromMinutes(10);
    private static readonly TimeSpan ConfigPullIdleTimeout = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan ConfigPullChunkIdleTimeout = TimeSpan.FromSeconds(8);
    private const int ConfigPullMaxResendAttempts = 4;
    private const int ConfigApplyChunkBytes = 128 * 1024;
    private const int ConfigApplySinglePayloadLimitBytes = 128 * 1024;
    private const int NormalQos = 1;
    private const int ControlQos = 2;

    private static readonly string[] OnlineDeviceDiscoveryTopics =
    [
        "edge/status",
        "edge/status/+",
        "edge/telemetry",
        "edge/telemetry/+",
        "edge/telemetry/full",
        "edge/telemetry/full/+",
        "edge/telemetry/realtime",
        "edge/telemetry/realtime/+",
        "edge/system/monitor/telemetry",
        "edge/system/monitor/telemetry/+",
        "edge/system/monitor/points",
        "edge/system/monitor/points/+",
        "edge/system/monitor/alert",
        "edge/system/monitor/alert/+",
        "edge/ota/status",
        "edge/ota/status/+",
        "edge/config/pull/reply",
        "edge/config/pull/reply/+",
        "edge/config/apply/reply",
        "edge/config/apply/reply/+",
        "edge/config/delete/reply",
        "edge/config/delete/reply/+",
        "edge/config/restore/reply",
        "edge/config/restore/reply/+"
    ];

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private readonly Func<DeviceEndpoint, IReadOnlyList<MqttPublication>, CancellationToken, Task> _publishPublicationsAsync;
    private readonly string _realtimeSessionOwner = Guid.NewGuid().ToString("N");

    public MqttDeviceTransport()
        : this(PublishPublicationsAsync)
    {
    }

    internal MqttDeviceTransport(
        Func<DeviceEndpoint, IReadOnlyList<MqttPublication>, CancellationToken, Task> publishPublicationsAsync)
    {
        _publishPublicationsAsync = publishPublicationsAsync ?? throw new ArgumentNullException(nameof(publishPublicationsAsync));
    }

    public DeviceAccessMode Mode => DeviceAccessMode.RemoteMqtt;

    public async Task<TransportResult<DeviceHealth>> GetHealthAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<DeviceHealth>.Fail(error);
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            return TransportResult<DeviceHealth>.Ok(
                new DeviceHealth(true, "mqtt-connected", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), broker.Host),
                "mqtt broker accepted the connection"
            );
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or TaskCanceledException)
        {
            return TransportResult<DeviceHealth>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<DeviceIdentity>> GetIdentityAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var identity = new DeviceIdentity(
            endpoint.MachineCode,
            "",
            "",
            "remote-mqtt-device",
            "",
            ""
        );
        return Task.FromResult(TransportResult<DeviceIdentity>.Ok(identity, "identity comes from selected MQTT device profile"));
    }

    public async Task<TransportResult<IReadOnlyList<OnlineDevice>>> DiscoverOnlineDevicesAsync(
        DeviceEndpoint endpoint,
        TimeSpan scanDuration,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<IReadOnlyList<OnlineDevice>>.Fail(error);
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            foreach (var topic in OnlineDeviceDiscoveryTopics)
            {
                await client.SubscribeAsync(topic, NormalQos, cancellationToken);
            }

            // 收集所有收到消息的 topic（含未识别 mc 的），用于诊断"扫不全"
            var rawTopics = new List<string>();
            var messages = await client.ReadMatchingAsync(
                OnlineDeviceDiscoveryTopics,
                scanDuration,
                cancellationToken,
                onMessage: msg =>
                {
                    var preview = msg.Payload.Length > 80 ? msg.Payload[..80] + "…" : msg.Payload;
                    rawTopics.Add($"{msg.Topic}  |  {preview}");
                    return false; // 不提前停止
                }
            );
            var devicesByMachine = new Dictionary<string, OnlineDevice>(StringComparer.OrdinalIgnoreCase);
            foreach (var message in messages)
            {
                if (!TryBuildOnlineDevice(message, out var device))
                {
                    continue;
                }

                if (!devicesByMachine.TryGetValue(device.MachineCode, out var existing) ||
                    existing.LastSeenTs <= device.LastSeenTs)
                {
                    devicesByMachine[device.MachineCode] = device;
                }
            }

            var devices = devicesByMachine.Values
                .OrderBy(item => item.MachineCode, StringComparer.OrdinalIgnoreCase)
                .ToArray();

            var diag = rawTopics.Count > 0
                ? $"收到 {rawTopics.Count} 条消息：\n  - " + string.Join("\n  - ", rawTopics)
                : "扫描期间未收到任何匹配主题的消息";
            return TransportResult<IReadOnlyList<OnlineDevice>>.Ok(
                devices,
                devices.Length == 0
                    ? $"no online machine telemetry received during scan\n{diag}"
                    : $"online machines={devices.Length}\n{diag}"
            );
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<IReadOnlyList<OnlineDevice>>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<DirectAuthState>> GetAuthStateAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var state = new DirectAuthState(false, false, false, false);
        return Task.FromResult(TransportResult<DirectAuthState>.Ok(state, "remote MQTT mode does not use direct maintenance auth"));
    }

    public async Task<TransportResult<RealtimePointSnapshot>> GetRealtimePointsAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<RealtimePointSnapshot>.Fail(error);
        }

        if (string.IsNullOrWhiteSpace(endpoint.MachineCode))
        {
            return TransportResult<RealtimePointSnapshot>.Fail("machineCode is required for MQTT realtime");
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var topic = ScopedTopic("edge/telemetry/realtime", endpoint.MachineCode);
            var requestTopic = ScopedTopic("edge/telemetry/realtime/request", endpoint.MachineCode);
            var monitorRequestTopic = ScopedTopic("edge/system/monitor/request", endpoint.MachineCode);
            await client.SubscribeAsync(topic, NormalQos, cancellationToken);
            var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var sessionMeter = string.IsNullOrWhiteSpace(endpoint.RealtimeMeterCode)
                ? "*"
                : endpoint.RealtimeMeterCode.Trim();
            var sessionId = BuildRealtimeSessionId(endpoint.MachineCode, sessionMeter);
            var monitorRequest = new Dictionary<string, object>
            {
                ["action"] = "subscribe",
                ["sessionId"] = sessionId,
                ["machineCode"] = endpoint.MachineCode,
                ["intervalMs"] = 1000,
                ["ttlSec"] = 20,
                ["ts"] = ts
            };
            if (!string.IsNullOrWhiteSpace(endpoint.RealtimeMeterCode))
            {
                monitorRequest["meterCode"] = endpoint.RealtimeMeterCode.Trim();
            }

            await client.PublishAsync(
                monitorRequestTopic,
                JsonSerializer.Serialize(monitorRequest),
                NormalQos,
                cancellationToken
            );

            var request = new Dictionary<string, object>
            {
                ["requestId"] = "DESKTOP_REALTIME_" + ts,
                ["action"] = "subscribe",
                ["sessionId"] = sessionId,
                ["machineCode"] = endpoint.MachineCode,
                ["intervalMs"] = 1000,
                ["ttlSec"] = 20,
                ["ts"] = ts
            };
            if (!string.IsNullOrWhiteSpace(endpoint.RealtimeMeterCode))
            {
                request["meterCode"] = endpoint.RealtimeMeterCode.Trim();
            }

            var payload = JsonSerializer.Serialize(request);
            await client.PublishAsync(requestTopic, payload, NormalQos, cancellationToken);
            var messages = await client.ReadMatchingAsync(
                new[] { topic },
                TimeSpan.FromSeconds(8),
                cancellationToken
            );

            var latest = AssembleRealtimeSnapshots(
                messages.Select(message => message.Payload), endpoint.MachineCode, sessionId);
            if (latest is null)
            {
                return TransportResult<RealtimePointSnapshot>.Fail("no MQTT realtime payload received");
            }

            latest = FilterSnapshotByMeter(latest, endpoint.RealtimeMeterCode);
            return TransportResult<RealtimePointSnapshot>.Ok(latest);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<RealtimePointSnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<bool>> StopRealtimeAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out _, out var error))
        {
            return TransportResult<bool>.Fail(error);
        }

        if (string.IsNullOrWhiteSpace(endpoint.MachineCode))
        {
            return TransportResult<bool>.Fail("machineCode is required for MQTT realtime stop");
        }

        try
        {
            var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var sessionMeter = string.IsNullOrWhiteSpace(endpoint.RealtimeMeterCode)
                ? "*"
                : endpoint.RealtimeMeterCode.Trim();
            var sessionId = BuildRealtimeSessionId(endpoint.MachineCode, sessionMeter);
            var commonRequest = new Dictionary<string, object>
            {
                ["action"] = "unsubscribe",
                ["sessionId"] = sessionId,
                ["machineCode"] = endpoint.MachineCode,
                ["ts"] = ts
            };
            var realtimeRequest = new Dictionary<string, object>(commonRequest)
            {
                ["requestId"] = "DESKTOP_REALTIME_STOP_" + ts
            };
            IReadOnlyList<MqttPublication> publications =
            [
                new(
                    ScopedTopic("edge/system/monitor/request", endpoint.MachineCode),
                    JsonSerializer.Serialize(commonRequest),
                    NormalQos),
                new(
                    ScopedTopic("edge/telemetry/realtime/request", endpoint.MachineCode),
                    JsonSerializer.Serialize(realtimeRequest),
                    NormalQos)
            ];

            await _publishPublicationsAsync(endpoint, publications, cancellationToken);
            return TransportResult<bool>.Ok(true, "MQTT realtime unsubscribe published");
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<bool>.Fail(ex.Message);
        }
    }

    private static async Task PublishPublicationsAsync(
        DeviceEndpoint endpoint,
        IReadOnlyList<MqttPublication> publications,
        CancellationToken cancellationToken)
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            throw new InvalidOperationException(error);
        }

        using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
        foreach (var publication in publications)
        {
            await client.PublishAsync(
                publication.Topic,
                publication.Payload,
                publication.Qos,
                cancellationToken);
        }
    }

    public async Task<TransportResult<FullTelemetrySnapshot>> GetFullTelemetryAsync(
        DeviceEndpoint endpoint,
        TimeSpan listenDuration,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<FullTelemetrySnapshot>.Fail(error);
        }

        if (string.IsNullOrWhiteSpace(endpoint.MachineCode))
        {
            return TransportResult<FullTelemetrySnapshot>.Fail("machineCode is required for MQTT full telemetry");
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var topic = ScopedTopic("edge/telemetry/full", endpoint.MachineCode);
            await client.SubscribeAsync(topic, NormalQos, cancellationToken);
            var messages = await client.ReadMatchingAsync(
                new[] { topic },
                listenDuration,
                cancellationToken
            );

            var payloads = messages.Select(message => message.Payload).ToArray();
            var assembled = AssembleRealtimeSnapshots(payloads);
            var latest = assembled is null
                ? null
                : new FullTelemetrySnapshot(
                    assembled.Ts,
                    assembled.Count,
                    topic,
                    string.Join(Environment.NewLine, payloads),
                    assembled.Points);
            return latest is null
                ? TransportResult<FullTelemetrySnapshot>.Fail($"no MQTT full telemetry payload received on {topic}")
                : TransportResult<FullTelemetrySnapshot>.Ok(latest, $"full telemetry received on {latest.Topic}");
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<FullTelemetrySnapshot>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<OtaCapabilities>> GetOtaCapabilitiesAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var capabilities = new OtaCapabilities(
            new[] { "config", "full", "scada" },
            false,
            "client-supported OTA schema types; edge capability is not authenticated here"
        );
        return Task.FromResult(TransportResult<OtaCapabilities>.Ok(capabilities));
    }

    public async Task<TransportResult<OtaJobResponse>> StartOtaJobAsync(
        DeviceEndpoint endpoint,
        OtaJobRequest request,
        CancellationToken cancellationToken
    )
    {
        var validationError = request.ValidateForEndpoint(endpoint);
        if (validationError is not null)
            return TransportResult<OtaJobResponse>.Fail(validationError);
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<OtaJobResponse>.Fail(error);
        }

        var machineCode = request.MachineCode;

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic("edge/ota/request", machineCode);
            var replyTopic = ScopedTopic("edge/ota/reply", machineCode);
            var statusTopic = ScopedTopic("edge/ota/status", machineCode);
            await client.SubscribeAsync(replyTopic, ControlQos, cancellationToken);
            await client.SubscribeAsync(statusTopic, ControlQos, cancellationToken);

            var payload = JsonSerializer.Serialize(
                new
                {
                    jobId = request.JobId,
                    machineCode,
                    packageType = request.PackageType,
                    artifactUrl = request.ArtifactUrl,
                    version = request.Version,
                    sha256 = request.Sha256,
                    size = request.Size,
                    signature = request.Signature,
                    upgradeMode = request.UpgradeMode,
                    ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                }
            );
            await client.PublishAsync(requestTopic, payload, ControlQos, cancellationToken);

            var messages = await client.ReadMatchingAsync(
                new[] { replyTopic, statusTopic },
                TimeSpan.FromSeconds(8),
                cancellationToken,
                message => message.Topic == replyTopic
                    ? string.Equals(TryParseOtaReply(message.Payload, request.PackageType)?.JobId,
                        request.JobId, StringComparison.Ordinal)
                    : message.Topic == statusTopic &&
                      TryParseOtaStatus(message.Payload) is { } statusForStop &&
                      string.Equals(statusForStop.JobId, request.JobId, StringComparison.Ordinal) &&
                      string.Equals(statusForStop.MachineCode, request.MachineCode, StringComparison.Ordinal)
            );
            var reply = messages
                .Where(message => message.Topic == replyTopic)
                .Select(message => TryParseOtaReply(message.Payload, request.PackageType))
                .Where(value => value is not null)
                .LastOrDefault(value => value!.JobId == request.JobId);

            if (reply is not null)
            {
                return TransportResult<OtaJobResponse>.Ok(reply, reply.Message);
            }

            var status = messages
                .Where(message => message.Topic == statusTopic)
                .Select(message => TryParseOtaStatus(message.Payload))
                .Where(value => value is not null)
                .LastOrDefault(value => string.Equals(value!.JobId, request.JobId, StringComparison.Ordinal) &&
                    string.Equals(value.MachineCode, request.MachineCode, StringComparison.Ordinal));
            if (status is not null)
            {
                var accepted = !string.Equals(status.Stage, "failed", StringComparison.OrdinalIgnoreCase);
                var message = $"MQTT OTA status stage={status.Stage} progress={status.Progress}% message={status.Message}";
                return TransportResult<OtaJobResponse>.Ok(
                    new OtaJobResponse(accepted, request.JobId, request.PackageType, message),
                    message
                );
            }

            return TransportResult<OtaJobResponse>.Fail(
                $"no matching MQTT OTA reply/status received on {replyTopic} or {statusTopic} for jobId={request.JobId}"
            );
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<OtaJobResponse>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<OtaStatusSnapshot>> GetOtaStatusAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<OtaStatusSnapshot>.Fail(error);
        }

        if (string.IsNullOrWhiteSpace(endpoint.MachineCode))
        {
            return TransportResult<OtaStatusSnapshot>.Fail("machineCode is required for MQTT OTA status");
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var topic = ScopedTopic("edge/ota/status", endpoint.MachineCode);
            await client.SubscribeAsync(topic, ControlQos, cancellationToken);
            var messages = await client.ReadMatchingAsync(new[] { topic }, TimeSpan.FromSeconds(8), cancellationToken);
            var items = messages
                .Select(message => TryParseOtaStatus(message.Payload))
                .Where(item => item is not null)
                .ToArray();
            var snapshot = new OtaStatusSnapshot(
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                items.Length,
                items!
            );
            return TransportResult<OtaStatusSnapshot>.Ok(snapshot);
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<OtaStatusSnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<ConfigSnapshot>> PullConfigSnapshotAsync(
        DeviceEndpoint endpoint,
        ConfigPullRequest request,
        IProgress<ConfigPullProgress>? progress,
        CancellationToken cancellationToken
    )
    {
        progress?.Report(new ConfigPullProgress("validate", "正在检查 MQTT 配置拉取参数...", 5, 0, 0));
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<ConfigSnapshot>.Fail(error);
        }

        var machineCode = string.IsNullOrWhiteSpace(request.MachineCode)
            ? endpoint.MachineCode
            : request.MachineCode;
        if (string.IsNullOrWhiteSpace(machineCode))
        {
            return TransportResult<ConfigSnapshot>.Fail("machineCode is required for MQTT config pull");
        }

        var requestId = string.IsNullOrWhiteSpace(request.RequestId)
            ? "CFG_PULL_" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
            : request.RequestId;
        var scope = string.IsNullOrWhiteSpace(request.Scope) ? "config" : request.Scope.Trim().ToLowerInvariant();
        if (scope is not ("config" or "scada"))
        {
            return TransportResult<ConfigSnapshot>.Fail($"unsupported config pull scope: {scope}");
        }

        ConfigSnapshotAssemblyState? assembler = null;
        try
        {
            progress?.Report(new ConfigPullProgress("connect", $"正在连接 MQTT Broker {broker.Host}:{broker.Port}...", 10, 0, 0));
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic("edge/config/pull/request", machineCode);
            var replyTopic = ScopedTopic("edge/config/pull/reply", machineCode);
            progress?.Report(new ConfigPullProgress("subscribe", $"正在订阅配置回复 {replyTopic}...", 20, 0, 0));
            await client.SubscribeAsync(replyTopic, NormalQos, cancellationToken);

            var payload = JsonSerializer.Serialize(
                new
                {
                    requestId,
                    machineCode,
                    scope,
                    ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                }
            );
            progress?.Report(new ConfigPullProgress("publish", $"正在发送配置拉取请求 {requestTopic}...", 30, 0, 0));
            await client.PublishAsync(requestTopic, payload, NormalQos, cancellationToken);

            assembler = new ConfigSnapshotAssemblyState(requestId);
            ConfigSnapshot? snapshot = null;
            var waitStopReason = "";
            var waitStartedAt = DateTimeOffset.UtcNow;
            var resendAttempts = 0;
            progress?.Report(new ConfigPullProgress("wait", $"等待设备回复 {replyTopic}，大配置会持续接收分片...", 35, 0, 0));
            while (snapshot is null)
            {
                var remaining = ConfigPullTotalTimeout - (DateTimeOffset.UtcNow - waitStartedAt);
                if (remaining <= TimeSpan.Zero)
                {
                    waitStopReason = $"total timeout {FormatDuration(ConfigPullTotalTimeout)} elapsed";
                    break;
                }

                var idleTimeout = assembler.IsChunkedIncomplete
                    ? ConfigPullChunkIdleTimeout
                    : ConfigPullIdleTimeout;
                await client.ReadMatchingAsync(
                    new[] { replyTopic },
                    remaining,
                    cancellationToken,
                    message =>
                    {
                        snapshot = assembler.Accept(message.Payload);
                        if (snapshot is not null)
                        {
                            progress?.Report(new ConfigPullProgress(
                                "complete",
                                $"配置快照已接收完成，文件 {snapshot.FileCount} 个。",
                                100,
                                assembler.ReceivedChunks,
                                assembler.TotalChunks
                            ));
                            return true;
                        }

                        if (assembler.MatchingPayloads > 0 && assembler.TotalChunks > 0)
                        {
                            var percent = 35 + (int)Math.Round(55.0 * assembler.ReceivedChunks / assembler.TotalChunks);
                            progress?.Report(new ConfigPullProgress(
                                "receive",
                                $"正在接收配置分片 {assembler.ReceivedChunks}/{assembler.TotalChunks}...",
                                Math.Clamp(percent, 35, 95),
                                assembler.ReceivedChunks,
                                assembler.TotalChunks
                            ));
                        }
                        else if (assembler.MatchingPayloads > 0)
                        {
                            progress?.Report(new ConfigPullProgress(
                                "receive",
                                "已收到配置回复，正在解析...",
                                80,
                                0,
                                0
                            ));
                        }

                        return false;
                    },
                    idleTimeout,
                    reason => waitStopReason = reason,
                    () => assembler.IsChunkedIncomplete
                        ? ConfigPullChunkIdleTimeout
                        : ConfigPullIdleTimeout
                );

                if (snapshot is not null)
                {
                    break;
                }

                if (!assembler.IsChunkedIncomplete || resendAttempts >= ConfigPullMaxResendAttempts)
                {
                    break;
                }

                var missingChunks = assembler.MissingChunks.ToArray();
                if (missingChunks.Length == 0)
                {
                    break;
                }

                ++resendAttempts;
                progress?.Report(new ConfigPullProgress(
                    "resend",
                    $"正在请求补发缺失配置分片 {string.Join(",", missingChunks.Take(8))}{(missingChunks.Length > 8 ? "..." : "")}，第 {resendAttempts}/{ConfigPullMaxResendAttempts} 次...",
                    95,
                    assembler.ReceivedChunks,
                    assembler.TotalChunks
                ));
                var resendPayload = JsonSerializer.Serialize(
                    new
                    {
                        requestId,
                        machineCode,
                        scope,
                        resend = true,
                        missingChunks,
                        ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                    }
                );
                await client.PublishAsync(requestTopic, resendPayload, NormalQos, cancellationToken);
                waitStopReason = "";
            }

            return snapshot is null
                ? TransportResult<ConfigSnapshot>.Fail(assembler.BuildFailureMessage(replyTopic, requestId, waitStopReason))
                : TransportResult<ConfigSnapshot>.Ok(snapshot, $"config snapshot files={snapshot.FileCount}");
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or FormatException or TaskCanceledException)
        {
            if (assembler?.MatchingPayloads > 0 && ex is SocketException or IOException or TaskCanceledException)
            {
                return TransportResult<ConfigSnapshot>.Fail(assembler.BuildFailureMessage(
                    ScopedTopic("edge/config/pull/reply", machineCode),
                    requestId,
                    ex.Message
                ));
            }

            return TransportResult<ConfigSnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<SystemStatusSnapshot>> GetSystemStatusAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<SystemStatusSnapshot>.Fail(error);
        }

        if (string.IsNullOrWhiteSpace(endpoint.MachineCode))
        {
            return TransportResult<SystemStatusSnapshot>.Fail("machineCode is required for MQTT system status");
        }

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic("edge/system/monitor/request", endpoint.MachineCode);
            var replyTopic = ScopedTopic("edge/system/monitor/reply", endpoint.MachineCode);
            var telemetryTopic = ScopedTopic("edge/system/monitor/telemetry", endpoint.MachineCode);
            await client.SubscribeAsync(replyTopic, NormalQos, cancellationToken);
            await client.SubscribeAsync(telemetryTopic, NormalQos, cancellationToken);

            var sessionId = "DESKTOP_STATUS_" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var payload = JsonSerializer.Serialize(
                new
                {
                    action = "subscribe",
                    sessionId,
                    machineCode = endpoint.MachineCode,
                    intervalMs = 1000,
                    ttlSec = 15,
                    ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                }
            );
            await client.PublishAsync(requestTopic, payload, NormalQos, cancellationToken);

            var messages = await client.ReadMatchingAsync(
                new[] { telemetryTopic, replyTopic },
                TimeSpan.FromSeconds(10),
                cancellationToken
            );
            var status = messages
                .Where(message => message.Topic == telemetryTopic)
                .Select(message => TryParseSystemStatus(message.Payload))
                .Where(value => value is not null)
                .LastOrDefault();
            return status is null
                ? TransportResult<SystemStatusSnapshot>.Fail("no MQTT system monitor telemetry received")
                : TransportResult<SystemStatusSnapshot>.Ok(status, "system monitor telemetry received");
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<SystemStatusSnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<DiagResponse>> ExecuteDiagAsync(
        DeviceEndpoint endpoint,
        DiagRequest request,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<DiagResponse>.Fail(error);
        }

        var machineCode = endpoint.MachineCode;
        if (string.IsNullOrWhiteSpace(machineCode) || machineCode.Length > 128 ||
            !string.IsNullOrWhiteSpace(request.MachineCode) && request.MachineCode != machineCode)
        {
            return TransportResult<DiagResponse>.Fail("MQTT diag target machineCode mismatch");
        }

        var cmdId = string.IsNullOrWhiteSpace(request.CmdId)
            ? $"DIAG_{Guid.NewGuid():N}"
            : request.CmdId;

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken,
                DiagReplyAdmission.MaxMqttJsonChars + 1024, 8);
            var requestTopic = ScopedTopic("edge/system/diag/request", machineCode);
            var replyTopic = ScopedTopic("edge/system/diag/reply", machineCode);
            await client.SubscribeAsync(replyTopic, ControlQos, cancellationToken);

            var payload = JsonSerializer.Serialize(
                new
                {
                    cmdId,
                    machineCode,
                    @operator = request.Operator,
                    command = request.Command,
                    service = request.Service,
                    tailLines = request.TailLines,
                    requestTs = request.RequestTs == 0
                        ? DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                        : request.RequestTs
                }
            );
            await client.PublishAsync(requestTopic, payload, ControlQos, cancellationToken);

            DiagResponse? terminal = null;
            var rejected = false;
            var observed = 0;
            await client.ReadMatchingAsync(
                new[] { replyTopic },
                TimeSpan.FromSeconds(20),
                cancellationToken,
                onMessage: message =>
                {
                    observed++;
                    var match = DiagReplyAdmission.Match(message.Payload, request with { CmdId = cmdId },
                        machineCode, out var candidate);
                    if (match == DiagReplyKind.Terminal) terminal = candidate;
                    if (match == DiagReplyKind.AdmissionRejected) rejected = true;
                    return terminal is not null || rejected || observed >= 8;
                });
            if (rejected) return TransportResult<DiagResponse>.Fail("MQTT diag cmdId_conflict: request not admitted");
            if (observed >= 8 && terminal is null)
                return TransportResult<DiagResponse>.Fail("too many unmatched MQTT diag replies");
            return terminal is null
                ? TransportResult<DiagResponse>.Fail("no matching MQTT diag reply received")
                : TransportResult<DiagResponse>.Ok(terminal, $"diag exitCode={terminal.ExitCode}");
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidDataException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<DiagResponse>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<ConfigApplyResponse>> ApplyConfigAsync(
        DeviceEndpoint endpoint,
        ConfigApplyRequest request,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<ConfigApplyResponse>.Fail(error);
        }

        var machineCode = string.IsNullOrWhiteSpace(request.MachineCode)
            ? endpoint.MachineCode
            : request.MachineCode;
        if (string.IsNullOrWhiteSpace(machineCode))
        {
            return TransportResult<ConfigApplyResponse>.Fail("machineCode is required for MQTT config apply");
        }

        var requestId = string.IsNullOrWhiteSpace(request.RequestId)
            ? "CFG_APPLY_" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
            : request.RequestId;

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic("edge/config/apply/request", machineCode);
            var replyTopic = ScopedTopic("edge/config/apply/reply", machineCode);
            await client.SubscribeAsync(replyTopic, ControlQos, cancellationToken);

            var payload = JsonSerializer.Serialize(
                request with
                {
                    RequestId = requestId,
                    MachineCode = machineCode,
                    PasswordSha256 = ""
                }
            );
            var payloadBytes = Encoding.UTF8.GetBytes(payload);
            if (payloadBytes.Length <= ConfigApplySinglePayloadLimitBytes)
            {
                await client.PublishAsync(requestTopic, payload, ControlQos, cancellationToken);
            }
            else
            {
                var chunkCount = (payloadBytes.Length + ConfigApplyChunkBytes - 1) / ConfigApplyChunkBytes;
                for (var i = 0; i < chunkCount; ++i)
                {
                    var offset = i * ConfigApplyChunkBytes;
                    var count = Math.Min(ConfigApplyChunkBytes, payloadBytes.Length - offset);
                    var chunkPayload = JsonSerializer.Serialize(new
                    {
                        requestId,
                        machineCode,
                        success = true,
                        chunked = true,
                        chunkIndex = i + 1,
                        chunkCount,
                        totalBytes = payloadBytes.Length,
                        payloadHex = Convert.ToHexString(payloadBytes.AsSpan(offset, count)),
                        ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                    });
                    await client.PublishAsync(requestTopic, chunkPayload, ControlQos, cancellationToken);
                }
            }

            ConfigApplyResponse? reply = null;
            var waitStopReason = "";
            await client.ReadMatchingAsync(
                new[] { replyTopic },
                TimeSpan.FromSeconds(90),
                cancellationToken,
                message =>
                {
                    var value = TryParseConfigApplyReply(message.Payload);
                    if (value is null || !string.Equals(value.RequestId, requestId, StringComparison.Ordinal))
                    {
                        return false;
                    }
                    if (IsChunkAck(message.Payload))
                    {
                        return false;
                    }
                    reply = value;
                    return true;
                },
                TimeSpan.FromSeconds(45),
                reason => waitStopReason = reason
            );

            return reply is null
                ? TransportResult<ConfigApplyResponse>.Fail(
                    $"no matching MQTT config apply reply received on {replyTopic} for requestId={requestId}" +
                    (string.IsNullOrWhiteSpace(waitStopReason) ? "" : $"; wait stopped: {waitStopReason}")
                )
                : TransportResult<ConfigApplyResponse>.Ok(reply, reply.Message);
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<ConfigApplyResponse>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<ConfigFileOperationResponse>> DeleteConfigFileAsync(
        DeviceEndpoint endpoint,
        ConfigFileOperationRequest request,
        CancellationToken cancellationToken
    ) => ExecuteConfigFileOperationAsync(
        endpoint,
        request,
        "edge/config/delete/request",
        "edge/config/delete/reply",
        "MQTT config delete",
        cancellationToken
    );

    public Task<TransportResult<ConfigFileOperationResponse>> RestoreConfigBackupAsync(
        DeviceEndpoint endpoint,
        ConfigFileOperationRequest request,
        CancellationToken cancellationToken
    ) => ExecuteConfigFileOperationAsync(
        endpoint,
        request,
        "edge/config/restore/request",
        "edge/config/restore/reply",
        "MQTT config restore",
        cancellationToken
    );

    public async Task<TransportResult<BatchControlResponse>> ExecuteBatchControlAsync(
        DeviceEndpoint endpoint,
        BatchControlRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.Commands.Any(command => command.HighPriority))
        {
            return TransportResult<BatchControlResponse>.Fail(
                GatewayDesktop.Infrastructure.Security.RemoteHighPriorityPolicy.RejectionMessage);
        }
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<BatchControlResponse>.Fail(error);
        }

        var machineCode = string.IsNullOrWhiteSpace(request.MachineCode)
            ? endpoint.MachineCode
            : request.MachineCode;
        if (string.IsNullOrWhiteSpace(machineCode))
        {
            return TransportResult<BatchControlResponse>.Fail("machineCode is required for MQTT batch control");
        }

        var commands = request.Commands
            .Where(command => command.Index != 0)
            .Select((command, index) => NormalizeControlCommand(command, request.RequestId, index))
            .ToArray();
        if (commands.Length == 0)
        {
            return TransportResult<BatchControlResponse>.Fail("batch control requires at least one command");
        }

        if (commands.Select(command => command.CmdId).Distinct(StringComparer.Ordinal).Count() != commands.Length)
        {
            return TransportResult<BatchControlResponse>.Fail("MQTT batch control cmdId values must be unique");
        }

        var requestId = string.IsNullOrWhiteSpace(request.RequestId)
            ? "CTRL_BATCH_" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
            : request.RequestId;
        var startedAt = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var results = new List<ControlCommandResult>();

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic("edge/command/request", machineCode);
            var replyTopic = ScopedTopic("edge/command/reply", machineCode);
            await client.SubscribeAsync(replyTopic, ControlQos, cancellationToken);

            foreach (var command in commands)
            {
                var payload = JsonSerializer.Serialize(
                    new
                    {
                        cmdId = command.CmdId,
                        machineCode,
                        meterCode = command.MeterCode,
                        pointCode = command.PointCode,
                        index = command.Index,
                        value = command.Value,
                        source = string.IsNullOrWhiteSpace(command.Source) ? "gateway-desktop" : command.Source,
                        ts = command.Ts > 0 ? command.Ts : DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                        highPriority = command.HighPriority
                    }
                );
                await client.PublishAsync(requestTopic, payload, ControlQos, cancellationToken);
            }

            var pending = commands
                .Select(command => command.CmdId)
                .ToHashSet(StringComparer.Ordinal);
            await client.ReadMatchingAsync(
                new[] { replyTopic },
                TimeSpan.FromSeconds(Math.Clamp(commands.Length * 2, 12, 60)),
                cancellationToken,
                message =>
                {
                    var reply = TryParseCommandReply(message.Payload);
                    if (reply is null || !pending.Contains(reply.CmdId))
                    {
                        return false;
                    }

                    var source = commands.First(command => string.Equals(command.CmdId, reply.CmdId, StringComparison.Ordinal));
                    if (!MatchesOptionalControlIdentity(reply.MachineCode, machineCode)
                        || !MatchesOptionalControlIdentity(reply.MeterCode, source.MeterCode)
                        || !MatchesOptionalControlIdentity(reply.PointCode, source.PointCode)
                        || (reply.Index != 0 && reply.Index != source.Index))
                    {
                        return false;
                    }

                    pending.Remove(reply.CmdId);
                    results.Add(reply with
                    {
                        MachineCode = string.IsNullOrWhiteSpace(reply.MachineCode) ? machineCode : reply.MachineCode,
                        MeterCode = string.IsNullOrWhiteSpace(reply.MeterCode) ? source.MeterCode : reply.MeterCode,
                        PointCode = string.IsNullOrWhiteSpace(reply.PointCode) ? source.PointCode : reply.PointCode,
                        Index = reply.Index == 0 ? source.Index : reply.Index,
                        Value = source.Value,
                        Ts = reply.Ts == 0 ? DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() : reply.Ts
                    });
                    return pending.Count == 0;
                },
                TimeSpan.FromSeconds(12)
            );

            foreach (var command in commands.Where(command => pending.Contains(command.CmdId)))
            {
                results.Add(new ControlCommandResult(
                    command.CmdId,
                    machineCode,
                    command.MeterCode,
                    command.PointCode,
                    command.Index,
                    command.Value,
                    false,
                    "no matching MQTT command reply received",
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                ));
            }

            results = results
                .OrderBy(result => Array.FindIndex(commands, command => string.Equals(command.CmdId, result.CmdId, StringComparison.Ordinal)))
                .ToList();
            var accepted = results.Count(result => result.Success);
            var failed = results.Count - accepted;
            var response = new BatchControlResponse(
                requestId,
                machineCode,
                failed == 0,
                commands.Length,
                accepted,
                failed,
                failed == 0
                    ? $"MQTT batch control accepted {accepted}/{commands.Length}"
                    : $"MQTT batch control accepted {accepted}/{commands.Length}, failed {failed}",
                results,
                startedAt
            );
            return TransportResult<BatchControlResponse>.Ok(response, response.Message);
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or OperationCanceledException)
        {
            if (results.Count == 0)
            {
                return TransportResult<BatchControlResponse>.Fail(ex.Message);
            }

            // Only correlated callback results are evidence; absent replies remain unknown to callers.
            var confirmed = results
                .OrderBy(result => Array.FindIndex(commands, command => string.Equals(command.CmdId, result.CmdId, StringComparison.Ordinal)))
                .ToArray();
            var accepted = confirmed.Count(result => result.Success);
            return new TransportResult<BatchControlResponse>
            {
                Success = false,
                Message = ex.Message,
                Value = new BatchControlResponse(requestId, machineCode, false, commands.Length,
                    accepted, confirmed.Length - accepted, ex.Message, confirmed, startedAt)
            };
        }
    }

    private static bool MatchesOptionalControlIdentity(string actual, string expected)
        => string.IsNullOrEmpty(actual) || string.Equals(actual, expected, StringComparison.Ordinal);

    private async Task<TransportResult<ConfigFileOperationResponse>> ExecuteConfigFileOperationAsync(
        DeviceEndpoint endpoint,
        ConfigFileOperationRequest request,
        string requestTopicBase,
        string replyTopicBase,
        string label,
        CancellationToken cancellationToken
    )
    {
        if (!TryParseBroker(endpoint.MqttBroker, out var broker, out var error))
        {
            return TransportResult<ConfigFileOperationResponse>.Fail(error);
        }

        var machineCode = string.IsNullOrWhiteSpace(request.MachineCode)
            ? endpoint.MachineCode
            : request.MachineCode;
        if (string.IsNullOrWhiteSpace(machineCode))
        {
            return TransportResult<ConfigFileOperationResponse>.Fail("machineCode is required for MQTT config operation");
        }

        var requestId = string.IsNullOrWhiteSpace(request.RequestId)
            ? "CFG_OP_" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
            : request.RequestId;

        try
        {
            using var client = await MinimalMqttClient.ConnectAsync(endpoint, broker, cancellationToken);
            var requestTopic = ScopedTopic(requestTopicBase, machineCode);
            var replyTopic = ScopedTopic(replyTopicBase, machineCode);
            await client.SubscribeAsync(replyTopic, ControlQos, cancellationToken);

            var payload = JsonSerializer.Serialize(
                request with
                {
                    RequestId = requestId,
                    MachineCode = machineCode,
                    PasswordSha256 = ""
                }
            );
            await client.PublishAsync(requestTopic, payload, ControlQos, cancellationToken);

            var messages = await client.ReadMatchingAsync(
                new[] { replyTopic },
                TimeSpan.FromSeconds(30),
                cancellationToken
            );
            var reply = messages
                .Select(message => JsonSerializer.Deserialize<ConfigFileOperationResponse>(message.Payload, JsonOptions))
                .Where(value => value is not null)
                .LastOrDefault(value => string.Equals(value!.RequestId, requestId, StringComparison.Ordinal));

            return reply is null
                ? TransportResult<ConfigFileOperationResponse>.Fail($"no matching {label} reply received")
                : TransportResult<ConfigFileOperationResponse>.Ok(reply, reply.Message);
        }
        catch (Exception ex) when (ex is SocketException or IOException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            return TransportResult<ConfigFileOperationResponse>.Fail(ex.Message);
        }
    }

    internal static RealtimePointSnapshot? AssembleRealtimeSnapshots(IEnumerable<string> payloads) =>
        AssembleRealtimeSnapshots(payloads, null, null);

    internal static RealtimePointSnapshot? AssembleRealtimeSnapshots(
        IEnumerable<string> payloads, string? expectedMachineCode, string? expectedSessionId)
    {
        var envelopes = new List<RealtimeSnapshotEnvelope>();
        var order = 0;
        foreach (var payload in payloads)
        {
            try
            {
                var envelope = TryParseRealtimeEnvelope(
                    payload, order++, expectedMachineCode, expectedSessionId);
                if (envelope is not null)
                {
                    envelopes.Add(envelope);
                }
            }
            catch (JsonException)
            {
                // A malformed or unrelated MQTT message must not discard a complete snapshot received beside it.
            }
        }

        var candidates = new List<(int Order, RealtimePointSnapshot Snapshot)>();
        candidates.AddRange(envelopes
            .Where(envelope => !envelope.Chunked)
            .Select(envelope => (envelope.Order, envelope.Snapshot)));

        foreach (var group in envelopes
                     .Where(envelope => envelope.Chunked)
                     .GroupBy(envelope => (envelope.MachineCode, envelope.SessionId, envelope.ChunkId)))
        {
            var expectedCount = group.Select(envelope => envelope.ChunkCount).Distinct().ToArray();
            if (expectedCount.Length != 1 || expectedCount[0] <= 0)
            {
                continue;
            }

            var chunks = group
                .GroupBy(envelope => envelope.ChunkIndex)
                .Select(chunkGroup => chunkGroup.OrderBy(envelope => envelope.Order).Last())
                .OrderBy(envelope => envelope.ChunkIndex)
                .ToArray();
            if (chunks.Length != expectedCount[0] ||
                chunks.Where((chunk, index) => chunk.ChunkIndex != index + 1).Any())
            {
                continue;
            }

            var points = chunks.SelectMany(chunk => chunk.Snapshot.Points).ToArray();
            var ts = points.Length > 0
                ? points.Max(point => point.Ts)
                : chunks.Max(chunk => chunk.Snapshot.Ts);
            candidates.Add((
                chunks.Max(chunk => chunk.Order),
                new RealtimePointSnapshot(ts, points.Length, points)));
        }

        return candidates.Count == 0
            ? null
            : candidates.OrderBy(candidate => candidate.Order).Last().Snapshot;
    }

    private static RealtimeSnapshotEnvelope? TryParseRealtimeEnvelope(
        string payload, int order, string? expectedMachineCode = null, string? expectedSessionId = null)
    {
        using var document = JsonDocument.Parse(payload);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        var machineCode = root.TryGetProperty("machineCode", out var machineElement) &&
            machineElement.ValueKind == JsonValueKind.String
            ? machineElement.GetString() ?? ""
            : "";
        var sessionId = root.TryGetProperty("sessionId", out var sessionElement) &&
            sessionElement.ValueKind == JsonValueKind.String
                ? sessionElement.GetString() ?? ""
                : "";
        if ((expectedMachineCode is not null &&
                !string.Equals(machineCode, expectedMachineCode, StringComparison.Ordinal)) ||
            (expectedSessionId is not null &&
                !string.Equals(sessionId, expectedSessionId, StringComparison.Ordinal)))
        {
            return null;
        }

        var hasRootTs = false;
        var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (root.TryGetProperty("ts", out var tsElement) && tsElement.TryGetInt64(out var rootTs))
        {
            hasRootTs = true;
            ts = rootTs;
        }
        var points = new List<RealtimePoint>();
        if (!root.TryGetProperty("meters", out var meters) || meters.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        foreach (var meter in meters.EnumerateArray())
        {
            var meterCode = meter.TryGetProperty("meterCode", out var meterCodeElement)
                ? meterCodeElement.GetString() ?? ""
                : "";
            if (!meter.TryGetProperty("values", out var values) || values.ValueKind != JsonValueKind.Array)
            {
                continue;
            }

            foreach (var value in values.EnumerateArray())
            {
                var point = value.ValueKind == JsonValueKind.Array
                    ? ParseCompactPoint(machineCode, meterCode, value)
                    : ParseObjectPoint(machineCode, meterCode, value);
                if (point is not null)
                {
                    points.Add(point);
                }
            }
        }

        if (!hasRootTs && points.Count > 0)
        {
            ts = points.Max(point => point.Ts);
        }

        var snapshot = new RealtimePointSnapshot(ts, points.Count, points);
        var chunked = root.TryGetProperty("chunked", out var chunkedElement) &&
            chunkedElement.ValueKind == JsonValueKind.True;
        if (!chunked)
        {
            return new RealtimeSnapshotEnvelope(snapshot, machineCode, sessionId, false, string.Empty, 0, 0, order);
        }

        if (!root.TryGetProperty("chunkId", out var chunkIdElement) ||
            chunkIdElement.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(chunkIdElement.GetString()) ||
            !root.TryGetProperty("chunkIndex", out var chunkIndexElement) ||
            !chunkIndexElement.TryGetInt32(out var chunkIndex) ||
            !root.TryGetProperty("chunkCount", out var chunkCountElement) ||
            !chunkCountElement.TryGetInt32(out var chunkCount) ||
            chunkIndex <= 0 ||
            chunkCount <= 0 ||
            chunkIndex > chunkCount)
        {
            return null;
        }

        return new RealtimeSnapshotEnvelope(
            snapshot,
            machineCode,
            sessionId,
            true,
            chunkIdElement.GetString()!,
            chunkIndex,
            chunkCount,
            order);
    }

    private static RealtimePointSnapshot? TryParseRealtimeSnapshot(string payload) =>
        TryParseRealtimeEnvelope(payload, 0)?.Snapshot;

    private sealed record RealtimeSnapshotEnvelope(
        RealtimePointSnapshot Snapshot,
        string MachineCode,
        string SessionId,
        bool Chunked,
        string ChunkId,
        int ChunkIndex,
        int ChunkCount,
        int Order);

    private static RealtimePointSnapshot FilterSnapshotByMeter(
        RealtimePointSnapshot snapshot,
        string meterCode
    )
    {
        if (string.IsNullOrWhiteSpace(meterCode))
        {
            return snapshot;
        }

        var filtered = snapshot.Points
            .Where(point => string.Equals(point.MeterCode, meterCode, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        return snapshot with
        {
            Count = filtered.Length,
            Points = filtered
        };
    }

    private static FullTelemetrySnapshot? TryParseFullTelemetrySnapshot(string topic, string payload)
    {
        var snapshot = TryParseRealtimeSnapshot(payload);
        return snapshot is null
            ? null
            : new FullTelemetrySnapshot(snapshot.Ts, snapshot.Count, topic, payload, snapshot.Points);
    }

    private static SystemStatusSnapshot? TryParseSystemStatus(string payload)
    {
        using var document = JsonDocument.Parse(payload);
        var root = document.RootElement;
        if (!root.TryGetProperty("machineCode", out var machineElement))
        {
            return null;
        }

        var cellular = root.TryGetProperty("cellular", out var cellularElement) &&
            cellularElement.ValueKind == JsonValueKind.Object
            ? JsonSerializer.Deserialize<CellularStatus>(cellularElement.GetRawText(), JsonOptions)
            : null;

        return new SystemStatusSnapshot(
            machineElement.GetString() ?? "",
            ReadDouble(root, "cpuUsage"),
            ReadDouble(root, "memUsage"),
            ReadDouble(root, "diskUsage"),
            ReadDouble(root, "load1"),
            ReadInt(root, "processCount"),
            cellular ?? EmptyCellularStatus(ReadLong(root, "ts")),
            Array.Empty<DeviceServiceStatus>(),
            ReadLong(root, "ts")
        );
    }

    private static RealtimePoint? ParseCompactPoint(string machineCode, string meterCode, JsonElement value)
    {
        var items = value.EnumerateArray().ToArray();
        if (items.Length < 7 ||
            !items[0].TryGetUInt32(out var index) ||
            !TryGetPointValue(items[2], out var pointValue) ||
            !items[3].TryGetInt32(out var quality) ||
            !items[4].TryGetInt64(out var ts))
        {
            return null;
        }

        return new RealtimePoint(
            index,
            machineCode,
            meterCode,
            items[1].GetString() ?? "",
            items.Length > 7 ? items[7].GetString() : null,
            null,
            pointValue,
            quality,
            ts,
            items[6].ValueKind == JsonValueKind.True
        );
    }

    private static RealtimePoint? ParseObjectPoint(string machineCode, string meterCode, JsonElement value)
    {
        if (!value.TryGetProperty("index", out var indexElement) ||
            !indexElement.TryGetUInt32(out var index) ||
            !value.TryGetProperty("value", out var valueElement) ||
            !TryGetPointValue(valueElement, out var pointValue) ||
            !value.TryGetProperty("quality", out var qualityElement) ||
            !qualityElement.TryGetInt32(out var quality) ||
            !value.TryGetProperty("ts", out var tsElement) ||
            !tsElement.TryGetInt64(out var ts))
        {
            return null;
        }

        return new RealtimePoint(
            index,
            machineCode,
            meterCode,
            value.TryGetProperty("pointCode", out var pointCodeElement) ? pointCodeElement.GetString() ?? "" : "",
            ReadOptionalString(value, "name"),
            ReadOptionalString(value, "pointName"),
            pointValue,
            quality,
            ts,
            value.TryGetProperty("stale", out var staleElement) && staleElement.ValueKind == JsonValueKind.True
        );
    }

    private static string? ReadOptionalString(JsonElement root, string name) =>
        root.TryGetProperty(name, out var element) && element.ValueKind == JsonValueKind.String
            ? element.GetString()
            : null;

    private static bool TryGetPointValue(JsonElement element, out double? value)
    {
        value = null;
        if (element.ValueKind == JsonValueKind.Null) return true;
        if (!TryGetDouble(element, out var number)) return false;
        value = number;
        return true;
    }

    private static bool TryGetDouble(JsonElement element, out double value)
    {
        if (element.ValueKind == JsonValueKind.Number && element.TryGetDouble(out value))
        {
            return true;
        }

        if (element.ValueKind == JsonValueKind.String &&
            double.TryParse(element.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out value))
        {
            return true;
        }

        value = 0;
        return false;
    }

    private static double ReadDouble(JsonElement root, string name) =>
        root.TryGetProperty(name, out var element) && TryGetDouble(element, out var value)
            ? value
            : 0;

    private static int ReadInt(JsonElement root, string name) =>
        root.TryGetProperty(name, out var element) && element.TryGetInt32(out var value)
            ? value
            : 0;

    private static long ReadLong(JsonElement root, string name) =>
        root.TryGetProperty(name, out var element) && element.TryGetInt64(out var value)
            ? value
            : DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    private static CellularStatus EmptyCellularStatus(long ts) => new(
        false,
        false,
        false,
        false,
        false,
        false,
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        -1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        ts
    );

    private static OtaJobResponse? TryParseOtaReply(string payload, string packageType)
    {
        var reply = JsonSerializer.Deserialize<MqttOtaReply>(payload, JsonOptions);
        return reply is null
            ? null
            : new OtaJobResponse(reply.Accepted, reply.JobId, packageType, reply.Message);
    }

    private static OtaStatusItem? TryParseOtaStatus(string payload) =>
        JsonSerializer.Deserialize<OtaStatusItem>(payload, JsonOptions);

    private static ConfigApplyResponse? TryParseConfigApplyReply(string payload)
    {
        try
        {
            return JsonSerializer.Deserialize<ConfigApplyResponse>(payload, JsonOptions);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static bool IsChunkAck(string payload)
    {
        try
        {
            using var document = JsonDocument.Parse(payload);
            return document.RootElement.TryGetProperty("chunked", out var chunked) &&
                chunked.ValueKind == JsonValueKind.True;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static ControlCommandItem NormalizeControlCommand(
        ControlCommandItem command,
        string requestId,
        int index
    )
    {
        var cmdId = string.IsNullOrWhiteSpace(command.CmdId)
            ? $"{(string.IsNullOrWhiteSpace(requestId) ? "CTRL_BATCH" : requestId)}_{index + 1:D3}"
            : command.CmdId;
        return command with
        {
            CmdId = cmdId,
            Source = string.IsNullOrWhiteSpace(command.Source) ? "gateway-desktop" : command.Source,
            Ts = command.Ts > 0 ? command.Ts : DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
        };
    }

    private static ControlCommandResult? TryParseCommandReply(string payload)
    {
        using var document = JsonDocument.Parse(payload);
        var root = document.RootElement;
        var cmdId = ReadOptionalString(root, "cmdId") ?? "";
        if (string.IsNullOrWhiteSpace(cmdId))
        {
            return null;
        }

        var success = root.TryGetProperty("success", out var successElement) &&
            successElement.ValueKind == JsonValueKind.True;
        var index = root.TryGetProperty("index", out var indexElement) &&
            indexElement.TryGetUInt32(out var parsedIndex)
                ? parsedIndex
                : 0;
        return new ControlCommandResult(
            cmdId,
            ReadOptionalString(root, "machineCode") ?? "",
            ReadOptionalString(root, "meterCode") ?? "",
            ReadOptionalString(root, "pointCode") ?? "",
            index,
            ReadDouble(root, "value"),
            success,
            ReadOptionalString(root, "message") ?? "",
            ReadLong(root, "ts"),
            ReadOptionalString(root, "stage") ?? "",
            ReadLong(root, "requestedAt"),
            ReadLong(root, "acceptedAt"),
            ReadLong(root, "writeStartedAt"),
            ReadLong(root, "writeCompletedAt"),
            ReadLong(root, "queueDelayMs"),
            ReadLong(root, "deviceWriteMs"),
            ReadLong(root, "edgeElapsedMs"),
            ReadLong(root, "totalElapsedMs"),
            root.TryGetProperty("verifyAttempted", out var verifyAttemptedElement) &&
                verifyAttemptedElement.ValueKind == JsonValueKind.True,
            root.TryGetProperty("verifyPassed", out var verifyPassedElement) &&
                verifyPassedElement.ValueKind == JsonValueKind.True,
            root.TryGetProperty("highPriority", out var highPriorityElement) &&
                highPriorityElement.ValueKind == JsonValueKind.True
        );
    }

    private sealed class ConfigSnapshotAssemblyState
    {
        private readonly string requestId;
        private readonly SortedDictionary<int, byte[]> chunks = new();
        private ulong totalBytes;
        private int nonMatchingPayloads;
        private string lastError = "";

        public ConfigSnapshotAssemblyState(string requestId)
        {
            this.requestId = requestId;
        }

        public int MatchingPayloads { get; private set; }

        public int ReceivedChunks => chunks.Count;

        public int TotalChunks { get; private set; }

        public bool IsChunkedIncomplete => TotalChunks > 0 && chunks.Count < TotalChunks;

        public IReadOnlyList<int> MissingChunks
        {
            get
            {
                if (TotalChunks <= 0 || chunks.Count >= TotalChunks)
                {
                    return Array.Empty<int>();
                }

                var missing = new List<int>();
                for (var i = 1; i <= TotalChunks; ++i)
                {
                    if (!chunks.ContainsKey(i))
                    {
                        missing.Add(i);
                    }
                }

                return missing;
            }
        }

        public ConfigSnapshot? Accept(string payload)
        {
            try
            {
                using var document = JsonDocument.Parse(payload);
                var root = document.RootElement;
                var payloadRequestId = root.TryGetProperty("requestId", out var requestElement)
                    ? requestElement.GetString() ?? ""
                    : "";
                if (!string.Equals(payloadRequestId, requestId, StringComparison.Ordinal))
                {
                    ++nonMatchingPayloads;
                    return null;
                }

                ++MatchingPayloads;

                if (root.TryGetProperty("success", out var successElement) &&
                    successElement.ValueKind == JsonValueKind.False)
                {
                    var message = root.TryGetProperty("message", out var messageElement)
                        ? messageElement.GetString() ?? "device returned config pull failure"
                        : "device returned config pull failure";
                    throw new InvalidOperationException(message);
                }

                var chunked = root.TryGetProperty("chunked", out var chunkedElement) &&
                    chunkedElement.ValueKind == JsonValueKind.True;
                if (!chunked)
                {
                    return JsonSerializer.Deserialize<ConfigSnapshot>(payload, JsonOptions);
                }

                if (!root.TryGetProperty("chunkIndex", out var chunkIndexElement) ||
                    !chunkIndexElement.TryGetInt32(out var chunkIndex) ||
                    !root.TryGetProperty("chunkCount", out var countElement) ||
                    !countElement.TryGetInt32(out var currentChunkCount) ||
                    !root.TryGetProperty("payloadHex", out var hexElement))
                {
                    lastError = "config pull chunk metadata is incomplete";
                    return null;
                }

                if (chunkIndex <= 0 || currentChunkCount <= 0 || chunkIndex > currentChunkCount)
                {
                    lastError = $"invalid config pull chunk index {chunkIndex}/{currentChunkCount}";
                    return null;
                }

                if (TotalChunks == 0)
                {
                    TotalChunks = currentChunkCount;
                    if (root.TryGetProperty("totalBytes", out var totalElement) &&
                        totalElement.TryGetUInt64(out var parsedTotal))
                    {
                        totalBytes = parsedTotal;
                    }
                }
                else if (TotalChunks != currentChunkCount)
                {
                    throw new InvalidOperationException("config pull chunk metadata changed");
                }

                chunks[chunkIndex] = Convert.FromHexString(hexElement.GetString() ?? "");
                if (chunks.Count < TotalChunks)
                {
                    return null;
                }

                var bytes = new List<byte>();
                for (var i = 1; i <= TotalChunks; ++i)
                {
                    if (!chunks.TryGetValue(i, out var part))
                    {
                        lastError = $"config pull chunk {i}/{TotalChunks} is missing";
                        return null;
                    }
                    bytes.AddRange(part);
                }

                if (totalBytes > 0 && (ulong)bytes.Count != totalBytes)
                {
                    throw new InvalidOperationException("config pull assembled size mismatch");
                }

                var json = Encoding.UTF8.GetString(bytes.ToArray());
                return JsonSerializer.Deserialize<ConfigSnapshot>(json, JsonOptions);
            }
            catch (Exception ex) when (ex is JsonException or FormatException)
            {
                lastError = ex.Message;
                throw;
            }
        }

        public string BuildFailureMessage(string replyTopic, string requestId, string waitStopReason)
        {
            var waitSuffix = string.IsNullOrWhiteSpace(waitStopReason)
                ? ""
                : $"; wait stopped: {waitStopReason}";

            if (MatchingPayloads == 0)
            {
                return nonMatchingPayloads > 0
                    ? $"received {nonMatchingPayloads} config reply message(s) on {replyTopic}, but none matched requestId={requestId}; check whether another desktop session is pulling config{waitSuffix}"
                    : $"no matching MQTT config snapshot received on {replyTopic} for requestId={requestId}; check machineCode and edge/config/pull/request subscription{waitSuffix}";
            }

            if (TotalChunks > 0 && ReceivedChunks < TotalChunks)
            {
                return $"MQTT config snapshot incomplete on {replyTopic}: received {ReceivedChunks}/{TotalChunks} chunks for requestId={requestId}" +
                    waitSuffix +
                    (string.IsNullOrWhiteSpace(lastError) ? "" : $"; last error: {lastError}");
            }

            return $"MQTT config snapshot reply was received on {replyTopic}, but could not be assembled for requestId={requestId}" +
                waitSuffix +
                (string.IsNullOrWhiteSpace(lastError) ? "" : $"; last error: {lastError}");
        }
    }

    private static string FormatDuration(TimeSpan duration)
    {
        return duration.TotalMinutes >= 1
            ? $"{duration.TotalMinutes:0.#}m"
            : $"{duration.TotalSeconds:0.#}s";
    }

    internal static bool TryBuildOnlineDeviceForTest(string topic, string payload, out OnlineDevice device) =>
        TryBuildOnlineDevice(new MqttMessage(topic, payload), out device);

    private static bool TryBuildOnlineDevice(MqttMessage message, out OnlineDevice device)
    {
        var machineCode = "";
        var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (TryReadMachineCodeFromPayload(message.Payload, out var payloadMachineCode, out var payloadTs))
        {
            machineCode = payloadMachineCode;
            ts = payloadTs;
        }
        else if (TryReadMachineCodeFromTopic(message.Topic, out var topicMachineCode))
        {
            machineCode = topicMachineCode;
        }

        if (string.IsNullOrWhiteSpace(machineCode))
        {
            device = new OnlineDevice("", message.Topic, ts);
            return false;
        }

        device = new OnlineDevice(machineCode, message.Topic, ts);
        return true;
    }

    private static bool TryReadMachineCodeFromPayload(string payload, out string machineCode, out long ts)
    {
        machineCode = "";
        ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (string.IsNullOrWhiteSpace(payload))
        {
            return false;
        }

        try
        {
            using var document = JsonDocument.Parse(payload);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                return false;
            }

            TryReadMachineCodeFromJson(root, ref machineCode, ref ts);
            return IsValidMachineCodeCandidate(machineCode);
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static void TryReadMachineCodeFromJson(JsonElement element, ref string machineCode, ref long ts)
    {
        switch (element.ValueKind)
        {
            case JsonValueKind.Object:
                foreach (var property in element.EnumerateObject())
                {
                    if (string.Equals(property.Name, "ts", StringComparison.OrdinalIgnoreCase) &&
                        property.Value.TryGetInt64(out var parsedTs))
                    {
                        ts = parsedTs;
                    }
                    else if (string.IsNullOrWhiteSpace(machineCode) &&
                        IsMachineCodeProperty(property.Name) &&
                        property.Value.ValueKind == JsonValueKind.String)
                    {
                        machineCode = property.Value.GetString() ?? "";
                    }

                    if (string.IsNullOrWhiteSpace(machineCode) ||
                        !string.Equals(property.Name, "points", StringComparison.OrdinalIgnoreCase))
                    {
                        TryReadMachineCodeFromJson(property.Value, ref machineCode, ref ts);
                    }
                }
                break;

            case JsonValueKind.Array:
                foreach (var item in element.EnumerateArray())
                {
                    TryReadMachineCodeFromJson(item, ref machineCode, ref ts);
                    if (!string.IsNullOrWhiteSpace(machineCode))
                    {
                        break;
                    }
                }
                break;
        }
    }

    /// <summary>识别设备上报 payload 中常见的 machineCode 字段名变体。</summary>
    private static bool IsMachineCodeProperty(string name)
    {
        return string.Equals(name, "machineCode", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "machine_code", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "machineId", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "machine_id", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "gatewayCode", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "gateway_code", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "gatewayId", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "gateway_id", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "clientId", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "client_id", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "mc", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "deviceCode", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "device_code", StringComparison.OrdinalIgnoreCase)
            || string.Equals(name, "sn", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryReadMachineCodeFromTopic(string topic, out string machineCode)
    {
        machineCode = "";
        var parts = topic.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0)
        {
            return false;
        }

        machineCode = parts[^1];
        return !IsGenericDiscoveryTopicSuffix(machineCode) && IsValidMachineCodeCandidate(machineCode);
    }

    private static bool IsGenericDiscoveryTopicSuffix(string value)
    {
        return string.Equals(value, "status", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "telemetry", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "full", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "realtime", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "monitor", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "points", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "alert", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "ota", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "config", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "reply", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsValidMachineCodeCandidate(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > 96)
        {
            return false;
        }

        foreach (var ch in value)
        {
            if (!char.IsLetterOrDigit(ch) && ch is not '_' and not '-' and not '.')
            {
                return false;
            }
        }

        return true;
    }

    private static string ScopedTopic(string topic, string machineCode) =>
        string.IsNullOrWhiteSpace(machineCode) || topic.EndsWith("/" + machineCode, StringComparison.Ordinal)
            ? topic
            : topic + "/" + machineCode;

    private string BuildRealtimeSessionId(string machineCode, string meterCode)
    {
        var scope = JsonSerializer.Serialize(new[] { machineCode, meterCode });
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(scope)))[..16];
        return "STUDIO_RT_" + _realtimeSessionOwner + "_" + hash;
    }

    private static bool TryParseBroker(
        string broker,
        out MqttBroker brokerEndpoint,
        out string error
    )
    {
        brokerEndpoint = new MqttBroker("127.0.0.1", 1883, false);
        error = "";

        if (string.IsNullOrWhiteSpace(broker))
        {
            error = "mqtt broker is required";
            return false;
        }

        if (Uri.TryCreate(broker, UriKind.Absolute, out var uri))
        {
            brokerEndpoint = new MqttBroker(
                uri.Host,
                uri.Port > 0 ? uri.Port : (uri.Scheme is "ssl" or "mqtts" ? 8883 : 1883),
                uri.Scheme is "ssl" or "mqtts"
            );
            return !string.IsNullOrWhiteSpace(brokerEndpoint.Host);
        }

        var parts = broker.Split(':', 2);
        var host = parts[0].Trim();
        var port = 1883;
        if (parts.Length == 2 && !int.TryParse(parts[1], out port))
        {
            error = "mqtt broker port is invalid";
            return false;
        }

        if (string.IsNullOrWhiteSpace(host))
        {
            error = "mqtt broker host is required";
            return false;
        }

        brokerEndpoint = new MqttBroker(host, port, false);
        return true;
    }

    private sealed record MqttBroker(string Host, int Port, bool Tls);

    internal sealed record MqttPublication(string Topic, string Payload, int Qos);

    private sealed record MqttMessage(string Topic, string Payload);

    private sealed record MqttOtaReply(string JobId, string MachineCode, bool Accepted, string Message, long Ts);

    private sealed class MinimalMqttClient : IDisposable
    {
        private static readonly TimeSpan MqttPingInterval = TimeSpan.FromSeconds(30);

        private readonly TcpClient tcpClient;
        private readonly Stream stream;
        private readonly SemaphoreSlim writeLock = new(1, 1);
        private readonly Queue<MqttMessage> pendingMessages = new();
        private readonly List<ushort> pendingInboundQoS2PacketIds = new();
        private DateTimeOffset lastOutboundAt = DateTimeOffset.UtcNow;
        private ushort packetId;
        private int maxInboundPacketBytes = int.MaxValue;
        private int maxPendingMessages = int.MaxValue;

        private MinimalMqttClient(TcpClient tcpClient, Stream stream)
        {
            this.tcpClient = tcpClient;
            this.stream = stream;
        }

        public static async Task<MinimalMqttClient> ConnectAsync(
            DeviceEndpoint endpoint,
            MqttBroker broker,
            CancellationToken cancellationToken,
            int maxInboundPacketBytes = int.MaxValue,
            int maxPendingMessages = int.MaxValue
        )
        {
            var tcpClient = new TcpClient();
            await tcpClient.ConnectAsync(broker.Host, broker.Port, cancellationToken);
            Stream stream = tcpClient.GetStream();
            if (broker.Tls)
            {
                var sslStream = new SslStream(
                    stream,
                    false,
                    (_, certificate, chain, errors) => ValidateServerCertificate(endpoint, certificate, chain, errors)
                );
                var authOptions = new SslClientAuthenticationOptions
                {
                    TargetHost = broker.Host,
                    ClientCertificates = BuildClientCertificates(endpoint),
                    CertificateRevocationCheckMode = X509RevocationMode.NoCheck
                };
                try
                {
                    await sslStream.AuthenticateAsClientAsync(authOptions, cancellationToken);
                }
                catch (Exception ex) when (ex is AuthenticationException or IOException)
                {
                    throw new InvalidOperationException(BuildTlsHandshakeFailureMessage(endpoint, ex.Message), ex);
                }
                stream = sslStream;
            }

            var client = new MinimalMqttClient(tcpClient, stream)
            {
                maxInboundPacketBytes = maxInboundPacketBytes,
                maxPendingMessages = maxPendingMessages
            };
            try
            {
                await client.SendConnectAsync(endpoint, cancellationToken);
                return client;
            }
            catch (Exception ex) when (broker.Tls && ex is AuthenticationException or IOException)
            {
                client.Dispose();
                throw new InvalidOperationException(BuildTlsHandshakeFailureMessage(endpoint, ex.Message), ex);
            }
            catch
            {
                client.Dispose();
                throw;
            }
        }

        private static string BuildTlsHandshakeFailureMessage(DeviceEndpoint endpoint, string detail)
        {
            var suffix = string.IsNullOrWhiteSpace(endpoint.MqttTlsCertFile) ||
                string.IsNullOrWhiteSpace(endpoint.MqttTlsKeyFile)
                    ? "broker may require a client certificate; configure mqtt.tls.certFile and mqtt.tls.keyFile"
                    : "check mqtt.tls.caFile/certFile/keyFile and broker certificate policy";
            return $"MQTT TLS handshake failed: {detail}; {suffix}";
        }

        private static X509CertificateCollection BuildClientCertificates(DeviceEndpoint endpoint)
        {
            var certFile = ExpandLocalPath(endpoint.MqttTlsCertFile);
            var keyFile = ExpandLocalPath(endpoint.MqttTlsKeyFile);
            if (string.IsNullOrWhiteSpace(certFile) && string.IsNullOrWhiteSpace(keyFile))
            {
                return new X509CertificateCollection();
            }

            if (string.IsNullOrWhiteSpace(certFile) || string.IsNullOrWhiteSpace(keyFile))
            {
                throw new InvalidOperationException("MQTT TLS certFile and keyFile must be configured together");
            }

            EnsureReadableFile(certFile, "MQTT TLS certFile");
            EnsureReadableFile(keyFile, "MQTT TLS keyFile");
            using var certificate = X509Certificate2.CreateFromPemFile(certFile, keyFile);
            return new X509CertificateCollection
            {
                new X509Certificate2(certificate.Export(X509ContentType.Pkcs12))
            };
        }

        private static bool ValidateServerCertificate(
            DeviceEndpoint endpoint,
            X509Certificate? certificate,
            X509Chain? chain,
            SslPolicyErrors sslPolicyErrors
        )
        {
            if (endpoint.MqttTlsInsecureSkipVerify || string.IsNullOrWhiteSpace(endpoint.MqttTlsCaFile))
            {
                return true;
            }

            if (certificate is null || (sslPolicyErrors & SslPolicyErrors.RemoteCertificateNameMismatch) != 0)
            {
                return false;
            }

            try
            {
                var caFile = ExpandLocalPath(endpoint.MqttTlsCaFile);
                EnsureReadableFile(caFile, "MQTT TLS caFile");
                var caCertificates = new X509Certificate2Collection();
                caCertificates.ImportFromPemFile(caFile);

                using var serverCertificate = new X509Certificate2(certificate);
                using var customChain = new X509Chain();
                customChain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                customChain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
                customChain.ChainPolicy.CustomTrustStore.AddRange(caCertificates);
                customChain.ChainPolicy.ExtraStore.AddRange(caCertificates);
                return customChain.Build(serverCertificate);
            }
            catch (CryptographicException)
            {
                return false;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        private static void EnsureReadableFile(string path, string label)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                throw new InvalidOperationException($"{label} is required");
            }

            if (!File.Exists(path))
            {
                throw new InvalidOperationException($"{label} does not exist: {path}");
            }
        }

        private static string ExpandLocalPath(string path)
        {
            var expanded = Environment.ExpandEnvironmentVariables(path.Trim());
            if (expanded.StartsWith("~/", StringComparison.Ordinal) ||
                expanded.StartsWith("~\\", StringComparison.Ordinal))
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    expanded[2..]
                );
            }

            return expanded;
        }

        public async Task SubscribeAsync(string topic, int qos, CancellationToken cancellationToken)
        {
            qos = ClampQos(qos);
            var variable = new List<byte>();
            var id = NextPacketId();
            variable.Add((byte)(id >> 8));
            variable.Add((byte)(id & 0xFF));
            var payload = new List<byte>();
            WriteUtf8(payload, topic);
            payload.Add((byte)qos);

            var packet = BuildPacket(0x82, variable.Concat(payload));
            await WritePacketAsync(packet, cancellationToken);
            var subAck = await ReadPacketAsync(cancellationToken);
            if (subAck.Type != 0x90)
            {
                throw new InvalidOperationException("MQTT broker did not return SUBACK");
            }
        }

        public async Task PublishAsync(string topic, string payload, int qos, CancellationToken cancellationToken)
        {
            qos = ClampQos(qos);
            var body = new List<byte>();
            WriteUtf8(body, topic);
            ushort id = 0;
            if (qos > 0)
            {
                id = NextPacketId();
                body.Add((byte)(id >> 8));
                body.Add((byte)(id & 0xFF));
            }
            body.AddRange(Encoding.UTF8.GetBytes(payload));
            var packetType = qos == 0
                ? (byte)0x30
                : qos == 1
                    ? (byte)0x32
                    : (byte)0x34;
            var packet = BuildPacket(packetType, body);
            await WritePacketAsync(packet, cancellationToken);
            if (qos == 1)
            {
                await WaitForAckAsync(0x40, id, "PUBACK", cancellationToken);
            }
            else if (qos == 2)
            {
                await WaitForAckAsync(0x50, id, "PUBREC", cancellationToken);
                await SendPubRelAsync(id, cancellationToken);
                await WaitForAckAsync(0x70, id, "PUBCOMP", cancellationToken);
            }
        }

        public async Task<IReadOnlyList<MqttMessage>> ReadMatchingAsync(
            IReadOnlyCollection<string> topics,
            TimeSpan timeout,
            CancellationToken cancellationToken,
            Func<MqttMessage, bool>? onMessage = null,
            TimeSpan? idleTimeout = null,
            Action<string>? onWaitStopped = null,
            Func<TimeSpan?>? idleTimeoutProvider = null
        )
        {
            var messages = new List<MqttMessage>();
            var stopFromPending = await DrainPendingMessagesAsync(topics, messages, onMessage, cancellationToken);
            if (stopFromPending)
            {
                return messages;
            }

            if (pendingInboundQoS2PacketIds.Count > 0)
            {
                await CompletePendingInboundQoS2Async(cancellationToken);
                stopFromPending = await DrainPendingMessagesAsync(topics, messages, onMessage, cancellationToken);
                if (stopFromPending)
                {
                    return messages;
                }
            }

            var startAt = DateTimeOffset.UtcNow;
            var lastMatchingAt = startAt;
            var completed = false;
            var stopReason = "";
            while (true)
            {
                var now = DateTimeOffset.UtcNow;
                var totalRemaining = timeout - (now - startAt);
                var activeIdleTimeout = idleTimeoutProvider?.Invoke() ?? idleTimeout;
                if (totalRemaining <= TimeSpan.Zero)
                {
                    stopReason = $"total timeout {FormatDuration(timeout)} elapsed";
                    break;
                }

                var readTimeout = totalRemaining;
                if (activeIdleTimeout is { } idleLimit)
                {
                    var idleRemaining = idleLimit - (now - lastMatchingAt);
                    if (idleRemaining <= TimeSpan.Zero)
                    {
                        stopReason = $"no matching MQTT message for {FormatDuration(idleLimit)}";
                        break;
                    }

                    if (idleRemaining < readTimeout)
                    {
                        readTimeout = idleRemaining;
                    }
                }

                MqttPacket? packet;
                try
                {
                    packet = await ReadPacketWithKeepAliveAsync(readTimeout, cancellationToken);
                }
                catch (OperationCanceledException)
                {
                    stopReason = cancellationToken.IsCancellationRequested
                        ? "operation was cancelled"
                        : activeIdleTimeout is { } catchIdleLimit && DateTimeOffset.UtcNow - lastMatchingAt >= catchIdleLimit
                            ? $"no matching MQTT message for {FormatDuration(catchIdleLimit)}"
                            : $"total timeout {FormatDuration(timeout)} elapsed";
                    break;
                }

                if (packet is null)
                {
                    stopReason = activeIdleTimeout is { } readIdleLimit && DateTimeOffset.UtcNow - lastMatchingAt >= readIdleLimit
                        ? $"no matching MQTT message for {FormatDuration(readIdleLimit)}"
                        : $"total timeout {FormatDuration(timeout)} elapsed";
                    break;
                }

                if (packet.Type == 0xD0)
                {
                    continue;
                }

                if (packet.Type == 0x62 && TryReadPacketId(packet, out var queuedPubRelId))
                {
                    await SendPubCompAsync(queuedPubRelId, cancellationToken);
                    pendingInboundQoS2PacketIds.Remove(queuedPubRelId);
                    continue;
                }

                if ((packet.Type & 0xF0) == 0x30)
                {
                    var message = ParsePublish(packet, out var publishPacketId, out var publishQos);
                    var shouldStop = false;
                    if (publishQos == 1 && publishPacketId != 0)
                    {
                        await SendPubAckAsync(publishPacketId, cancellationToken);
                    }
                    else if (publishQos == 2 && publishPacketId != 0)
                    {
                        await SendPubRecAsync(publishPacketId, cancellationToken);
                        QueueInboundQoS2(publishPacketId);
                    }

                    if (topics.Any(topic => TopicMatches(topic, message.Topic)))
                    {
                        lastMatchingAt = DateTimeOffset.UtcNow;
                        messages.Add(message);
                        if (onMessage?.Invoke(message) == true)
                        {
                            shouldStop = true;
                        }
                    }

                    if (publishQos == 2 && publishPacketId != 0)
                    {
                        await CompletePendingInboundQoS2Async(cancellationToken);
                    }

                    if (shouldStop)
                    {
                        completed = true;
                        break;
                    }
                }
            }

            if (!completed && !string.IsNullOrWhiteSpace(stopReason))
            {
                onWaitStopped?.Invoke(stopReason);
            }

            return messages;
        }

        private static string FormatDuration(TimeSpan duration)
        {
            return duration.TotalMinutes >= 1
                ? $"{duration.TotalMinutes:0.#}m"
                : $"{duration.TotalSeconds:0.#}s";
        }

        private static bool TopicMatches(string filter, string topic)
        {
            if (string.Equals(filter, topic, StringComparison.Ordinal))
            {
                return true;
            }

            var filterParts = filter.Split('/');
            var topicParts = topic.Split('/');
            for (var i = 0; i < filterParts.Length; ++i)
            {
                var filterPart = filterParts[i];
                if (filterPart == "#")
                {
                    return i == filterParts.Length - 1;
                }

                if (i >= topicParts.Length)
                {
                    return false;
                }

                if (filterPart == "+")
                {
                    continue;
                }

                if (!string.Equals(filterPart, topicParts[i], StringComparison.Ordinal))
                {
                    return false;
                }
            }

            return topicParts.Length == filterParts.Length;
        }

        public void Dispose()
        {
            stream.Dispose();
            tcpClient.Dispose();
            writeLock.Dispose();
        }

        private async Task SendConnectAsync(DeviceEndpoint endpoint, CancellationToken cancellationToken)
        {
            var variable = new List<byte>();
            WriteUtf8(variable, "MQTT");
            variable.Add(4);

            byte flags = 0x02;
            if (!string.IsNullOrEmpty(endpoint.MqttUsername))
            {
                flags |= 0x80;
            }
            if (!string.IsNullOrEmpty(endpoint.MqttPassword))
            {
                flags |= 0x40;
            }
            variable.Add(flags);
            variable.Add(0);
            variable.Add(60);

            var payload = new List<byte>();
            WriteUtf8(payload, BuildConnectionClientId(endpoint));
            if (!string.IsNullOrEmpty(endpoint.MqttUsername))
            {
                WriteUtf8(payload, endpoint.MqttUsername);
            }
            if (!string.IsNullOrEmpty(endpoint.MqttPassword))
            {
                WriteUtf8(payload, endpoint.MqttPassword);
            }

            var packet = BuildPacket(0x10, variable.Concat(payload));
            await WritePacketAsync(packet, cancellationToken);
            var connAck = await ReadPacketAsync(cancellationToken);
            if (connAck.Type != 0x20 || connAck.Payload.Length < 2 || connAck.Payload[1] != 0)
            {
                throw new InvalidOperationException("MQTT broker rejected the connection");
            }
        }

        private ushort NextPacketId() => ++packetId == 0 ? ++packetId : packetId;

        private static int ClampQos(int qos) => Math.Clamp(qos, 0, 2);

        private static string BuildConnectionClientId(DeviceEndpoint endpoint)
        {
            var configured = endpoint.MqttClientId.Trim();
            var baseId = string.IsNullOrWhiteSpace(configured)
                ? "GatewayStudio"
                : configured;
            if (!baseId.StartsWith("GatewayStudio", StringComparison.OrdinalIgnoreCase))
            {
                return baseId;
            }

            var suffix = Guid.NewGuid().ToString("N")[..8];
            var processId = Environment.ProcessId.ToString("x", CultureInfo.InvariantCulture);
            var maxBaseLength = Math.Max(1, 128 - processId.Length - suffix.Length - 2);
            if (baseId.Length > maxBaseLength)
            {
                baseId = baseId[..maxBaseLength].TrimEnd('-', '_', '.');
            }

            return $"{baseId}-{processId}-{suffix}";
        }

        private static MqttMessage ParsePublish(MqttPacket packet, out ushort publishPacketId, out int publishQos)
        {
            publishPacketId = 0;
            publishQos = (packet.Type >> 1) & 0x03;
            if (packet.Payload.Length < 2)
            {
                return new MqttMessage("", "");
            }

            var topicLength = BinaryPrimitives.ReadUInt16BigEndian(packet.Payload.AsSpan(0, 2));
            if (packet.Payload.Length < 2 + topicLength)
            {
                return new MqttMessage("", "");
            }

            var topic = Encoding.UTF8.GetString(packet.Payload, 2, topicLength);
            var payloadOffset = 2 + topicLength;
            if (publishQos > 0)
            {
                if (packet.Payload.Length < payloadOffset + 2)
                {
                    return new MqttMessage(topic, "");
                }

                publishPacketId = BinaryPrimitives.ReadUInt16BigEndian(packet.Payload.AsSpan(payloadOffset, 2));
                payloadOffset += 2;
            }

            var payload = payloadOffset >= packet.Payload.Length
                ? ""
                : Encoding.UTF8.GetString(packet.Payload, payloadOffset, packet.Payload.Length - payloadOffset);
            return new MqttMessage(topic, payload);
        }

        private async Task<MqttPacket?> ReadPacketWithKeepAliveAsync(
            TimeSpan timeout,
            CancellationToken cancellationToken
        )
        {
            var deadline = DateTimeOffset.UtcNow + timeout;
            using var readCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            var readTask = ReadPacketAsync(readCts.Token);
            try
            {
                while (true)
                {
                    if (readTask.IsCompleted)
                    {
                        return await readTask;
                    }

                    var now = DateTimeOffset.UtcNow;
                    var remaining = deadline - now;
                    if (remaining <= TimeSpan.Zero)
                    {
                        readCts.Cancel();
                        return null;
                    }

                    var pingRemaining = MqttPingInterval - (now - lastOutboundAt);
                    if (pingRemaining <= TimeSpan.Zero)
                    {
                        await SendPingAsync(cancellationToken);
                        continue;
                    }

                    var wait = remaining < pingRemaining ? remaining : pingRemaining;
                    var completed = await Task.WhenAny(readTask, Task.Delay(wait, cancellationToken));
                    if (completed == readTask)
                    {
                        return await readTask;
                    }

                    cancellationToken.ThrowIfCancellationRequested();
                }
            }
            finally
            {
                if (!readTask.IsCompleted)
                {
                    _ = readTask.ContinueWith(
                        task => _ = task.Exception,
                        CancellationToken.None,
                        TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                        TaskScheduler.Default
                    );
                }
            }
        }

        private async Task SendPingAsync(CancellationToken cancellationToken)
        {
            await WritePacketAsync(new byte[] { 0xC0, 0x00 }, cancellationToken);
        }

        private async Task SendPubAckAsync(ushort publishPacketId, CancellationToken cancellationToken)
        {
            await WritePacketAsync(
                new[]
                {
                    (byte)0x40,
                    (byte)0x02,
                    (byte)(publishPacketId >> 8),
                    (byte)(publishPacketId & 0xFF)
                },
                cancellationToken
            );
        }

        private async Task SendPubRecAsync(ushort publishPacketId, CancellationToken cancellationToken)
        {
            await WritePacketAsync(BuildPacketIdPacket(0x50, publishPacketId), cancellationToken);
        }

        private async Task SendPubRelAsync(ushort publishPacketId, CancellationToken cancellationToken)
        {
            await WritePacketAsync(BuildPacketIdPacket(0x62, publishPacketId), cancellationToken);
        }

        private async Task SendPubCompAsync(ushort publishPacketId, CancellationToken cancellationToken)
        {
            await WritePacketAsync(BuildPacketIdPacket(0x70, publishPacketId), cancellationToken);
        }

        private async Task CompleteInboundQos2Async(ushort publishPacketId, CancellationToken cancellationToken)
        {
            while (true)
            {
                var packet = await ReadPacketAsync(cancellationToken);
                if (packet.Type == 0xD0)
                {
                    continue;
                }

                if (packet.Type == 0x62 && TryReadPacketId(packet, out var pubRelId))
                {
                    await SendPubCompAsync(pubRelId, cancellationToken);
                    pendingInboundQoS2PacketIds.Remove(pubRelId);
                    if (pubRelId == publishPacketId)
                    {
                        return;
                    }
                    continue;
                }

                if ((packet.Type & 0xF0) == 0x30)
                {
                    await AcknowledgeUnexpectedPublishAsync(packet, cancellationToken);
                    continue;
                }

                throw new InvalidOperationException("MQTT broker did not complete QoS2 inbound handshake");
            }
        }

        private async Task WaitForAckAsync(byte expectedType, ushort expectedPacketId, string label, CancellationToken cancellationToken)
        {
            while (true)
            {
                var packet = await ReadPacketAsync(cancellationToken);
                if (packet.Type == 0xD0)
                {
                    continue;
                }

                if (packet.Type == 0x62 && TryReadPacketId(packet, out var queuedPubRelId))
                {
                    await SendPubCompAsync(queuedPubRelId, cancellationToken);
                    pendingInboundQoS2PacketIds.Remove(queuedPubRelId);
                    continue;
                }

                if (packet.Type == expectedType && TryReadPacketId(packet, out var packetId) && packetId == expectedPacketId)
                {
                    return;
                }

                if ((packet.Type & 0xF0) == 0x30)
                {
                    await AcknowledgeUnexpectedPublishAsync(packet, cancellationToken);
                    continue;
                }

                throw new InvalidOperationException($"MQTT broker did not return {label}");
            }
        }

        private async Task AcknowledgeUnexpectedPublishAsync(MqttPacket packet, CancellationToken cancellationToken)
        {
            var message = ParsePublish(packet, out var publishPacketId, out var publishQos);
            if (publishQos == 1 && publishPacketId != 0)
            {
                await SendPubAckAsync(publishPacketId, cancellationToken);
            }
            else if (publishQos == 2 && publishPacketId != 0)
            {
                await SendPubRecAsync(publishPacketId, cancellationToken);
                QueueInboundQoS2(publishPacketId);
            }
            if (pendingMessages.Count >= maxPendingMessages)
                throw new InvalidDataException("MQTT diagnostic pending reply limit exceeded");
            pendingMessages.Enqueue(message);
        }

        private async Task<bool> DrainPendingMessagesAsync(
            IReadOnlyCollection<string> topics,
            List<MqttMessage> messages,
            Func<MqttMessage, bool>? onMessage,
            CancellationToken cancellationToken
        )
        {
            while (pendingMessages.Count > 0)
            {
                var message = pendingMessages.Dequeue();
                if (!topics.Any(topic => TopicMatches(topic, message.Topic)))
                {
                    continue;
                }

                messages.Add(message);
                if (onMessage?.Invoke(message) == true)
                {
                    await CompletePendingInboundQoS2Async(cancellationToken);
                    return true;
                }
            }

            return false;
        }

        private void QueueInboundQoS2(ushort publishPacketId)
        {
            if (!pendingInboundQoS2PacketIds.Contains(publishPacketId))
            {
                pendingInboundQoS2PacketIds.Add(publishPacketId);
            }
        }

        private async Task CompletePendingInboundQoS2Async(CancellationToken cancellationToken)
        {
            while (pendingInboundQoS2PacketIds.Count > 0)
            {
                var publishPacketId = pendingInboundQoS2PacketIds[0];
                await CompleteInboundQos2Async(publishPacketId, cancellationToken);
                pendingInboundQoS2PacketIds.Remove(publishPacketId);
            }
        }

        private static bool TryReadPacketId(MqttPacket packet, out ushort packetId)
        {
            packetId = 0;
            if (packet.Payload.Length < 2)
            {
                return false;
            }

            packetId = BinaryPrimitives.ReadUInt16BigEndian(packet.Payload.AsSpan(0, 2));
            return true;
        }

        private static byte[] BuildPacketIdPacket(byte type, ushort packetId) =>
        [
            type,
            0x02,
            (byte)(packetId >> 8),
            (byte)(packetId & 0xFF)
        ];

        private async Task WritePacketAsync(byte[] packet, CancellationToken cancellationToken)
        {
            await writeLock.WaitAsync(cancellationToken);
            try
            {
                await stream.WriteAsync(packet, cancellationToken);
                lastOutboundAt = DateTimeOffset.UtcNow;
            }
            finally
            {
                writeLock.Release();
            }
        }

        private async Task<MqttPacket> ReadPacketAsync(CancellationToken cancellationToken)
        {
            var typeBuffer = new byte[1];
            await ReadExactAsync(typeBuffer, cancellationToken);
            var remaining = await ReadRemainingLengthAsync(cancellationToken);
            if (remaining > maxInboundPacketBytes)
                throw new InvalidDataException("MQTT diagnostic frame exceeds bounded receive limit");
            var payload = new byte[remaining];
            if (remaining > 0)
            {
                await ReadExactAsync(payload, cancellationToken);
            }
            return new MqttPacket(typeBuffer[0], payload);
        }

        private async Task ReadExactAsync(byte[] buffer, CancellationToken cancellationToken)
        {
            var offset = 0;
            while (offset < buffer.Length)
            {
                var read = await stream.ReadAsync(buffer.AsMemory(offset, buffer.Length - offset), cancellationToken);
                if (read == 0)
                {
                    throw new IOException("MQTT connection closed");
                }
                offset += read;
            }
        }

        private async Task<int> ReadRemainingLengthAsync(CancellationToken cancellationToken)
        {
            var multiplier = 1;
            var value = 0;
            var buffer = new byte[1];
            for (var i = 0; i < 4; ++i)
            {
                await ReadExactAsync(buffer, cancellationToken);
                value += (buffer[0] & 127) * multiplier;
                if ((buffer[0] & 128) == 0)
                {
                    return value;
                }
                multiplier *= 128;
            }
            throw new IOException("invalid MQTT remaining length");
        }

        private static byte[] BuildPacket(byte type, IEnumerable<byte> payload)
        {
            var payloadBytes = payload.ToArray();
            var packet = new List<byte> { type };
            WriteRemainingLength(packet, payloadBytes.Length);
            packet.AddRange(payloadBytes);
            return packet.ToArray();
        }

        private static void WriteUtf8(ICollection<byte> output, string value)
        {
            var bytes = Encoding.UTF8.GetBytes(value);
            output.Add((byte)(bytes.Length >> 8));
            output.Add((byte)(bytes.Length & 0xFF));
            foreach (var item in bytes)
            {
                output.Add(item);
            }
        }

        private static void WriteRemainingLength(ICollection<byte> output, int length)
        {
            do
            {
                var encoded = length % 128;
                length /= 128;
                if (length > 0)
                {
                    encoded |= 128;
                }
                output.Add((byte)encoded);
            } while (length > 0);
        }

        private sealed record MqttPacket(byte Type, byte[] Payload);
    }
}
