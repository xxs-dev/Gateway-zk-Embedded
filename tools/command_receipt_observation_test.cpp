#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/point_store_router.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <cerrno>
using namespace edge_gateway;
namespace dm = edge_gateway::system_monitor_direct_maintenance;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void write(const std::string& path, const std::string& text) { std::ofstream out(path); out << text; require(out.good(), "fixture write"); }
std::string body(const std::string& reply) {
    require(reply.find("HTTP/1.1 200") == 0, reply);
    return reply.substr(reply.find("\r\n\r\n") + 4);
}
struct Fixture {
    std::string root, segment, hash;
    SystemMonitorConfig::DirectMaintenanceConfig config;
    Fixture() {
        char name[] = "/tmp/wb-receipts-XXXXXX";
        require(mkdtemp(name), "private fixture"); root = name;
        segment = "wb_receipts_" + std::to_string(getpid());
        config.credentialFile = root + "/credential"; config.authStateFile = root + "/auth";
        config.appConfigFile = root + "/app.json"; config.identityConfigFile = root + "/device_identity.json";
        config.otaAppConfigFile = config.appConfigFile;
        config.scadaUpperComputerSafetyEnabled = false;
        std::string error;
        require(dm::provisionCredential(config, "SyntheticReceipt#2026", &error), error);
        hash = dm::passwordSha256Hex("SyntheticReceipt#2026");
        write(config.identityConfigFile, "{\"machineCode\":\"SYNTHETIC\"}");
        write(config.appConfigFile, "{\"identityConfigFile\":\"" + config.identityConfigFile + "\",\"deviceConfigFiles\":[\"" + root + "/device.json\"]}");
        writeDevice();
    }
    void writeDevice(const std::string& machine = "SYNTHETIC") {
        write(root + "/device.json", "{\"machineCode\":\"" + machine + "\",\"protocol\":{\"type\":\"modbus_tcp\"},\"memoryStore\":{\"sharedMemoryName\":\"" + segment + "\"},\"meters\":[{\"meterCode\":\"pcs\",\"points\":[{\"index\":1103,\"pointCode\":\"power\",\"address\":2,\"read\":{\"dataType\":\"int16\"},\"write\":{\"enable\":true,\"dataType\":\"int16\"}}]}]}");
    }
    std::string request(const std::string& index = "1103", const std::string& password = "") const {
        return "{\"passwordSha256\":\"" + (password.empty() ? hash : password) + "\",\"requestId\":\"observation\",\"machineCode\":\"SYNTHETIC\",\"meterCode\":\"pcs\",\"pointCode\":\"power\",\"index\":" + index + "}";
    }
    std::string send(const std::string& r) const { return dm::handleHttpRequest(config, "POST", "/api/v1/control/receipts", "", "", r, "127.0.0.1"); }
    ~Fixture() {
        shm_unlink(("/" + segment).c_str());
        for (const auto* name : {"credential", "auth", "app.json", "device_identity.json", "device.json"}) std::remove((root + "/" + name).c_str());
        rmdir(root.c_str());
    }
};
void verifyHttpIngress(Fixture& f) {
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    require(probe >= 0, "reserve loopback socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve private port");
    socklen_t size = sizeof(address);
    require(::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0, "read private port");
    ::close(probe);
    auto config = f.config;
    config.enabled = true;
    config.listenHosts = {"127.0.0.1"};
    config.allowedClientCidrs = {"127.0.0.1/32"};
    config.listenPort = ntohs(address.sin_port);
    int exit = -1;
    std::thread server([&] { exit = dm::runFromConfig(config); });
    const auto stop = [&] { dm::requestStop(); if (server.joinable()) server.join(); };
    const auto send = [&](const std::string& payload) {
        int fd = -1;
        for (int i = 0; i < 50; ++i) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            require(fd >= 0, "HTTP client socket");
            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) break;
            ::close(fd); fd = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(fd >= 0, "HTTP receipt server ready");
        try {
            timeval timeout{}; timeout.tv_sec = 3;
            require(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "HTTP timeout");
            const auto request = "POST /api/v1/control/receipts HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: " +
                std::to_string(payload.size()) + "\r\n\r\n" + payload;
            std::size_t sent = 0;
            while (sent < request.size()) {
                const auto n = ::send(fd, request.data() + sent, request.size() - sent, 0);
                if (n < 0 && errno == EINTR) continue;
                require(n > 0, "HTTP query send"); sent += n;
            }
            std::string response; char buffer[4096];
            for (;;) {
                const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n < 0 && errno == EINTR) continue;
                if (n == 0) break;
                require(n > 0 && response.size() + n < 1024 * 1024, "HTTP query receive");
                response.append(buffer, n);
            }
            ::close(fd); return response;
        } catch (...) { ::close(fd); throw; }
    };
    try {
        require(send(f.request("1103", "wrong")).find("HTTP/1.1 401") == 0, "socket authentication");
        const auto doc = json::JsonParser(body(send(f.request()))).parse();
        const auto& records = doc.find("records")->asArray().values;
        require(records.size() == 20 && records.front()->find("cmdId")->asString() == "cmd-30" &&
            !records.front()->find("success")->asBool(), "socket preserves original failure and bounded history");
        require(send(f.request("0")).find("HTTP/1.1 400") == 0, "socket index validation");
        stop(); require(exit == 0, "HTTP server normal exit");
    } catch (...) { stop(); throw; }
}
void tests() {
    Fixture f;
    require(f.send(f.request("1103", "wrong")).find("HTTP/1.1 401") == 0, "auth required");
    require(f.send(f.request()).find("HTTP/1.1 503") == 0, "missing segment unavailable");
    require(shm_open(("/" + f.segment).c_str(), 0, 0600) == -1, "query must not create segment");
    MemoryPointStore store(f.segment, MemoryStoreOpenMode::CreateExclusive);
    PendingWriteCommand pending; pending.cmdId = "pending-untouched"; pending.index = 1103; pending.value = 5;
    store.submitWriteCommand(pending);
    require(json::JsonParser(body(f.send(f.request()))).parse().find("records")->asArray().values.empty(), "empty is not success");
    for (int i = 1; i <= 30; ++i) {
        WritebackResultRecord r; r.cmdId = "cmd-" + std::to_string(i); r.index = 1103; r.value = i + 0.125;
        r.success = i != 30; r.message = "original \"message\""; r.completedAt = 1000 - i;
        r.outcomeUnknown = i == 30; r.stage = i == 30 ? "writeback-timeout" : "writeback-succeeded";
        r.verifyAttempted = true; r.verifyPassed = i != 30;
        store.recordWritebackResult(r);
    }
    WritebackResultRecord other; other.cmdId = "other-point"; other.index = 1104; store.recordWritebackResult(other);
    auto doc = json::JsonParser(body(f.send(f.request()))).parse();
    const auto& rows = doc.find("records")->asArray().values;
    require(rows.size() == 20 && rows.front()->find("cmdId")->asString() == "cmd-30" && rows.back()->find("cmdId")->asString() == "cmd-11", "bounded insertion ordering and filtering");
    require(rows.front()->find("outcome")->asString() == "unknown" && rows.back()->find("outcome")->asString() == "succeeded",
            "receipts carry the three-state outcome");
    require(!rows.front()->find("success")->asBool() && rows.front()->find("value")->asNumber() == 30.125, "preserve failed result and precision");
    require(rows.front()->find("message")->asString() == "original \"message\"", "JSON escaping");
    verifyHttpIngress(f);
    require(store.peekPendingWriteCommands().size() == 1 && store.recentWritebackResults(1103, 20).size() == 20, "query must not consume anything");
    for (const auto* bad : {"0", "-1", "1103.5", "4294967296", "\"1103\"", "null"})
        require(f.send(f.request(bad)).find("HTTP/1.1 400") == 0, "strict index validation");
    auto malformed = f.request(); malformed.pop_back();
    require(f.send(malformed).find("HTTP/1.1 400") == 0, "malformed JSON is a client error");
    auto oversized = f.request(); oversized.insert(oversized.size() - 1, ",\"padding\":\"" + std::string(8192, 'x') + "\"");
    require(f.send(oversized).find("HTTP/1.1 400") == 0, "bounded request parsing");
    auto wrong = f.request(); wrong.replace(wrong.find("SYNTHETIC"), 9, "WRONG");
    require(f.send(wrong).find("HTTP/1.1 400") == 0, "machine mismatch");
    wrong = f.request(); wrong.replace(wrong.find("power"), 5, "wrong");
    require(f.send(wrong).find("HTTP/1.1 400") == 0, "point mismatch");
    write(f.config.appConfigFile, "{\"deviceConfigFiles\":[\"" + f.root + "/device.json\",\"" + f.root + "/device.json\"]}");
    require(f.send(f.request()).find("HTTP/1.1 400") == 0, "ambiguous mapping rejected");
    write(f.config.authStateFile, "{\"paired\":true,\"sensitiveApiEnabled\":false}");
    require(f.send(f.request()).find("HTTP/1.1 403") == 0, "sensitive API gate");
    for (int i = 0; i < 5000; ++i) { other.cmdId = "wrap-" + std::to_string(i); store.recordWritebackResult(other); }
    require(store.recentWritebackResults(1103, 20).empty(), "overwritten records are not invented");
    require(store.recentWritebackResults(1104, 1).front().cmdId == "wrap-4999", "ring wrapping");
    std::cout << "command receipt observation: PASS\n";
}
// Private protocol harness mode exercises the actual authenticated request handler
// against original driver receipts. It never starts a server or sends controls.
void observe(const std::string& segment, const std::string& app, const std::string& identity, const std::string& derivedName = "") {
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    require(segment.find("wb_protocol_") == 0 && app.find("/tmp/wb-protocol-runtime-") == 0 && identity.find("/tmp/wb-protocol-runtime-") == 0, "isolated fixture required");
    Fixture f; f.config.appConfigFile = app; f.config.identityConfigFile = identity; f.config.otaAppConfigFile = app;
    MemoryPointStore store(segment, MemoryStoreOpenMode::OpenExisting);
    std::unique_ptr<MemoryPointStore> derived;
    if (!derivedName.empty()) {
        require(derivedName == segment + "_derived", "private derived segment required");
        derived.reset(new MemoryPointStore(derivedName, MemoryStoreOpenMode::OpenExisting));
    }
    const auto appConfig = ConfigLoader::loadAppConfigFromFile(app);
    const auto deviceIdentity = ConfigLoader::loadDeviceIdentityFromFile(identity);
    PointStoreRouter metadata;
    metadata.addRoutesFromDeviceConfigs(ConfigLoader::loadMany(appConfig.deviceConfigFiles, deviceIdentity), appConfig.mqttDriver.sharedMemoryName);
    const auto target = metadata.routeByIndex(1103);
    require(static_cast<bool>(target), "control point mapping required");
    std::map<std::string, WritebackResultRecord> seen;
    std::string line;
    while (std::getline(std::cin, line) && line != "quit") {
        require(line == "get", "read-only command required");
        const auto request = "{\"passwordSha256\":\"" + f.hash + "\",\"requestId\":\"protocol\",\"machineCode\":\"" + target->machineCode + "\",\"meterCode\":\"" + target->meterCode + "\",\"pointCode\":\"" + target->pointCode + "\",\"index\":1103}";
        std::string owner;
        if (derived) {
            const auto reply = dm::handleHttpRequest(f.config, "POST", "/api/v1/compute/loaded-graphs", "", "", request, "127.0.0.1");
            owner = reply.find("HTTP/1.1 200") == 0 ? body(reply) : "null";
        }
        const auto observation = body(f.send(request));
        const auto parsed = json::JsonParser(observation).parse();
        for (const auto& row : parsed.find("records")->asArray().values) {
            WritebackResultRecord r; r.cmdId = row->find("cmdId")->asString(); r.value = row->find("value")->asNumber();
            r.success = row->find("success")->asBool(); r.verifyPassed = row->find("verifyPassed")->asBool();
            r.completedAt = static_cast<std::int64_t>(row->find("writeCompletedAt")->asNumber());
            r.index = static_cast<std::uint32_t>(row->find("index")->asNumber());
            r.requestedAt = static_cast<std::int64_t>(row->find("requestedAt")->asNumber());
            r.verifyAttempted = row->find("verifyAttempted")->asBool(); seen[r.cmdId] = r;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::cout << "{\"observedAt\":" << now << ",\"receiptObservation\":" << observation;
        if (derived) std::cout << ",\"ownerObservation\":" << owner;
        std::cout << ",\"points\":[";
        bool comma = false;
        auto points = store.getAllLatest(now);
        if (derived) { const auto extra = derived->getAllLatest(now); points.insert(points.end(), extra.begin(), extra.end()); }
        for (const auto& p : points) {
            if (comma) std::cout << ','; comma = true;
            std::cout << "{\"index\":" << p.index << ",\"value\":" << p.value << ",\"quality\":" << p.quality << ",\"stale\":" << (p.stale ? "true" : "false") << ",\"ts\":" << p.ts << ",\"expireAt\":" << p.expireAt << '}';
        }
        std::cout << "],\"receipts\":["; comma = false;
        for (const auto& entry : seen) {
            const auto& r = entry.second;
            if (comma) std::cout << ','; comma = true;
            std::cout << "{\"cmdId\":\"" << r.cmdId << "\",\"value\":" << r.value << ",\"index\":" << r.index
                << ",\"requestedAt\":" << r.requestedAt << ",\"verifyAttempted\":" << (r.verifyAttempted ? "true" : "false")
                << ",\"success\":" << (r.success ? "true" : "false") << ",\"verifyPassed\":" << (r.verifyPassed ? "true" : "false") << ",\"completedAt\":" << r.completedAt << '}';
        }
        std::cout << "],\"pending\":" << store.peekPendingWriteCommands().size() << "}" << std::endl;
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "--observe") observe(argv[2], argv[3], argv[4]);
        else if (argc == 6 && std::string(argv[1]) == "--observe-owner") observe(argv[2], argv[3], argv[4], argv[5]);
        else { require(argc == 1, "unexpected arguments"); tests(); }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
