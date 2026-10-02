using GatewayDesktop.Core.Entities;
using GatewayDesktop.Core.Enums;
using GatewayDesktop.Core.Interfaces;
using GatewayDesktop.Infrastructure.Transport.Real;
using GatewayDesktop.Services.Device;
using Moq;
using Legacy = GatewayDesktop.Core.Devices;

namespace GatewayDesktop.Tests.Services;

public sealed class DiagnosticServiceRequestTests
{
    [Fact]
    public async Task Caller_cancellation_keeps_remote_state_unknown_and_next_execution_gets_new_id()
    {
        const string machine = "GW_DIAG_TEST";
        var pending = new TaskCompletionSource<Legacy.TransportResult<Legacy.DiagResponse>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var sent = new List<Legacy.DiagRequest>();
        var legacy = new Mock<Legacy.IDeviceTransport>();
        legacy.Setup(x => x.GetHealthAsync(It.IsAny<Legacy.DeviceEndpoint>(), It.IsAny<CancellationToken>()))
            .ReturnsAsync(Legacy.TransportResult<Legacy.DeviceHealth>.Ok(new(true, "ok", 1, "fixture")));
        legacy.Setup(x => x.GetIdentityAsync(It.IsAny<Legacy.DeviceEndpoint>(), It.IsAny<CancellationToken>()))
            .ReturnsAsync(Legacy.TransportResult<Legacy.DeviceIdentity>.Ok(new(machine, "", "", "fixture", "", "")));
        legacy.Setup(x => x.ExecuteDiagAsync(It.IsAny<Legacy.DeviceEndpoint>(),
                It.IsAny<Legacy.DiagRequest>(), It.IsAny<CancellationToken>()))
            .Returns((Legacy.DeviceEndpoint _, Legacy.DiagRequest request, CancellationToken _) =>
            {
                sent.Add(request);
                return sent.Count == 1 ? pending.Task : Task.FromResult(
                    Legacy.TransportResult<Legacy.DiagResponse>.Ok(new(request.CmdId, machine,
                        request.Operator, request.Command, true, 0, "ok", "", request.RequestTs, 2,
                        "", "exit", 0, false, true, false, "terminal")));
            });
        using var adapter = new RealDeviceTransport(legacy.Object, ConnectionMode.RemoteMqtt);
        var connected = await adapter.ConnectAsync(new ConnectionProfile { MachineCode = machine, Mode = ConnectionMode.RemoteMqtt });
        Assert.True(connected.Success);
        var connection = new Mock<IConnectionService>();
        connection.Setup(x => x.GetTransport()).Returns(adapter);
        var service = new DiagnosticService(connection.Object);
        var command = new DiagnosticCommand { Name = "Uptime", Command = "uptime" };
        using var cancellation = new CancellationTokenSource();
        var first = service.ExecuteAsync(command, "unused", 99, cancellation.Token);
        cancellation.Cancel();
        pending.SetResult(Legacy.TransportResult<Legacy.DiagResponse>.Fail("transport consumed cancellation"));
        var unknown = await first;
        Assert.False(unknown.Success);
        Assert.Contains("远端是否仍在执行未知", unknown.Error);
        Assert.Equal(sent[0].CmdId, unknown.CmdId);
        Assert.Equal(machine, unknown.MachineCode);

        var second = await service.ExecuteAsync(command, "unused", 99);
        Assert.True(second.Success, second.Error);
        Assert.NotEqual(sent[0].CmdId, sent[1].CmdId);
        Assert.StartsWith("DIAG_", sent[0].CmdId);
        Assert.Equal("", sent[0].Service);
        Assert.Equal(100, sent[0].TailLines);
    }
}
