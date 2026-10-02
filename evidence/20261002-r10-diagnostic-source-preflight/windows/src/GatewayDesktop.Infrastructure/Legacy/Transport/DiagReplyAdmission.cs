using System.Text.Json;
using GatewayDesktop.Core.Devices;

namespace GatewayDesktop.Transport;

internal enum DiagReplyKind { Ignore, Terminal, AdmissionRejected }

internal static class DiagReplyAdmission
{
    internal const int MaxMqttJsonChars = 6 * 1024 * 1024 + 4096;
    internal const int MaxDirectJsonBytes = 6 * 64 * 1024 + 4096;
    internal static string ExpectedArg(DiagRequest request) => request.Command switch
    {
        "journal_tail" => Math.Clamp(request.TailLines, 1, 500).ToString(),
        "systemctl_status" => request.Service,
        _ => string.Empty
    };

    internal static DiagReplyKind Match(string payload, DiagRequest request, string machineCode,
        out DiagResponse? terminal)
    {
        terminal = null;
        if (payload.Length > MaxMqttJsonChars) return DiagReplyKind.Ignore;
        try
        {
            using var document = JsonDocument.Parse(payload);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                Text(root, "machineCode") != machineCode) return DiagReplyKind.Ignore;
            var kind = Text(root, "replyKind");
            var expectedArg = ExpectedArg(request);
            if (kind == "admission_rejection")
            {
                return Text(root, "reason") == "cmdId_conflict" &&
                    Text(root, "attemptedCmdId") == request.CmdId &&
                    Text(root, "command") == request.Command &&
                    Text(root, "arg") == expectedArg &&
                    Text(root, "operator") == request.Operator &&
                    !root.TryGetProperty("cmdId", out _)
                    ? DiagReplyKind.AdmissionRejected : DiagReplyKind.Ignore;
            }
            if (kind is not null and not "terminal" ||
                Text(root, "cmdId") != request.CmdId ||
                Text(root, "command") != request.Command ||
                Text(root, "operator") != request.Operator ||
                Text(root, "reason") == "cmdId_conflict") return DiagReplyKind.Ignore;
            var arg = Text(root, "arg");
            if (arg is not null && arg != expectedArg || expectedArg.Length > 0 && arg is null)
                return DiagReplyKind.Ignore;
            if (!root.TryGetProperty("requestTs", out var timestamp) ||
                !timestamp.TryGetInt64(out var requestTs) || requestTs != request.RequestTs)
                return DiagReplyKind.Ignore;
            if (!OptionalKind(root, "reason", JsonValueKind.String) ||
                !OptionalKind(root, "signal", JsonValueKind.Number) ||
                !OptionalKind(root, "timedOut", JsonValueKind.True, JsonValueKind.False) ||
                !OptionalKind(root, "cleanupConfirmed", JsonValueKind.True, JsonValueKind.False) ||
                !OptionalKind(root, "truncated", JsonValueKind.True, JsonValueKind.False))
                return DiagReplyKind.Ignore;
            if (!root.TryGetProperty("success", out var successField) ||
                successField.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
                !root.TryGetProperty("exitCode", out var exitField) ||
                exitField.ValueKind != JsonValueKind.Number || !exitField.TryGetInt32(out var exitCode))
                return DiagReplyKind.Ignore;
            var success = successField.GetBoolean();
            var signal = 0;
            if (root.TryGetProperty("signal", out var signalField) && !signalField.TryGetInt32(out signal))
                return DiagReplyKind.Ignore;
            var timedOut = root.TryGetProperty("timedOut", out var timedOutField) && timedOutField.GetBoolean();
            var cleanupUnknown = root.TryGetProperty("cleanupConfirmed", out var cleanupField) &&
                !cleanupField.GetBoolean();
            if (signal < 0 || success && (exitCode != 0 || signal != 0 || timedOut || cleanupUnknown ||
                    Text(root, "reason") is { } successReason && successReason != "exit") ||
                !success && exitCode == 0)
                return DiagReplyKind.Ignore;
            terminal = JsonSerializer.Deserialize<DiagResponse>(payload,
                new JsonSerializerOptions(JsonSerializerDefaults.Web));
            return terminal is null ? DiagReplyKind.Ignore : DiagReplyKind.Terminal;
        }
        catch (JsonException) { return DiagReplyKind.Ignore; }
        catch (InvalidOperationException) { return DiagReplyKind.Ignore; }
    }

    private static string? Text(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() : null;

    private static bool OptionalKind(JsonElement root, string name, params JsonValueKind[] kinds) =>
        !root.TryGetProperty(name, out var value) || kinds.Contains(value.ValueKind);
}
