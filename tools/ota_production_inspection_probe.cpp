// Explicit private-fixture HTTP entry. Never deploy this executable.
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc != 6 || std::string(argv[1]).rfind("/tmp/production-runtime-", 0) != 0)
            throw std::runtime_error("private fixture home, app, identity, auth directory and case required");
        namespace dm = edge_gateway::system_monitor_direct_maintenance;
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig c;
        c.otaAppConfigFile = argv[2]; c.appConfigFile = argv[2]; c.identityConfigFile = argv[3];
        c.credentialFile = std::string(argv[4]) + "/credential.json";
        c.authStateFile = std::string(argv[4]) + "/auth.json";
        const std::string scenario = argv[5];
        std::string error;
        if (scenario != "unprovisioned" && !dm::provisionCredential(c, "SyntheticInspectionOnly123!", &error))
            throw std::runtime_error("private credential provisioning failed");
        c.requireSensitiveApiEnabled = scenario == "sensitive-disabled";
        if (scenario == "sensitive-disabled") std::remove(c.authStateFile.c_str());
        std::string body = "{\"requestId\":\"INSPECT_1\",\"machineCode\":\"";
        body += scenario == "wrong-target" ? "OTHER" : "FLEET_NODE_NOT_CORE";
        body += "\",\"passwordSha256\":\"";
        body += dm::passwordSha256Hex(scenario == "wrong-password" ? "WrongSyntheticPassword" : "SyntheticInspectionOnly123!");
        body += "\"}";
        if (scenario == "duplicate") body.insert(1, "\"requestId\":\"DUPLICATE\",");
        std::cout << dm::handleHttpRequest(c, scenario == "get" ? "GET" : "POST",
            "/api/v1/ota/production-inspect", "", "", body, "127.0.0.1");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
