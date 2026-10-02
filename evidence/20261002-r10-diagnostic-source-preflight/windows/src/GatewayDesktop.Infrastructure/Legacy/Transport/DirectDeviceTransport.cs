using System.Net.Http.Json;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text;
using GatewayDesktop.Core.Devices;
using GatewayDesktop.Transport;

namespace GatewayDesktop.Transport.Direct;

public sealed class DirectDeviceTransport : IDeviceTransport
{
    private static readonly JsonSerializerOptions RequestJsonOptions = new(JsonSerializerDefaults.Web);
    private readonly HttpClient httpClient;

    public DirectDeviceTransport(HttpClient? httpClient = null)
    {
        this.httpClient = httpClient ?? CreateDirectHttpClient();
    }

    public DeviceAccessMode Mode => DeviceAccessMode.LocalDirect;

    private static HttpClient CreateDirectHttpClient()
    {
        return new HttpClient(new HttpClientHandler
        {
            UseProxy = false
        })
        {
            Timeout = TimeSpan.FromSeconds(30)
        };
    }

    private async Task<HttpResponseMessage> PostJsonAsync<T>(
        Uri uri,
        T value,
        CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, RequestJsonOptions);
        using var request = new HttpRequestMessage(HttpMethod.Post, uri)
        {
            Content = new ByteArrayContent(payload)
        };
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json")
        {
            CharSet = "utf-8"
        };

