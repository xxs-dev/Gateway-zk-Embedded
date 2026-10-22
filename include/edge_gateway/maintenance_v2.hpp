#pragma once

// Direct maintenance channel v2 (HTTPS, pinned device certificate, session
// tokens, platform-signed field tickets, journaled config transactions with
// health check and automatic rollback, controlled point writes with dry-run,
// hash-chained device audit). Runs inside SystemMonitor next to v1.
//
// Material directory (root only, not part of config snapshot/apply):
//   server.crt / server.key        device certificate (ECDSA P-256, CN = machine code)
//   trusted-ticket-keys.json       platform Ed25519 keys trusted for field tickets
// State directory:
//   sessions.json, used-tickets.json, audit.jsonl, debug-mode.json, transactions/<id>/

#include <ostream>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace maintenance_v2 {

struct Options {
    std::string materialDirectory = "/opt/modbus-gateway/config/maintenance-v2";
    std::string stateDirectory = "/opt/modbus-gateway/data/maintenance-v2";
    // Empty: systemd ("gateway-services.sh list", "systemctl is-active", "systemctl restart gateway-services.service").
    // Otherwise an executable called as "<cmd> list | state <unit> | restart" (tests).
    std::string servicesCommand;
    // argv prefix that re-runs this program for a transaction helper; the server
    // appends "--maintenance-v2-tx <id> --maintenance-v2-tx-mode <mode>".
    std::vector<std::string> helperCommand;
    // "auto": systemd-run when systemd is running, otherwise a detached child.
    std::string helperLauncher = "auto";
};

using DirectConfig = SystemMonitorConfig::DirectMaintenanceConfig;

// Blocks until requestStop(). Returns non-zero when the server cannot start.
int run(const DirectConfig& config, const Options& options);
void requestStop();

// `SystemMonitor --init-maintenance-tls`: creates the device certificate when
// missing (idempotent) and prints "fingerprint=AA:BB:..." (or "unsupported=1").
int initTls(const DirectConfig& config, const Options& options, std::ostream& out, std::ostream& err);

// Transaction helper process (mode: apply | restart | recover).
int runTransactionHelper(const DirectConfig& config, const Options& options, const std::string& transactionId, const std::string& mode);

}  // namespace maintenance_v2
}  // namespace edge_gateway
