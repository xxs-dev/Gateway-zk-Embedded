#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge_gateway/system_monitor_direct_maintenance.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

std::string loopbackRequest(int port, const std::string& request) {
    int fd = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "private HTTP socket allocation failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) break;
        const int error = errno;
        ::close(fd); fd = -1;
        if (error != ECONNREFUSED) throw std::runtime_error("private HTTP connect failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(fd >= 0, "private HTTP server did not listen");
    timeval timeout{}; timeout.tv_sec = 20;
    require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "private HTTP receive timeout failed");
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto count = ::send(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "private HTTP send failed");
        sent += static_cast<std::size_t>(count);
    }
    std::string reply;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) break;
        require(count > 0 && reply.size() + static_cast<std::size_t>(count) < 1024 * 1024,
                "private HTTP response failed or unbounded");
        reply.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return reply;
}

void verifySerialHttpBudget(const edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig& base,
                            const std::string& password, const std::string& root,
                            const std::string& bin) {
    namespace dm = edge_gateway::system_monitor_direct_maintenance;
    const auto probe = ::socket(AF_INET, SOCK_STREAM, 0);
    require(probe >= 0, "private port reservation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "private port bind failed");
    socklen_t addressSize = sizeof(address);
    require(::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0,
            "private port query failed");
    const int port = ntohs(address.sin_port);
    ::close(probe);
    auto config = base;
    config.enabled = true;
    config.listenHosts = {"127.0.0.1"};
    config.listenPort = port;
    const auto started = root + "-diag-started";
    { std::ofstream script(bin + "/uptime");
      script << "#!/bin/sh\nprintf started > '" << started << "'\nsleep 2\nprintf 'socket-ok\\n'\n"; }
    require(::chmod((bin + "/uptime").c_str(), 0700) == 0, "private socket script mode");
    const auto body = std::string("{\"passwordSha256\":\"") + password +
        "\",\"machineCode\":\"GW_TEST\",\"cmdId\":\"DIRECT_SOCKET\",\"command\":\"uptime\"}";
    const auto diagRequest = std::string("POST /api/v1/diag/execute HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: ") +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    const std::string healthRequest = "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::thread server;
    std::thread client;
    std::string diagnosticReply;
    std::exception_ptr clientError;
    int serverExit = -1;
    const auto finish = [&] {
        if (client.joinable()) client.join();
        dm::requestStop();
        if (server.joinable()) server.join();
    };
    try {
        server = std::thread([&] { serverExit = dm::runFromConfig(config); });
        client = std::thread([&] {
            try { diagnosticReply = loopbackRequest(port, diagRequest); }
            catch (...) { clientError = std::current_exception(); }
        });
        bool entered = false;
        for (int i = 0; i < 100; ++i) {
            std::ifstream marker(started);
            if (marker.good()) { entered = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(entered, "private diagnostic did not enter server");
        const auto begin = std::chrono::steady_clock::now();
        const auto health = loopbackRequest(port, healthRequest);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        finish();
        if (clientError) std::rethrow_exception(clientError);
        require(serverExit == 0 && startsWith(diagnosticReply, "HTTP/1.1 200") &&
                diagnosticReply.find("socket-ok") != std::string::npos,
                "private socket diagnostic response failed");
        require(startsWith(health, "HTTP/1.1 200") &&
                elapsed >= std::chrono::milliseconds(1000) && elapsed < std::chrono::seconds(8),
                "serial health response must wait but return within HTTP budget");
    } catch (...) { finish(); throw; }
    std::remove(started.c_str());
    { std::ofstream script(bin + "/uptime");
      script << "#!/bin/sh\nprintf x >> '" << root << "-diag-count'\nprintf 'direct-ok\\n'\n"; }
    require(::chmod((bin + "/uptime").c_str(), 0700) == 0, "restore private short command mode");
}

void verifyDirectDiagnostics(const edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig& config,
                             const std::string& password, const std::string& root) {
    namespace dm = edge_gateway::system_monitor_direct_maintenance;
    const auto bin = root + "-diag-bin";
    require(::mkdir(bin.c_str(), 0700) == 0, "private diagnostic command directory");
    const auto priorPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
    require(::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1) == 0, "private diagnostic PATH");
    const auto script = [&](const std::string& name, const std::string& source) {
        const auto path = bin + "/" + name;
        { std::ofstream output(path); output << "#!/bin/sh\n" << source; }
        require(::chmod(path.c_str(), 0700) == 0, "private diagnostic script mode");
    };
    const auto counter = root + "-diag-count";
    script("uptime", "printf x >> '" + counter + "'\nprintf 'direct-ok\\n'\n");
    script("journalctl", "sleep 30\n");
    script("top", "yes X | head -c 70000\n");
    script("free", "kill -TERM $$\n");
    const auto prefix = std::string("{\"passwordSha256\":\"") + password +
        "\",\"machineCode\":\"GW_TEST\",\"operator\":\"tester\",\"requestTs\":123,";
    const auto send = [&](const std::string& payload) {
        return dm::handleHttpRequest(config, "POST", "/api/v1/diag/execute", "", "", payload, "10.0.0.7");
    };
    require(startsWith(send("{\"passwordSha256\":\"wrong\",\"machineCode\":\"GW_TEST\",\"cmdId\":\"UNAUTHORIZED\",\"command\":\"uptime\"}"),
                       "HTTP/1.1 401"), "direct diagnostic authentication changed");
    require(startsWith(send(std::string("{\"passwordSha256\":\"") + password +
        "\",\"machineCode\":\"WRONG\",\"cmdId\":\"WRONG_NODE\",\"command\":\"uptime\"}"), "HTTP/1.1 400"),
        "direct diagnostic must bind loaded identity");
    const auto request = prefix + "\"cmdId\":\"DIRECT_ONE\",\"command\":\"uptime\"}";
    const auto first = send(request);
    require(startsWith(first, "HTTP/1.1 200") && first.find("\"reason\":\"exit\"") != std::string::npos &&
            first.find("\"cleanupConfirmed\":true") != std::string::npos &&
            first.find("\"truncated\":false") != std::string::npos,
            "direct successful result must be classified and owned");
    require(send(request) == first, "same logical cmdId must replay exact terminal response");
    const auto conflict = send(prefix + "\"cmdId\":\"DIRECT_ONE\",\"command\":\"free_m\"}");
    require(startsWith(conflict, "HTTP/1.1 409") && conflict.find("\"reason\":\"cmdId_conflict\"") != std::string::npos &&
            conflict.find("\"cmdId\":") == std::string::npos,
            "conflicting cmdId must not execute");
    { std::ifstream input(counter); require(std::string((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()) == "x", "replay/conflict executed command again"); }
    const auto signalled = send(prefix + "\"cmdId\":\"DIRECT_SIGNAL\",\"command\":\"free_m\"}");
    require(signalled.find("\"reason\":\"exit\"") != std::string::npos &&
            signalled.find("\"exitCode\":143") != std::string::npos &&
            signalled.find("\"signal\":0") != std::string::npos &&
            signalled.find("\"cleanupConfirmed\":true") != std::string::npos,
            "inner script signal must not masquerade as supervised-shell signal");
    const auto truncated = send(prefix + "\"cmdId\":\"DIRECT_TRUNC\",\"command\":\"top_once\"}");
    require(truncated.find("\"truncated\":true") != std::string::npos &&
            truncated.find("[truncated]") != std::string::npos && truncated.size() < 500000,
            "direct output truncation must be structured and bounded");
    const auto start = std::chrono::steady_clock::now();
    const auto timedOut = send(prefix + "\"cmdId\":\"DIRECT_TIMEOUT\",\"command\":\"journal_tail\",\"tailLines\":4}");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(startsWith(timedOut, "HTTP/1.1 200") &&
            timedOut.find("\"reason\":\"timeout\"") != std::string::npos &&
            timedOut.find("\"timedOut\":true") != std::string::npos &&
            timedOut.find("\"cleanupConfirmed\":true") != std::string::npos &&
            elapsed < std::chrono::seconds(16), "direct timeout must clean owned job within HTTP bound");
    const auto invalid = send(prefix + "\"cmdId\":\"DIRECT_INVALID\",\"command\":\"systemctl_status\",\"service\":\"bad;name.service\"}");
    require(startsWith(invalid, "HTTP/1.1 400"), "direct service argument whitelist changed");
    verifySerialHttpBudget(config, password, root, bin);
    for (int i = 0; i < 11; ++i) {
        const auto reply = send(prefix + "\"cmdId\":\"DIRECT_FILL_" + std::to_string(i) + "\",\"command\":\"uptime\"}");
        require(startsWith(reply, "HTTP/1.1 200"), "direct bounded replay cache fill failed");
    }
    const auto full = send(prefix + "\"cmdId\":\"DIRECT_OVERFLOW\",\"command\":\"uptime\"}");
    require(startsWith(full, "HTTP/1.1 429") && full.find("\"reason\":\"cache_full\"") != std::string::npos &&
            full.find("\"cmdId\":") == std::string::npos, "cache full must reject without terminal cmdId reply");
    require(::setenv("PATH", priorPath.c_str(), 1) == 0, "restore diagnostic PATH");
    for (const auto& name : {"uptime", "journalctl", "top", "free"}) std::remove((bin + "/" + name).c_str());
    std::remove(counter.c_str());
    require(::rmdir(bin.c_str()) == 0, "private diagnostic command cleanup");
}

void verifyMaintenanceCredential() {
    namespace dm = edge_gateway::system_monitor_direct_maintenance;
    const auto root = std::string("/tmp/direct-maintenance-contract-") + std::to_string(::getpid());
    edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
    config.credentialFile = root + "-credential.json";
    config.authStateFile = root + "-auth-state.json";
    config.listenHosts = {"127.0.0.1"};
    config.maxAuthFailures = 3;
    std::remove(config.credentialFile.c_str());
    std::remove(config.authStateFile.c_str());

    bool rejectedEmptyCidrs = false;
    try {
        dm::validateListenerConfig(config);
    } catch (const std::exception&) {
        rejectedEmptyCidrs = true;
    }
    require(rejectedEmptyCidrs, "empty allowedClientCidrs must be refused by default");
    config.allowAnyClient = true;
    dm::validateListenerConfig(config);

    const std::string legacyHash = "5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3";
    const std::string legacyBody = std::string("{\"passwordSha256\":\"") + legacyHash + "\"}";
    require(!dm::credentialProvisioned(config), "credential should not exist yet");
    require(!dm::verifyPasswordSha256(config, legacyHash), "fixed legacy hash must be rejected by default");
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/auth/bootstrap-login", "", "", legacyBody, "10.0.0.1"),
                       "HTTP/1.1 503"),
            "sensitive endpoints must be refused while no credential is provisioned");
    config.allowLegacyFixedPassword = true;
    require(dm::verifyPasswordSha256(config, legacyHash), "legacy hash should be accepted only when explicitly allowed");
    config.allowLegacyFixedPassword = false;

    std::string error;
    require(!dm::provisionCredential(config, "short", &error), "short maintenance passwords must be rejected");
    require(dm::provisionCredential(config, "Maint#2026-secret", &error), "credential provisioning failed: " + error);
    require(dm::credentialProvisioned(config), "credential should be provisioned");
    const auto good = dm::passwordSha256Hex("Maint#2026-secret");
    require(good.size() == 64, "client passwordSha256 helper is broken");
    require(dm::verifyPasswordSha256(config, good), "provisioned password was rejected");
    require(!dm::verifyPasswordSha256(config, dm::passwordSha256Hex("wrong-password")), "wrong password was accepted");
    require(!dm::verifyPasswordSha256(config, legacyHash), "fixed legacy hash must not unlock a provisioned device");

    const std::string goodBody = std::string("{\"passwordSha256\":\"") + good + "\"}";
    const auto login = dm::handleHttpRequest(config, "POST", "/api/v1/auth/bootstrap-login", "", "", goodBody, "10.0.0.2");
    require(startsWith(login, "HTTP/1.1 200") && login.find("\"sensitiveApiEnabled\":true") != std::string::npos,
            "provisioned login should succeed and enable the sensitive API");
    // Authorized request reaches the handler (which then fails on the empty test app config).
    const auto authorized = dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.2");
    require(!startsWith(authorized, "HTTP/1.1 401") && !startsWith(authorized, "HTTP/1.1 403") &&
                !startsWith(authorized, "HTTP/1.1 503"),
            "valid credential did not pass the sensitive endpoint gate");

    const std::string badBody = std::string("{\"passwordSha256\":\"") + dm::passwordSha256Hex("nope-nope") + "\"}";
    for (int attempt = 0; attempt < 3; ++attempt) {
        require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", badBody, "10.0.0.3"),
                           "HTTP/1.1 401"),
                "wrong password should be unauthorized");
    }
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.3"),
                       "HTTP/1.1 429"),
            "client must be locked out after repeated failures");

    config.otaAppConfigFile = root + "-app.json";
    config.identityConfigFile = root + "-identity.json";
    config.otaStatusFile = root + "-ota-status.jsonl";
    { std::ofstream app(config.otaAppConfigFile);
      app << "{\"ota\":{\"enabled\":true,\"requireSignature\":true,\"checksumRequired\":true,"
             "\"downloadDir\":\"" << root << "-downloads\",\"stagingDir\":\"" << root
          << "-staging\",\"backupDir\":\"" << root << "-backup\",\"signaturePublicKeyPath\":\""
          << root << "-missing.pub\"}}\n"; }
    { std::ofstream identity(config.identityConfigFile); identity << "{\"machineCode\":\"GW_TEST\"}\n"; }
    const auto base = std::string("{\"passwordSha256\":\"") + good +
        "\",\"packageType\":\"config\",\"artifactUrl\":\"" + root +
        "-payload.tar.gz\",\"version\":\"1.0\",\"sha256\":\"" + std::string(64, '0') +
        "\",\"size\":7";
    const auto unsignedReply = dm::handleHttpRequest(config, "POST", "/api/v1/ota/jobs", "", "",
        base + ",\"jobId\":\"SIGNED_EXPLICIT\"}", "10.0.0.5");
    require(startsWith(unsignedReply, "HTTP/1.1 400") &&
            unsignedReply.find("ota signature is required") != std::string::npos,
            "Direct must reject missing OTA signature");
    const auto missingId = dm::handleHttpRequest(config, "POST", "/api/v1/ota/jobs", "", "",
        base + ",\"signature\":\"AAAA\"}", "10.0.0.5");
    require(startsWith(missingId, "HTTP/1.1 400") &&
            missingId.find("invalid ota jobId") != std::string::npos,
            "Direct must not invent an ID for a signed request");
    const auto signedReply = dm::handleHttpRequest(config, "POST", "/api/v1/ota/jobs", "", "",
        base + ",\"signature\":\"AAAA\",\"jobId\":\"SIGNED_EXPLICIT\"}", "10.0.0.5");
    require(startsWith(signedReply, "HTTP/1.1 202") &&
            signedReply.find("\"jobId\":\"SIGNED_EXPLICIT\"") != std::string::npos,
            "Direct must forward signed request with unchanged job ID");
    bool asyncFinished = false;
    for (int i = 0; i < 200; ++i) {
        std::ifstream input(config.otaStatusFile);
        const std::string statuses((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (statuses.find("\"jobId\":\"SIGNED_EXPLICIT\"") != std::string::npos &&
            statuses.find("\"stage\":\"failed\"") != std::string::npos) { asyncFinished = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(asyncFinished, "Direct signed request worker did not finish locally");
    verifyDirectDiagnostics(config, good, root);
    std::remove(config.otaAppConfigFile.c_str());
    std::remove(config.identityConfigFile.c_str());
    std::remove(config.otaStatusFile.c_str());

    std::remove(config.authStateFile.c_str());
    require(startsWith(dm::handleHttpRequest(config, "POST", "/api/v1/config/delete", "", "", goodBody, "10.0.0.4"),
                       "HTTP/1.1 403"),
            "sensitive API must stay disabled while authState.sensitiveApiEnabled is false");
    std::remove(config.credentialFile.c_str());
}

}  // namespace

int main() {
    try {
        const auto json = edge_gateway::system_monitor_direct_maintenance::otaCapabilitiesJson();
        require(json.find("\"supportedPackageTypes\":[\"config\",\"full\",\"scada\"]") != std::string::npos,
                "OTA package capabilities are incomplete");
        require(json.find("\"emsLogic\":{") != std::string::npos,
                "emsLogic capability object is missing");
        require(json.find("\"editorSourceSchema\":\"2.x\"") != std::string::npos,
                "editor source schema capability is incorrect");
        require(json.find("\"runtimeSchema\":\"2.x\"") != std::string::npos,
                "runtime schema capability is incorrect");
        require(json.find("\"compilerContract\":\"GraphEmsV2/direct\"") != std::string::npos,
                "compiler contract capability is incorrect");
        require(json.find("\"executesEditorSource\":true") != std::string::npos,
                "direct V2 execution capability is incorrect");
        verifyMaintenanceCredential();
        std::cout << "system_monitor_direct_maintenance_contract_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "system_monitor_direct_maintenance_contract_test failed: " << ex.what() << "\n";
        return 1;
    }
}
