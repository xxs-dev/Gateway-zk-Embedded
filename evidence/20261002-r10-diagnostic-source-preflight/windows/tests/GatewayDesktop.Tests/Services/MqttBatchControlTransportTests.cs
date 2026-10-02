using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using GatewayDesktop.Core.Devices;
using GatewayDesktop.Core.Entities;
using GatewayDesktop.Core.Enums;
using GatewayDesktop.Core.Interfaces;
using GatewayDesktop.Infrastructure.Transport.Real;
using GatewayDesktop.Services.Device;
using GatewayDesktop.Transport;
using GatewayDesktop.Transport.Mqtt;
using Moq;

namespace GatewayDesktop.Tests.Services;

public sealed class MqttBatchControlTransportTests
{
    private const string Machine = "STAGE7_LOOPBACK";

    [Theory]
    [InlineData(true, true)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(false, false)]
    public async Task Diagnostic_transport_distinguishes_admitted_terminal_from_conflicting_attempt(
        bool originalCaller, bool conflictFirst)
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunDiagAsync(conflictFirst);
        var request = new DiagRequest("DIAG_FIXED", Machine, "operator-a", "systemctl_status",
            originalCaller ? "gateway-services.service" : "other.service", 100, 12345, "");

        var result = await new MqttDeviceTransport().ExecuteDiagAsync(broker.Endpoint, request, broker.Token);
        var published = await server;