        // SystemMonitor's embedded HTTP server requires Content-Length and does not
        // accept Transfer-Encoding: chunked. ByteArrayContent supplies the exact length.
        return await httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
    }

    public async Task<TransportResult<DeviceHealth>> GetHealthAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<DeviceHealth>(
                new Uri(baseAddress, "/api/v1/health"),
                cancellationToken
            );
            return value is null
                ? TransportResult<DeviceHealth>.Fail("direct maintenance API returned empty health response")
                : TransportResult<DeviceHealth>.Ok(value, "SystemMonitor direct maintenance API is reachable");
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<DeviceHealth>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<DeviceIdentity>> GetIdentityAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<DeviceIdentity>(
                new Uri(baseAddress, "/api/v1/identity"),
                cancellationToken
            );
            return value is null
                ? TransportResult<DeviceIdentity>.Fail("direct maintenance API returned empty identity response")
                : TransportResult<DeviceIdentity>.Ok(value);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<DeviceIdentity>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<IReadOnlyList<OnlineDevice>>> DiscoverOnlineDevicesAsync(
        DeviceEndpoint endpoint,
        TimeSpan scanDuration,
        CancellationToken cancellationToken
    ) => Task.FromResult(TransportResult<IReadOnlyList<OnlineDevice>>.Ok(
        Array.Empty<OnlineDevice>(),
        "local direct mode does not scan MQTT online devices"
    ));

    public async Task<TransportResult<DirectAuthState>> GetAuthStateAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<DirectAuthState>(
                new Uri(baseAddress, "/api/v1/auth/state"),
                cancellationToken
            );
            return value is null
                ? TransportResult<DirectAuthState>.Fail("direct maintenance API returned empty auth state response")
                : TransportResult<DirectAuthState>.Ok(value);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<DirectAuthState>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<RealtimePointSnapshot>> GetRealtimePointsAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var path = string.IsNullOrWhiteSpace(endpoint.RealtimeMeterCode)
                ? "/api/v1/realtime/points"
                : "/api/v1/realtime/points?meterCode=" + Uri.EscapeDataString(endpoint.RealtimeMeterCode.Trim());
            var value = await httpClient.GetFromJsonAsync<RealtimePointSnapshot>(
                new Uri(baseAddress, path),
                cancellationToken
            );
            value = FilterSnapshotByMeter(value, endpoint.RealtimeMeterCode);
            return value is null
                ? TransportResult<RealtimePointSnapshot>.Fail("direct maintenance API returned empty realtime response")
                : TransportResult<RealtimePointSnapshot>.Ok(value);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<RealtimePointSnapshot>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<bool>> StopRealtimeAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    ) => Task.FromResult(TransportResult<bool>.Ok(
        false,
        "local direct mode reads snapshots and does not create a realtime push lease"));

    public async Task<TransportResult<FullTelemetrySnapshot>> GetFullTelemetryAsync(
        DeviceEndpoint endpoint,
        TimeSpan listenDuration,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<FullTelemetrySnapshot>(
                new Uri(baseAddress, "/api/v1/telemetry/full"),
                cancellationToken
            );
            return value is null
                ? TransportResult<FullTelemetrySnapshot>.Fail("direct maintenance API returned empty full telemetry response")
                : TransportResult<FullTelemetrySnapshot>.Ok(value, "direct full telemetry snapshot loaded");
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<FullTelemetrySnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<OtaCapabilities>> GetOtaCapabilitiesAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<OtaCapabilities>(
                new Uri(baseAddress, "/api/v1/ota/capabilities"),
                cancellationToken
            );
            return value is null
                ? TransportResult<OtaCapabilities>.Fail("direct maintenance API returned empty OTA capabilities response")
                : TransportResult<OtaCapabilities>.Ok(value);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<OtaCapabilities>.Fail(ex.Message);
        }
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
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            using var response = await PostJsonAsync(
                new Uri(baseAddress, "/api/v1/ota/jobs"),
                request,
                cancellationToken
            );
            var value = await response.Content.ReadFromJsonAsync<OtaJobResponse>(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                if (value is { Success: false } &&
                    string.Equals(value.JobId, request.JobId, StringComparison.Ordinal) &&
                    string.Equals(value.PackageType, request.PackageType, StringComparison.Ordinal))
                    return TransportResult<OtaJobResponse>.Ok(value, value.Message);
                return TransportResult<OtaJobResponse>.Fail(
                    value?.Message ?? $"direct OTA request failed: {(int)response.StatusCode}"
                );
            }

            return value is null
                ? TransportResult<OtaJobResponse>.Fail("direct maintenance API returned empty OTA job response")
                : TransportResult<OtaJobResponse>.Ok(value, value.Message);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<OtaJobResponse>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<OtaStatusSnapshot>> GetOtaStatusAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<OtaStatusSnapshot>(
                new Uri(baseAddress, "/api/v1/ota/status"),
                cancellationToken
            );
            return value is null
                ? TransportResult<OtaStatusSnapshot>.Fail("direct maintenance API returned empty OTA status response")
                : TransportResult<OtaStatusSnapshot>.Ok(value);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
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
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            progress?.Report(new ConfigPullProgress("request", "正在通过 SystemMonitor 直连维护接口拉取配置快照...", 20, 0, 0));
            using var response = await PostJsonAsync(
                new Uri(baseAddress, "/api/v1/config/snapshot"),
                request,
                cancellationToken);
            progress?.Report(new ConfigPullProgress("parse", "已收到直连维护配置快照，正在解析...", 80, 0, 0));
            var value = await response.Content.ReadFromJsonAsync<ConfigSnapshot>(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                return TransportResult<ConfigSnapshot>.Fail(
                    value?.Message ?? $"direct config pull failed: {(int)response.StatusCode}"
                );
            }

            return value is null
                ? TransportResult<ConfigSnapshot>.Fail("direct maintenance API returned empty config snapshot")
                : TransportResult<ConfigSnapshot>.Ok(value, $"config snapshot files={value.FileCount}");
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<ConfigSnapshot>.Fail(ex.Message);
        }
    }

    public async Task<TransportResult<SystemStatusSnapshot>> GetSystemStatusAsync(
        DeviceEndpoint endpoint,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            var value = await httpClient.GetFromJsonAsync<SystemStatusSnapshot>(
                new Uri(baseAddress, "/api/v1/system/status"),
                cancellationToken
            );
            return value is null
                ? TransportResult<SystemStatusSnapshot>.Fail("direct maintenance API returned empty system status")
                : TransportResult<SystemStatusSnapshot>.Ok(value, "system status loaded");
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
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
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            using var response = await PostJsonAsync(
                new Uri(baseAddress, "/api/v1/diag/execute"),
                request,
                cancellationToken
            );
            using var content = await response.Content.ReadAsStreamAsync(cancellationToken);
            using var bytes = new MemoryStream();
            var buffer = new byte[8192];
            int read;
            while ((read = await content.ReadAsync(buffer, cancellationToken)) > 0)
            {
                if (bytes.Length + read > DiagReplyAdmission.MaxDirectJsonBytes)
                    return TransportResult<DiagResponse>.Fail("direct diag response exceeded bounded JSON limit");
                bytes.Write(buffer, 0, read);
            }
            var payload = Encoding.UTF8.GetString(bytes.ToArray());
            if (!response.IsSuccessStatusCode)
            {
                string reason;
                try
                {
                    using var document = JsonDocument.Parse(payload);
                    var root = document.RootElement;
                    reason = root.ValueKind != JsonValueKind.Object ? "invalid error response" :
                        root.TryGetProperty("reason", out var field) && field.ValueKind == JsonValueKind.String
                        ? field.GetString() ?? "unknown" :
                        root.TryGetProperty("message", out field) && field.ValueKind == JsonValueKind.String
                        ? field.GetString() ?? "unknown" : "unknown";
                }
                catch (Exception ex) when (ex is JsonException or InvalidOperationException)
                { reason = "invalid error response"; }
                return TransportResult<DiagResponse>.Fail(
                    $"direct diag HTTP {(int)response.StatusCode}: {reason}"
                );
            }
            var matched = DiagReplyAdmission.Match(payload, request, endpoint.MachineCode, out var value);
            return matched != DiagReplyKind.Terminal || value is null
                ? TransportResult<DiagResponse>.Fail("direct diag response did not match the target request")
                : TransportResult<DiagResponse>.Ok(value, $"diag exitCode={value.ExitCode}");
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or IOException)
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
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            using var response = await PostJsonAsync(
                new Uri(baseAddress, "/api/v1/config/apply"),
                request,
                cancellationToken
            );
            var value = await response.Content.ReadFromJsonAsync<ConfigApplyResponse>(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                return TransportResult<ConfigApplyResponse>.Fail(
                    value?.Message ?? $"direct config apply failed: {(int)response.StatusCode}"
                );
            }

            return value is null
                ? TransportResult<ConfigApplyResponse>.Fail("direct maintenance API returned empty config apply response")
                : TransportResult<ConfigApplyResponse>.Ok(value, value.Message);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<ConfigApplyResponse>.Fail(ex.Message);
        }
    }

    public Task<TransportResult<ConfigFileOperationResponse>> DeleteConfigFileAsync(
        DeviceEndpoint endpoint,
        ConfigFileOperationRequest request,
        CancellationToken cancellationToken
    ) => PostConfigFileOperationAsync(endpoint, "/api/v1/config/delete", request, "direct config delete", cancellationToken);

    public Task<TransportResult<ConfigFileOperationResponse>> RestoreConfigBackupAsync(
        DeviceEndpoint endpoint,
        ConfigFileOperationRequest request,
        CancellationToken cancellationToken
    ) => PostConfigFileOperationAsync(endpoint, "/api/v1/config/restore", request, "direct config restore", cancellationToken);

    public async Task<TransportResult<BatchControlResponse>> ExecuteBatchControlAsync(
        DeviceEndpoint endpoint,
        BatchControlRequest request,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            using var response = await PostJsonAsync(
                new Uri(baseAddress, "/api/v1/control/batch"),
                request,
                cancellationToken
            );
            var value = await response.Content.ReadFromJsonAsync<BatchControlResponse>(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                return TransportResult<BatchControlResponse>.Fail(
                    value?.Message ?? $"direct batch control failed: {(int)response.StatusCode}"
                );
            }

            return value is null
                ? TransportResult<BatchControlResponse>.Fail("direct maintenance API returned empty batch control response")
                : TransportResult<BatchControlResponse>.Ok(value, value.Message);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<BatchControlResponse>.Fail(ex.Message);
        }
    }

    private async Task<TransportResult<ConfigFileOperationResponse>> PostConfigFileOperationAsync(
        DeviceEndpoint endpoint,
        string path,
        ConfigFileOperationRequest request,
        string label,
        CancellationToken cancellationToken
    )
    {
        var baseAddress = RequireDirectBaseAddress(endpoint);
        try
        {
            using var response = await PostJsonAsync(
                new Uri(baseAddress, path),
                request,
                cancellationToken
            );
            var value = await response.Content.ReadFromJsonAsync<ConfigFileOperationResponse>(cancellationToken);
            if (!response.IsSuccessStatusCode)
            {
                return TransportResult<ConfigFileOperationResponse>.Fail(
                    value?.Message ?? $"{label} failed: {(int)response.StatusCode}"
                );
            }

            return value is null
                ? TransportResult<ConfigFileOperationResponse>.Fail($"direct maintenance API returned empty {label} response")
                : TransportResult<ConfigFileOperationResponse>.Ok(value, value.Message);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException)
        {
            return TransportResult<ConfigFileOperationResponse>.Fail(ex.Message);
        }
    }

    private static RealtimePointSnapshot? FilterSnapshotByMeter(
        RealtimePointSnapshot? snapshot,
        string meterCode
    )
    {
        if (snapshot is null || string.IsNullOrWhiteSpace(meterCode))
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

    private static Uri RequireDirectBaseAddress(DeviceEndpoint endpoint)
    {
        if (endpoint.DirectBaseAddress is null)
        {
            throw new InvalidOperationException("direct base address is required for local direct mode");
        }

        return endpoint.DirectBaseAddress;
    }
}
