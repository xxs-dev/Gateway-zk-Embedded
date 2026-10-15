// Real HTTP dispatcher, private credentials, no listening server or device access.
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace dm = edge_gateway::system_monitor_direct_maintenance;
void requireTrace(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    char dir[] = "/tmp/gateway-trace-api-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    const auto root = std::string(dir);
    edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
    config.identityConfigFile = root + "/identity.json";
    config.otaAppConfigFile = root + "/app.json";
    config.credentialFile = root + "/credential.json";
    config.authStateFile = root + "/auth-state.json";
    config.maxAuthFailures = 3;
    { std::ofstream f(config.identityConfigFile); f << "{\"machineCode\":\"TRACE_TEST\"}"; }
    { std::ofstream f(config.otaAppConfigFile); f << "{\"deviceConfigFiles\":[],\"mqtt\":{\"clientId\":\"TRACE_TEST\"}}"; }
    const auto cleanup = [&] {
        for (const auto& file : {config.identityConfigFile, config.otaAppConfigFile,
                                config.credentialFile, config.authStateFile}) unlink(file.c_str());
        rmdir(dir);
    };
    const auto send = [&](const std::string& action, const std::string& body,
                          const std::string& client = "trace-test") {
        return dm::handleHttpRequest(config, "POST", "/api/v1/channel-trace/" + action,
                                     "", "", body, client);
    };
    try {
        for (const auto* action : {"capabilities", "start", "read", "stop"}) {
            requireTrace(send(action, "{}").find("503 Service Unavailable") != std::string::npos,
                         "unprovisioned trace endpoint must fail closed");
        }
        std::string error;
        requireTrace(dm::provisionCredential(config, "Trace-test-only-2026!", &error), error.c_str());
        const auto auth = "\"passwordSha256\":\"" + dm::passwordSha256Hex("Trace-test-only-2026!") + "\"";
        const auto valid = "{" + auth + ",\"machineCode\":\"TRACE_TEST\"}";
        for (const auto* action : {"capabilities", "start", "read", "stop"}) {
            requireTrace(send(action, "{}", action).find("401 Unauthorized") != std::string::npos,
                         "all trace endpoints must authenticate");
        }
        const auto wrong = send("capabilities", "{" + auth + ",\"machineCode\":\"OTHER\"}");
        requireTrace(wrong.find("machineCode mismatch") != std::string::npos, "wrong identity accepted");
        const auto cap = send("capabilities", valid);
        requireTrace(cap.find("200 OK") != std::string::npos &&
                     cap.find("\"supported\":true") != std::string::npos &&
                     cap.find("io-chunk") != std::string::npos, "capability contract failed");
        // Successful requests must not authorize the next request on the same worker.
        requireTrace(send("capabilities", "{}").find("401 Unauthorized") != std::string::npos,
                     "request authentication leaked across calls");
        const auto unknown = send("start", "{" + auth + ",\"machineCode\":\"TRACE_TEST\","
                                  "\"channel\":\"/tmp/other.json\",\"sessionId\":\"s1\",\"durationSec\":60}");
        requireTrace(unknown.find("not referenced") != std::string::npos, "unreferenced channel accepted");
        for (int i = 0; i < config.maxAuthFailures; ++i) {
            requireTrace(send("capabilities", "{}", "locked-trace-client").find("401 Unauthorized") != std::string::npos,
                         "bad credential must fail");
        }
        requireTrace(send("capabilities", valid, "locked-trace-client").find("429 Too Many Requests") != std::string::npos,
                     "trace bypasses shared authentication lockout");
        unlink(config.authStateFile.c_str());
        for (const auto* action : {"capabilities", "start", "read", "stop"}) {
            requireTrace(send(action, valid).find("403 Forbidden") != std::string::npos,
                         "trace bypasses sensitive API switch");
        }
        const auto legacy = dm::handleHttpRequest(config, "GET", "/api/v1/identity", "", "", "", "trace-test");
        requireTrace(legacy.find("200 OK") != std::string::npos && legacy.find("TRACE_TEST") != std::string::npos,
                     "existing identity endpoint changed");
        const auto notFound = dm::handleHttpRequest(config, "GET", "/api/v1/channel-trace/capabilities", "", "", "", "trace-test");
        requireTrace(notFound.find("404 Not Found") != std::string::npos, "unauthenticated GET trace route exposed");
        cleanup();
        std::cout << "channel trace API: provisioned credentials, four auth gates, request isolation, lockout, sensitive API switch, identity and allowlist passed\n";
    } catch (...) { cleanup(); throw; }
}
