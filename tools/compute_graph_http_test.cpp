#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include "edge_gateway/compute_graph_observation.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
namespace dm = edge_gateway::system_monitor_direct_maintenance;
void require(bool v, const char* text) { if (!v) throw std::runtime_error(text); }
void write(const std::string& p, const std::string& text) { std::ofstream out(p); out << text; require(bool(out), "fixture write"); }
int main(int argc, char** argv) {
    try {
        require(argc == 2, "isolated temporary directory required");
        const std::string dir = argv[1];
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig c;
        c.otaAppConfigFile = dir + "/app.json";
        c.identityConfigFile = dir + "/identity.json";
        c.credentialFile = dir + "/credential.json"; c.authStateFile = dir + "/auth.json";
        const auto health = dir + "/health.json";
        write(c.identityConfigFile, "{\"machineCode\":\"SYNTHETIC_GRAPH_TEST\"}");
        write(c.otaAppConfigFile, "{\"runtimeMode\":\"gateway\",\"computeEngine\":{\"enabled\":true,\"healthFile\":\"" + health + "\"}}");
        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        write(health, "{\"graphObservationVersion\":1,\"ts\":" + std::to_string(ts) + ",\"healthy\":true,\"processIdentity\":\"" + edge_gateway::computeProcessIdentity(getpid()) + "\",\"loadedGraphs\":[],\"privateExtra\":\"DO_NOT_EXPORT\"}");
        std::string error;
        require(dm::provisionCredential(c, "isolated-graph-test", &error), "provision synthetic credential");
        auto request = [&](const std::string& body) { return dm::handleHttpRequest(c, "POST", "/api/v1/compute/loaded-graphs", "", "", body, "127.0.0.1"); };
        require(request("{}").find("HTTP/1.1 401") == 0, "unauthenticated graph request accepted");
        const auto auth = "\"passwordSha256\":\"" + dm::passwordSha256Hex("isolated-graph-test") + "\"";
        require(request("{" + auth + ",\"machineCode\":\"WRONG\",\"requestId\":\"r\"}").find("HTTP/1.1 400") == 0, "wrong device accepted");
        const auto body = "{" + auth + ",\"machineCode\":\"SYNTHETIC_GRAPH_TEST\",\"requestId\":\"read_1\"}";
        const auto response = request(body);
        require(response.find("HTTP/1.1 200") == 0, "valid graph request failed");
        require(response.find("gateway-loaded-graphs/1") != std::string::npos && response.find("read_1") != std::string::npos, "response correlation missing");
        require(response.find("DO_NOT_EXPORT") == std::string::npos && response.find("passwordSha256") == std::string::npos, "unrelated private fields exposed");
        write(health, "{\"schemaVersion\":\"1.1\"}");
        require(request(body).find("HTTP/1.1 503") == 0, "legacy health not rejected");
#ifdef __linux__
        std::remove(health.c_str());
        require(mkfifo(health.c_str(), 0600) == 0, "create isolated fifo");
        require(request(body).find("HTTP/1.1 503") == 0, "FIFO health not rejected promptly");
        std::remove(health.c_str());
        require(symlink(c.credentialFile.c_str(), health.c_str()) == 0, "create isolated symlink");
        require(request(body).find("HTTP/1.1 503") == 0, "symlink health not rejected");
#endif
        for (const auto& p : {c.otaAppConfigFile, c.identityConfigFile, c.credentialFile, c.authStateFile, health}) std::remove(p.c_str());
        std::cout << "{\"passed\":true,\"authenticatedRead\":true,\"wrongDeviceRejected\":true,\"legacyRejected\":true,\"privateFieldsOmitted\":true}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
