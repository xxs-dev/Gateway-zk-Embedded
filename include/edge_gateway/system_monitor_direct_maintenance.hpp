#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace system_monitor_direct_maintenance {

std::string otaCapabilitiesJson();
int runFromConfig(const SystemMonitorConfig::DirectMaintenanceConfig& config);
void requestStop();

// Per-device maintenance credential. Clients keep sending passwordSha256 =
// hex(SHA-256(password)); the device stores PBKDF2-HMAC-SHA256(passwordSha256, salt).
bool credentialProvisioned(const SystemMonitorConfig::DirectMaintenanceConfig& config);
bool verifyPasswordSha256(
    const SystemMonitorConfig::DirectMaintenanceConfig& config,
    const std::string& passwordSha256
);
std::string passwordSha256Hex(const std::string& password);
// Writes credentialFile (0600) and marks authStateFile paired/sensitiveApiEnabled.
bool provisionCredential(
    const SystemMonitorConfig::DirectMaintenanceConfig& config,
    const std::string& password,
    std::string* error
);
// Throws when the listener configuration is unsafe (e.g. empty allowedClientCidrs).
void validateListenerConfig(const SystemMonitorConfig::DirectMaintenanceConfig& config);
// Routes one HTTP request exactly like the server loop (clientKey = peer address for lockout).
std::string handleHttpRequest(
    const SystemMonitorConfig::DirectMaintenanceConfig& config,
    const std::string& method,
    const std::string& path,
    const std::string& query,
    const std::string& headers,
    const std::string& body,
    const std::string& clientKey
);

}  // namespace system_monitor_direct_maintenance
}  // namespace edge_gateway
