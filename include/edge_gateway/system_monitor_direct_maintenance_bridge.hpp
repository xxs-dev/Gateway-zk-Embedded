#pragma once

// Read-only access to the v1 direct-maintenance building blocks for the v2
// channel (src/maintenance_v2). Implemented at the end of
// system_monitor_direct_maintenance.cpp so v1 and v2 share one implementation
// of identity, credential check, config path rules, config validation,
// snapshot, diagnostics and the point-store context.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {
namespace system_monitor_direct_maintenance {
namespace bridge {

using Config = SystemMonitorConfig::DirectMaintenanceConfig;

struct Identity {
    std::string machineCode;
    std::string imei;
    std::string serialNumber;
    std::string model;
    std::string hardwareVersion;
    std::string firmwareVersion;
};
Identity identity(const Config& config);
// identity machineCode, falling back to the app's MQTT clientId (same as v1).
std::string machineCode(const Config& config);

struct AuthStateInfo {
    bool paired = false;
    bool usingDefaultPassword = true;
    bool bootstrapMode = true;
    bool sensitiveApiEnabled = false;
};
AuthStateInfo authState(const Config& config);

enum class LoginResult { Ok, LockedOut, NotProvisioned, Invalid };
// v1 credential check including the per-client lockout (shared with v1).
LoginResult authenticate(const Config& config, const std::string& passwordSha256, const std::string& clientKey);
bool clientAllowed(const Config& config, std::uint32_t ipv4HostOrder);

struct Metrics {
    double cpuUsage = 0;
    double memUsage = 0;
    double diskUsage = 0;
    double load1 = 0;
    int processCount = 0;
};
Metrics metrics();
std::string serviceState(const std::string& unit);  // systemctl is-active (validated unit name)

// Same bodies as v1 (password checks are done by the caller).
std::string configSnapshotJson(const Config& config, const std::string& body);  // throws
int diagExecute(const Config& config, const std::string& body, std::string* responseBody);  // HTTP status
std::string otaStatusJson(const Config& config);

std::vector<std::string> configFiles(const Config& config);
bool isAllowedExistingConfigPath(const Config& config, const std::string& path);
// Same loader checks as v1 config/apply; throws with the loader's message.
void validateConfigContent(const Config& config, const std::string& path, const std::string& content);
std::string runShell(const std::string& command, int* exitCode, std::size_t maxOutputBytes);

// Point store context (app config, device configs, shared memory, routes).
class Realtime {
public:
    explicit Realtime(const Config& config);
    ~Realtime();
    PointStoreRouter& router();
    const AppConfig& app() const;
    std::string machineCode() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Optional<WritebackResultRecord> waitForWriteback(
    const PointStoreRouter& router,
    const PointStoreRoute& route,
    const std::string& cmdId,
    std::int64_t acceptedAt,
    int timeoutMs
);

}  // namespace bridge
}  // namespace system_monitor_direct_maintenance
}  // namespace edge_gateway
