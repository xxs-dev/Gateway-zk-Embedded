using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using GatewayDesktop.Core.Devices;
using GatewayDesktop.Transport;
using GatewayDesktop.Transport.Direct;

namespace GatewayDesktop.Tests.Services;

public sealed class DiagnosticReplyContractTests
{
    private static readonly DiagRequest Original = new("DIAG_ONE", "GW_TEST", "operator-a",
        "systemctl_status", "gateway-services.service", 100, 12345, "");
    private static readonly DiagRequest Conflict = Original with { Service = "other.service" };

    private const string Terminal = """
        {"replyKind":"terminal","cmdId":"DIAG_ONE","machineCode":"GW_TEST","operator":"operator-a",
         "command":"systemctl_status","arg":"gateway-services.service","success":false,"exitCode":1,
         "reason":"exit","signal":0,"timedOut":false,"cleanupConfirmed":true,"truncated":false,
         "stdout":"failed","stderr":"","requestTs":12345,"ts":12346}
        """;
    private const string Rejection = """
        {"replyKind":"admission_rejection","attemptedCmdId":"DIAG_ONE","machineCode":"GW_TEST",
         "command":"systemctl_status","arg":"other.service","operator":"operator-a",
         "reason":"cmdId_conflict","ts":12346}
        """;

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Conflict_and_terminal_order_never_transfers_original_result(bool conflictFirst)
    {
        var ordered = conflictFirst ? new[] { Rejection, Terminal } : new[] { Terminal, Rejection };
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(ordered.First(x => x == Rejection), Original, "GW_TEST", out _));
        Assert.Equal(DiagReplyKind.AdmissionRejected,
            DiagReplyAdmission.Match(Rejection, Conflict, "GW_TEST", out _));
        var matches = ordered.Select(payload => DiagReplyAdmission.Match(payload, Original, "GW_TEST", out _)).ToArray();
        Assert.Single(matches, kind => kind == DiagReplyKind.Terminal);
        Assert.Equal(DiagReplyKind.Terminal,
            DiagReplyAdmission.Match(Terminal, Original, "GW_TEST", out var result));
        Assert.Equal("exit", result!.Reason);
        Assert.True(result.CleanupConfirmed);
        Assert.False(result.Truncated);
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(Terminal, Conflict, "GW_TEST", out _));
    }

    [Fact]
    public void Legacy_conflict_and_malformed_classification_cannot_be_terminal()
    {
        var legacyConflict = Terminal.Replace("\"reason\":\"exit\"", "\"reason\":\"cmdId_conflict\"");
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(legacyConflict, Original, "GW_TEST", out _));
        var malformed = Terminal.Replace("\"signal\":0", "\"signal\":\"15\"");
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(malformed, Original, "GW_TEST", out _));
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(Terminal, Original, "OTHER", out _));
        var legacy = """
            {"cmdId":"DIAG_LEGACY","machineCode":"GW_TEST","operator":"operator-a",
             "command":"uptime","success":true,"exitCode":0,"stdout":"ok","stderr":"",
             "requestTs":12345,"ts":12346}
            """;
        var request = Original with { CmdId = "DIAG_LEGACY", Command = "uptime", Service = "" };
        Assert.Equal(DiagReplyKind.Terminal,
            DiagReplyAdmission.Match(legacy, request, "GW_TEST", out var result));
        Assert.Null(result!.Reason);
        Assert.Null(result.CleanupConfirmed);
        Assert.Null(result.Truncated);
    }

    [Theory]
    [InlineData("\"success\":false", "\"success\":true")]
    [InlineData("\"success\":false", "\"success\":\"false\"")]
    [InlineData("\"exitCode\":1", "\"exitCode\":\"1\"")]
    [InlineData("\"signal\":0", "\"signal\":-1")]
    public void Contradictory_or_untyped_terminal_is_ignored(string before, string after)
    {
        Assert.Equal(DiagReplyKind.Ignore,
            DiagReplyAdmission.Match(Terminal.Replace(before, after), Original, "GW_TEST", out _));
    }

    [Fact]
    public async Task Direct_non2xx_is_not_an_execution_reply_and_success_requires_binding()
    {
        var endpoint = new DeviceEndpoint { Mode = DeviceAccessMode.LocalDirect, MachineCode = "GW_TEST", DirectBaseAddress = new Uri("http://127.0.0.1:18181") };
        using var rejectedClient = new HttpClient(new FixedHandler(HttpStatusCode.Conflict,
            """{"success":false,"reason":"cmdId_conflict"}"""));
        var rejected = await new DirectDeviceTransport(rejectedClient).ExecuteDiagAsync(endpoint, Original, CancellationToken.None);
        Assert.False(rejected.Success);
        Assert.Contains("HTTP 409: cmdId_conflict", rejected.Message);
        Assert.Null(rejected.Value);

        using var mismatchedClient = new HttpClient(new FixedHandler(HttpStatusCode.OK,
            Terminal.Replace("gateway-services.service", "other.service")));
        var mismatched = await new DirectDeviceTransport(mismatchedClient).ExecuteDiagAsync(endpoint, Original, CancellationToken.None);
        Assert.False(mismatched.Success);
        Assert.Null(mismatched.Value);

        using var arrayClient = new HttpClient(new FixedHandler(HttpStatusCode.Conflict, "[]"));
        var array = await new DirectDeviceTransport(arrayClient).ExecuteDiagAsync(endpoint, Original, CancellationToken.None);
        Assert.False(array.Success);
        Assert.Contains("HTTP 409", array.Message);
    }

    [Fact]
    public async Task Escaped_output_at_approved_direct_limit_remains_bounded_and_parseable()
    {
        var endpoint = new DeviceEndpoint { Mode = DeviceAccessMode.LocalDirect, MachineCode = "GW_TEST",
            DirectBaseAddress = new Uri("http://127.0.0.1:18181") };
        var output = new string('\u0001', 64 * 1024);
        var payload = JsonSerializer.Serialize(new
        {
            replyKind = "terminal", cmdId = Original.CmdId, machineCode = "GW_TEST", @operator = Original.Operator,
            command = Original.Command, arg = Original.Service, success = true, exitCode = 0,
            stdout = output, stderr = "", requestTs = Original.RequestTs, ts = 12346
        });
        Assert.InRange(Encoding.UTF8.GetByteCount(payload), 128 * 1024, DiagReplyAdmission.MaxDirectJsonBytes);
        using var client = new HttpClient(new FixedHandler(HttpStatusCode.OK, payload));
        var result = await new DirectDeviceTransport(client).ExecuteDiagAsync(endpoint, Original, CancellationToken.None);
        Assert.True(result.Success, result.Message);
        Assert.Equal(output, result.Value!.Stdout);
    }

    [Fact]
    public async Task Direct_transport_propagates_caller_cancellation_instead_of_reporting_remote_cleanup()
    {
        var endpoint = new DeviceEndpoint { Mode = DeviceAccessMode.LocalDirect, MachineCode = "GW_TEST",
            DirectBaseAddress = new Uri("http://127.0.0.1:18181") };
        using var client = new HttpClient(new CancelHandler());
        using var cancel = new CancellationTokenSource();
        var pending = new DirectDeviceTransport(client).ExecuteDiagAsync(endpoint, Original, cancel.Token);
        cancel.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
    }

    private sealed class FixedHandler(HttpStatusCode status, string body) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(new HttpResponseMessage(status) { Content = new StringContent(body, Encoding.UTF8, "application/json") });
    }

    private sealed class CancelHandler : HttpMessageHandler
    {
        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("unreachable");
        }
    }
}