        Assert.Equal("DIAG_FIXED", published["cmdId"]!.GetValue<string>());
        Assert.Equal(request.Service, published["service"]!.GetValue<string>());
        if (originalCaller)
        {
            Assert.True(result.Success, result.Message);
            Assert.Equal("exit", result.Value!.Reason);
            Assert.Equal("gateway-services.service", result.Value.Arg);
        }
        else
        {
            Assert.False(result.Success);
            Assert.Null(result.Value);
            Assert.Contains("cmdId_conflict", result.Message);
        }
    }

    [Fact]
    public async Task Diagnostic_transport_retry_reuses_one_logical_request_id()
    {
        using var broker = new LoopbackBroker();
        var request = new DiagRequest("DIAG_RETRY_FIXED", Machine, "operator-a", "systemctl_status",
            "gateway-services.service", 100, 12345, "");
        for (var attempt = 0; attempt < 2; attempt++)
        {
            var server = broker.RunDiagAsync(conflictFirst: false);
            var result = await new MqttDeviceTransport().ExecuteDiagAsync(broker.Endpoint, request, broker.Token);
            var published = await server;
            Assert.True(result.Success, result.Message);
            Assert.Equal(request.CmdId, published["cmdId"]!.GetValue<string>());
            Assert.Equal(request.RequestTs, published["requestTs"]!.GetValue<long>());
        }
    }

    [Fact]
    public async Task Diagnostic_oversized_remaining_length_is_rejected_before_payload_allocation()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunDiagOversizedHeaderAsync();
        var request = new DiagRequest("DIAG_LIMIT", Machine, "operator-a", "uptime", "", 100, 12345, "");
        var result = await new MqttDeviceTransport().ExecuteDiagAsync(broker.Endpoint, request, broker.Token);
        await server;
        Assert.False(result.Success);
        Assert.Contains("bounded receive limit", result.Message);
    }

    [Fact]
    public async Task Diagnostic_pre_ack_pending_reply_flood_is_bounded()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunDiagPendingFloodAsync();
        var request = new DiagRequest("DIAG_FLOOD", Machine, "operator-a", "uptime", "", 100, 12345, "");
        var result = await new MqttDeviceTransport().ExecuteDiagAsync(broker.Endpoint, request, broker.Token);
        await server;
        Assert.False(result.Success);
        Assert.Contains("pending reply limit", result.Message);
    }

    [Fact]
    public async Task Signed_ota_loopback_publish_contains_signature_and_original_job_identity()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunOtaAsync();
        var request = OtaRequest();

        var result = await new MqttDeviceTransport().StartOtaJobAsync(broker.Endpoint, request, broker.Token);
        var payload = await server;

        Assert.True(result.Success, result.Message);
        Assert.Equal(request.JobId, result.Value!.JobId);
        Assert.Equal(request.JobId, payload["jobId"]!.GetValue<string>());
        Assert.Equal(Machine, payload["machineCode"]!.GetValue<string>());
        Assert.Equal(request.Signature, payload["signature"]!.GetValue<string>());
        Assert.Equal(request.ArtifactUrl, payload["artifactUrl"]!.GetValue<string>());
    }

    [Fact]
    public async Task Wrong_machine_status_cannot_confirm_signed_ota_job()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunOtaAsync(statusMachine: "OTHER");

        var result = await new MqttDeviceTransport().StartOtaJobAsync(broker.Endpoint, OtaRequest(), broker.Token);
        await server;

        Assert.False(result.Success);
        Assert.Null(result.Value);
    }

    [Theory]
    [InlineData("missing-signature")]
    [InlineData("http-url")]
    [InlineData("empty-endpoint")]
    public async Task Unsafe_ota_input_rejects_before_mqtt_connect(string variant)
    {
        using var broker = new LoopbackBroker();
        var endpoint = variant == "empty-endpoint" ? broker.Endpoint with { MachineCode = "" } : broker.Endpoint;
        var request = OtaRequest();
        if (variant == "missing-signature") request = request with { Signature = "" };
        if (variant == "http-url") request = request with { ArtifactUrl = "http://artifacts.example.test/a" };

        var result = await new MqttDeviceTransport().StartOtaJobAsync(endpoint, request, broker.Token);

        Assert.False(result.Success);
        Assert.False(broker.HasPendingConnection);
    }

    private static OtaJobRequest OtaRequest() => new(
        "OTA_REVIEW_1", Machine, "config", "https://artifacts.example.test/config.tar",
        "1.0.0", new string('a', 64), 7, "upgrade", "", Convert.ToBase64String(new byte[256]));

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Io_failure_after_collected_reply_preserves_only_confirmed_results(bool success)
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
        {
            await broker.ReplyAsync(stream, Reply(commands[0], success));
            // PUBCOMP proves the callback ran before this connection is closed.
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(
            broker.Endpoint, Request(), broker.Token);
        await server;

        Assert.False(result.Success);
        Assert.Contains("MQTT connection closed", result.Message);
        AssertPartial(result, success);
        Assert.Equal(2, broker.Published.Count);
        Assert.False(broker.HasPendingConnection);
    }

    [Fact]
    public async Task Ordinary_read_cancellation_keeps_existing_timeout_marker_behavior()
    {
        using var broker = new LoopbackBroker();
        using var cancellation = new CancellationTokenSource();
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
        {
            await broker.ReplyAsync(stream, Reply(commands[0], true));
            cancellation.Cancel();
            await broker.ExpectClientCloseAsync(stream);
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(
            broker.Endpoint, Request(), cancellation.Token);
        await server;

        Assert.True(result.Success);
        Assert.NotNull(result.Value);
        Assert.True(result.Value.Results[0].Success);
        Assert.Equal("no matching MQTT command reply received", result.Value.Results[1].Message);
        Assert.False(result.Value.Results[1].Success);
    }

    [Theory]
    [InlineData("cancel")]
    [InlineData("disconnect")]
    public async Task Final_qos2_handshake_failure_retains_already_collected_reply(string failure)
    {
        using var broker = new LoopbackBroker();
        using var cancellation = new CancellationTokenSource();
        var server = broker.RunBatchAsync(1, async (stream, commands) =>
        {
            await broker.ReplyAsync(stream, Reply(commands[0], true), completeHandshake: false);
            if (failure == "cancel")
            {
                cancellation.Cancel();
                await broker.ExpectClientCloseAsync(stream);
            }
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(
            broker.Endpoint, Request() with { Commands = [Request().Commands[0]] }, cancellation.Token);
        await server;

        Assert.False(result.Success);
        AssertPartial(result, true, total: 1);
    }

    [Theory]
    [InlineData("machineCode")]
    [InlineData("meterCode")]
    [InlineData("pointCode")]
    [InlineData("index")]
    public async Task Wrong_identity_does_not_consume_pending_before_valid_reply(string field)
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
        {
            var wrong = Reply(commands[0], false);
            wrong[field] = field == "index" ? JsonValue.Create(999) : JsonValue.Create("foreign");
            await broker.ReplyAsync(stream, wrong);
            await broker.ReplyAsync(stream, Reply(commands[0], true));
            await broker.ReplyAsync(stream, Reply(commands[1], false));
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(
            broker.Endpoint, Request(), broker.Token);
        await server;

        Assert.True(result.Success);
        Assert.NotNull(result.Value);
        Assert.Equal(new[] { "first", "second" }, result.Value.Results.Select(item => item.CmdId));
        Assert.True(result.Value.Results[0].Success);
        Assert.False(result.Value.Results[1].Success);
        Assert.Equal(1, result.Value.Accepted);
        Assert.Equal(1, result.Value.Failed);
    }

    [Fact]
    public async Task Legacy_identity_duplicate_and_foreign_replies_preserve_input_order()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
        {
            var foreign = Reply(commands[0], true);
            foreign["cmdId"] = "foreign";
            await broker.ReplyAsync(stream, foreign);
            var legacy = Reply(commands[1], true);
            foreach (var key in new[] { "machineCode", "meterCode", "pointCode", "index" })
                legacy.Remove(key);
            await broker.ReplyAsync(stream, legacy);
            await broker.ReplyAsync(stream, legacy);
            await broker.ReplyAsync(stream, Reply(commands[0], false));
        });

        var request = Request() with
        {
            Commands = Request().Commands.Select(item => item with { CmdId = "" }).ToArray()
        };
        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, request, broker.Token);
        await server;

        Assert.True(result.Success);
        Assert.NotNull(result.Value);
        Assert.Equal(new[] { "batch_001", "batch_002" }, result.Value.Results.Select(item => item.CmdId));
        Assert.Equal(2, broker.Published.Select(item => item["cmdId"]!.GetValue<string>()).Distinct().Count());
        Assert.False(result.Value.Results[0].Success);
        Assert.True(result.Value.Results[1].Success);
        Assert.Equal("M2", result.Value.Results[1].MeterCode);
        Assert.Equal(22u, result.Value.Results[1].Index);
    }

    [Theory]
    [InlineData("first", "first")]
    [InlineData("", "batch_001")]
    public async Task Duplicate_normalized_command_ids_are_rejected_before_connect(string firstId, string secondId)
    {
        using var broker = new LoopbackBroker();
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        var request = Request();
        request = request with
        {
            Commands = [request.Commands[0] with { CmdId = firstId }, request.Commands[1] with { CmdId = secondId }]
        };

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, request, cancellation.Token);

        Assert.False(result.Success);
        Assert.Null(result.Value);
        Assert.False(broker.HasPendingConnection);
        Assert.Contains("unique", result.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task No_collected_reply_does_not_fabricate_outcomes(bool failBeforePublish)
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunBatchAsync(2, (_, _) => Task.CompletedTask, failBeforePublish);
        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, Request(), broker.Token);
        await server;

        Assert.False(result.Success);
        Assert.Null(result.Value);
        Assert.Equal(failBeforePublish ? 0 : 2, broker.Published.Count);
        Assert.False(broker.HasPendingConnection);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Batch_service_with_real_mqtt_keeps_confirmed_and_unknown_separate(bool success)
    {
        using var broker = new LoopbackBroker();
        using var adapter = new RealDeviceTransport(new MqttDeviceTransport(), ConnectionMode.RemoteMqtt);
        var health = broker.AcceptHealthAsync();
        var connected = await adapter.ConnectAsync(new ConnectionProfile
        {
            MachineCode = Machine, Mode = ConnectionMode.RemoteMqtt,
            Mqtt = new MqttConfig { Broker = broker.Endpoint.MqttBroker }
        }, broker.Token);
        await health;
        Assert.True(connected.Success, connected.Message);
        var connection = new Mock<IConnectionService>(MockBehavior.Strict);
        connection.Setup(item => item.GetTransport()).Returns(adapter);
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
            await broker.ReplyAsync(stream, Reply(commands[0], success)));

        var result = await new BatchControlService(connection.Object).ExecuteAsync(
            [new BatchControlItem(11, "M1", "set", 1), new BatchControlItem(22, "M2", "set", 2)], false, broker.Token);
        await server;

        Assert.False(result.Success);
        Assert.Equal(success ? 1 : 0, result.Accepted);
        Assert.Equal(success ? 0 : 1, result.Failed);
        Assert.Equal(1, result.UnknownCount);
        Assert.True(result.Items[0].OutcomeKnown);
        Assert.Equal(success, result.Items[0].Success);
        Assert.False(result.Items[1].OutcomeKnown);
        Assert.Equal(2, broker.Published.Count);
        Assert.False(broker.HasPendingConnection);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Partial_exception_results_are_ordered_and_exclude_untrusted_replies(bool malformed)
    {
        using var broker = new LoopbackBroker();
        var request = Request() with
        {
            Commands = [.. Request().Commands, new ControlCommandItem("third", 33, "M3", "set", 3, "test", 1)]
        };
        var server = broker.RunBatchAsync(3, async (stream, commands) =>
        {
            var wrong = Reply(commands[0], true);
            wrong["machineCode"] = "foreign";
            await broker.ReplyAsync(stream, wrong);
            await broker.ReplyAsync(stream, Reply(commands[2], true));
            await broker.ReplyAsync(stream, Reply(commands[1], false));
            if (malformed) await broker.MalformedReplyAsync(stream);
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, request, broker.Token);
        await server;

        Assert.False(result.Success);
        Assert.NotNull(result.Value);
        Assert.Equal(3, result.Value.Total);
        Assert.Equal(1, result.Value.Accepted);
        Assert.Equal(1, result.Value.Failed);
        Assert.Equal(new[] { "second", "third" }, result.Value.Results.Select(item => item.CmdId));
        Assert.False(result.Value.Results[0].Success);
        Assert.True(result.Value.Results[1].Success);
    }

    [Fact]
    public async Task Actual_read_timeout_preserves_existing_wire_result_and_timeout_marker()
    {
        using var broker = new LoopbackBroker();
        var server = broker.RunBatchAsync(2, async (stream, commands) =>
        {
            await broker.ReplyAsync(stream, Reply(commands[0], true));
            await broker.ExpectClientCloseAsync(stream);
        });

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, Request(), broker.Token);
        await server;

        Assert.False(broker.Token.IsCancellationRequested);
        Assert.True(result.Success);
        Assert.NotNull(result.Value);
        Assert.Equal(2, result.Value.Results.Count);
        Assert.True(result.Value.Results[0].Success);
        Assert.False(result.Value.Results[1].Success);
        Assert.Equal("no matching MQTT command reply received", result.Value.Results[1].Message);
    }

    [Fact]
    public async Task Already_cancelled_request_never_connects_or_returns_execution_evidence()
    {
        using var broker = new LoopbackBroker();
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(
            broker.Endpoint, Request(), cancellation.Token);

        Assert.False(result.Success);
        Assert.Null(result.Value);
        Assert.False(broker.HasPendingConnection);
    }

    [Fact]
    public async Task High_priority_command_is_rejected_before_mqtt_connect()
    {
        using var broker = new LoopbackBroker();
        var request = Request() with
        {
            Commands = [Request().Commands[0] with { HighPriority = true }]
        };

        var result = await new MqttDeviceTransport().ExecuteBatchControlAsync(broker.Endpoint, request, broker.Token);

        Assert.False(result.Success);
        Assert.Contains("高优先级", result.Message);
        Assert.False(broker.HasPendingConnection);
    }

    private static void AssertPartial(TransportResult<BatchControlResponse> result, bool success, int total = 2)
    {
        Assert.NotNull(result.Value);
        Assert.Equal("batch", result.Value.RequestId);
        Assert.Equal(Machine, result.Value.MachineCode);
        Assert.False(result.Value.Success);
        Assert.Equal(total, result.Value.Total);
        Assert.Equal(success ? 1 : 0, result.Value.Accepted);
        Assert.Equal(success ? 0 : 1, result.Value.Failed);
        var item = Assert.Single(result.Value.Results);
        Assert.Equal("first", item.CmdId);
        Assert.Equal(success, item.Success);
        Assert.Equal("confirmed", item.Message);
        Assert.Equal("M1", item.MeterCode);
        Assert.Equal("set", item.PointCode);
        Assert.Equal(11u, item.Index);
        Assert.Equal(1, item.Value);
    }

    private static BatchControlRequest Request() => new("batch", Machine, "test", "",
    [
        new ControlCommandItem("first", 11, "M1", "set", 1, "test", 1),
        new ControlCommandItem("second", 22, "M2", "set", 2, "test", 1)
    ]);

    private static JsonObject Reply(JsonObject command, bool success)
    {
        var reply = command.DeepClone().AsObject();
        reply["success"] = success;
        reply["message"] = "confirmed";
        return reply;
    }

    // A bounded MQTT 3.1.1 peer, bound exclusively to an ephemeral loopback port.
    private sealed class LoopbackBroker : IDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _deadline = new(TimeSpan.FromSeconds(20));
        public List<JsonObject> Published { get; } = [];
        public CancellationToken Token => _deadline.Token;
        public bool HasPendingConnection => _listener.Pending();
        public DeviceEndpoint Endpoint { get; }

        public LoopbackBroker()
        {
            _listener.Start();
            Endpoint = new DeviceEndpoint
            {
                Mode = DeviceAccessMode.RemoteMqtt, MachineCode = Machine,
                MqttBroker = $"tcp://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}"
            };
        }

        public async Task<JsonObject> RunDiagAsync(bool conflictFirst)
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            var stream = client.GetStream();
            await ConnectAsync(stream);
            var subscribe = await ReadAsync(stream);
            Assert.Equal(0x82, subscribe.Type);
            Assert.Equal($"edge/system/diag/reply/{Machine}", ReadString(subscribe.Body, 2));
            await WriteAsync(stream, 0x90, [subscribe.Body[0], subscribe.Body[1], 2]);
            var publish = await ReadAsync(stream);
            Assert.Equal(0x34, publish.Type);
            Assert.Equal($"edge/system/diag/request/{Machine}", ReadString(publish.Body, 0));
            var offset = 2 + BinaryPrimitives.ReadUInt16BigEndian(publish.Body);
            var packetId = publish.Body[offset..(offset + 2)];
            var request = JsonNode.Parse(Encoding.UTF8.GetString(publish.Body[(offset + 2)..]))!.AsObject();
            await WriteAsync(stream, 0x50, packetId);
            var release = await ReadAsync(stream);
            Assert.Equal(0x62, release.Type);
            await WriteAsync(stream, 0x70, packetId);

            var id = request["cmdId"]!.GetValue<string>();
            var terminal = JsonSerializer.Serialize(new
            {
                replyKind = "terminal", cmdId = id, machineCode = Machine, @operator = "operator-a",
                command = "systemctl_status", arg = "gateway-services.service", success = false,
                exitCode = 1, reason = "exit", signal = 0, timedOut = false,
                cleanupConfirmed = true, truncated = false, stdout = "failed", stderr = "",
                requestTs = 12345, ts = 12346
            });
            var rejection = JsonSerializer.Serialize(new
            {
                replyKind = "admission_rejection", attemptedCmdId = id, machineCode = Machine,
                command = "systemctl_status", arg = "other.service", @operator = "operator-a",
                reason = "cmdId_conflict", ts = 12346
            });
            foreach (var payload in conflictFirst ? new[] { rejection, terminal } : new[] { terminal, rejection })
            {
                var topic = Encoding.UTF8.GetBytes($"edge/system/diag/reply/{Machine}");
                await WriteAsync(stream, 0x30,
                    [(byte)(topic.Length >> 8), (byte)topic.Length, .. topic, .. Encoding.UTF8.GetBytes(payload)]);
            }
            return request;
        }

        public async Task RunDiagOversizedHeaderAsync()
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            var stream = client.GetStream();
            await ConnectAsync(stream);
            var subscribe = await ReadAsync(stream);
            Assert.Equal(0x82, subscribe.Type);
            await WriteAsync(stream, 0x90, [subscribe.Body[0], subscribe.Body[1], 2]);
            var publish = await ReadAsync(stream);
            Assert.Equal(0x34, publish.Type);
            var offset = 2 + BinaryPrimitives.ReadUInt16BigEndian(publish.Body);
            var packetId = publish.Body[offset..(offset + 2)];
            await WriteAsync(stream, 0x50, packetId);
            Assert.Equal(0x62, (await ReadAsync(stream)).Type);
            await WriteAsync(stream, 0x70, packetId);
            var remaining = DiagReplyAdmission.MaxMqttJsonChars + 1025;
            var header = new List<byte> { 0x30 };
            do
            {
                var digit = (byte)(remaining % 128);
                remaining /= 128;
                if (remaining > 0) digit |= 0x80;
                header.Add(digit);
            } while (remaining > 0);
            await stream.WriteAsync(header.ToArray(), Token);
        }

        public async Task RunDiagPendingFloodAsync()
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            var stream = client.GetStream();
            await ConnectAsync(stream);
            var subscribe = await ReadAsync(stream);
            Assert.Equal(0x82, subscribe.Type);
            await WriteAsync(stream, 0x90, [subscribe.Body[0], subscribe.Body[1], 2]);
            Assert.Equal(0x34, (await ReadAsync(stream)).Type);
            var topic = Encoding.UTF8.GetBytes($"edge/system/diag/reply/{Machine}");
            byte[] body = [(byte)(topic.Length >> 8), (byte)topic.Length, .. topic, (byte)'{', (byte)'}'];
            Assert.InRange(body.Length, 1, 127);
            byte[] packet = [0x30, (byte)body.Length, .. body];
            var flood = Enumerable.Range(0, 9).SelectMany(_ => packet).ToArray();
            await stream.WriteAsync(flood, Token);
        }

        public async Task AcceptHealthAsync()
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            await ConnectAsync(client.GetStream());
        }

        public async Task<JsonObject> RunOtaAsync(string? statusMachine = null)
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            var stream = client.GetStream();
            await ConnectAsync(stream);
            foreach (var topic in new[] { $"edge/ota/reply/{Machine}", $"edge/ota/status/{Machine}" })
            {
                var subscribe = await ReadAsync(stream);
                Assert.Equal(0x82, subscribe.Type);
                Assert.Equal(topic, ReadString(subscribe.Body, 2));
                await WriteAsync(stream, 0x90, [subscribe.Body[0], subscribe.Body[1], 2]);
            }

            var publish = await ReadAsync(stream);
            Assert.Equal(0x34, publish.Type);
            Assert.Equal($"edge/ota/request/{Machine}", ReadString(publish.Body, 0));
            var offset = 2 + BinaryPrimitives.ReadUInt16BigEndian(publish.Body);
            var id = publish.Body[offset..(offset + 2)];
            var payload = JsonNode.Parse(Encoding.UTF8.GetString(publish.Body[(offset + 2)..]))!.AsObject();
            await WriteAsync(stream, 0x50, id);
            var release = await ReadAsync(stream);
            Assert.Equal(0x62, release.Type);
            Assert.Equal(id, release.Body);
            await WriteAsync(stream, 0x70, id);

            var replyTopic = statusMachine is null ? $"edge/ota/reply/{Machine}" : $"edge/ota/status/{Machine}";
            var reply = statusMachine is null
                ? """{"accepted":true,"jobId":"OTA_REVIEW_1","message":"accepted"}"""
                : $"{{\"jobId\":\"OTA_REVIEW_1\",\"machineCode\":\"{statusMachine}\",\"stage\":\"applying\",\"progress\":100,\"message\":\"waiting\"}}";
            var topicBytes = Encoding.UTF8.GetBytes(replyTopic);
            await WriteAsync(stream, 0x30,
                [(byte)(topicBytes.Length >> 8), (byte)topicBytes.Length, .. topicBytes, .. Encoding.UTF8.GetBytes(reply)]);
            return payload;
        }

        public async Task RunBatchAsync(int count, Func<NetworkStream, List<JsonObject>, Task> replies,
            bool failBeforePublish = false)
        {
            using var client = await _listener.AcceptTcpClientAsync(Token);
            var stream = client.GetStream();
            await ConnectAsync(stream);
            var subscribe = await ReadAsync(stream);
            Assert.Equal(0x82, subscribe.Type);
            Assert.Equal($"edge/command/reply/{Machine}", ReadString(subscribe.Body, 2));
            Assert.Equal(2, subscribe.Body[^1]);
            if (failBeforePublish) return;
            await WriteAsync(stream, 0x90, [subscribe.Body[0], subscribe.Body[1], 2]);
            for (var i = 0; i < count; i++)
            {
                var publish = await ReadAsync(stream);
                Assert.Equal(0x34, publish.Type);
                Assert.Equal($"edge/command/request/{Machine}", ReadString(publish.Body, 0));
                var offset = 2 + BinaryPrimitives.ReadUInt16BigEndian(publish.Body);
                var id = publish.Body[offset..(offset + 2)];
                var command = JsonNode.Parse(Encoding.UTF8.GetString(publish.Body[(offset + 2)..]))!.AsObject();
                Assert.Equal(Machine, command["machineCode"]!.GetValue<string>());
                Assert.False(command["highPriority"]!.GetValue<bool>());
                Published.Add(command);
                await WriteAsync(stream, 0x50, id);
                var release = await ReadAsync(stream);
                Assert.Equal(0x62, release.Type);
                Assert.Equal(id, release.Body);
                await WriteAsync(stream, 0x70, id);
            }
            await replies(stream, Published);
        }

        public async Task ReplyAsync(NetworkStream stream, JsonObject reply, bool completeHandshake = true)
        {
            var topic = Encoding.UTF8.GetBytes($"edge/command/reply/{Machine}");
            byte[] body = [(byte)(topic.Length >> 8), (byte)topic.Length, .. topic, 0, 91,
                .. Encoding.UTF8.GetBytes(reply.ToJsonString())];
            await WriteAsync(stream, 0x34, body);
            var received = await ReadAsync(stream);
            Assert.Equal(0x50, received.Type);
            Assert.Equal(new byte[] { 0, 91 }, received.Body);
            if (!completeHandshake) return;
            await WriteAsync(stream, 0x62, [0, 91]);
            var complete = await ReadAsync(stream);
            Assert.Equal(0x70, complete.Type);
            Assert.Equal(new byte[] { 0, 91 }, complete.Body);
        }

        public async Task ExpectClientCloseAsync(NetworkStream stream)
        {
            Assert.Equal(0, await stream.ReadAsync(new byte[1], Token));
        }

        public Task MalformedReplyAsync(NetworkStream stream)
        {
            var topic = Encoding.UTF8.GetBytes($"edge/command/reply/{Machine}");
            return WriteAsync(stream, 0x30,
                [(byte)(topic.Length >> 8), (byte)topic.Length, .. topic, (byte)'{']);
        }

        private async Task ConnectAsync(NetworkStream stream)
        {
            Assert.Equal(0x10, (await ReadAsync(stream)).Type);
            await WriteAsync(stream, 0x20, [0, 0]);
        }

        private async Task<(byte Type, byte[] Body)> ReadAsync(NetworkStream stream)
        {
            var header = new byte[1];
            await stream.ReadExactlyAsync(header, Token);
            var type = header[0];
            var length = 0;
            var multiplier = 1;
            for (var i = 0; i < 4; i++)
            {
                await stream.ReadExactlyAsync(header, Token);
                length += (header[0] & 127) * multiplier;
                if ((header[0] & 128) == 0)
                {
                    Assert.InRange(length, 0, 65536);
                    var body = new byte[length];
                    await stream.ReadExactlyAsync(body, Token);
                    return (type, body);
                }
                multiplier *= 128;
            }
            throw new InvalidDataException("Invalid MQTT length in test peer");
        }

        private async Task WriteAsync(NetworkStream stream, byte type, byte[] body)
        {
            var packet = new List<byte> { type };
            var remaining = body.Length;
            do
            {
                var next = (byte)(remaining % 128);
                remaining /= 128;
                packet.Add((byte)(next | (remaining > 0 ? 128 : 0)));
            } while (remaining > 0);
            packet.AddRange(body);
            await stream.WriteAsync(packet.ToArray(), Token);
        }

        private static string ReadString(byte[] body, int offset)
            => Encoding.UTF8.GetString(body, offset + 2,
                BinaryPrimitives.ReadUInt16BigEndian(body.AsSpan(offset, 2)));

        public void Dispose()
        {
            _deadline.Cancel();
            _listener.Stop();
            _deadline.Dispose();
        }
    }
}
