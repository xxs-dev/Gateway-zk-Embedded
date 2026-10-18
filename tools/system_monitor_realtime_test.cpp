// Real HTTP dispatcher and private shared memory; no network or physical driver.
#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/json_value.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using edge_gateway::json::JsonValue;
namespace dm = edge_gateway::system_monitor_direct_maintenance;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
const JsonValue& field(const JsonValue& value, const char* key) {
    const auto* result = value.find(key);
    require(result != nullptr, key);
    return *result;
}

int main(int argc, char** argv) {
    char directory[] = "/tmp/gateway-realtime-expiry-XXXXXX";
    require(mkdtemp(directory) != nullptr, "fixture directory");
    const std::string root(directory), segment = root.substr(root.find_last_of('/') + 1);
    const auto app = root + "/app.json", device = root + "/device.json", identity = root + "/device_identity.json";
    const auto cleanup = [&] {
        shm_unlink(("/" + segment).c_str());
        for (const auto& file : {app, device, identity}) unlink(file.c_str());
        rmdir(root.c_str());
    };
    try {
        { std::ofstream f(identity); f << "{\"machineCode\":\"REALTIME_TEST\"}"; }
        {
            std::ofstream f(device);
            f << "{\"protocol\":{\"type\":\"computed\"},\"memoryStore\":{\"sharedMemoryName\":\"" << segment
              << "\"},\"meters\":[{\"meterCode\":\"M\",\"points\":[";
            for (int i = 1; i <= 3; ++i) {
                if (i > 1) f << ',';
                f << "{\"index\":" << i << ",\"pointCode\":\"P" << i
                  << "\",\"enabled\":true,\"read\":{\"enable\":true,\"dataType\":\"computed\",\"function\":0,\"length\":0},\"write\":{\"enable\":false}}";
            }
            f << "]}]}";
        }
        {
            std::ofstream f(app);
            f << "{\"identityConfigFile\":\"" << identity << "\",\"deviceConfigFiles\":[\"" << device
              << "\"],\"mqtt\":{\"clientId\":\"REALTIME_TEST\"},\"mqttDriver\":{\"sharedMemoryName\":\"" << segment << "\"}}";
        }
        edge_gateway::MemoryPointStore store(segment, edge_gateway::MemoryStoreOpenMode::CreateExclusive);
        const auto configs = edge_gateway::ConfigLoader::loadMany({device}, edge_gateway::ConfigLoader::loadDeviceIdentityFromFile(identity));
        store.registerDevicePoints(configs);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int i = 1; i <= 3; ++i) {
            edge_gateway::PointValue p;
            p.index = i; p.machineCode = "REALTIME_TEST"; p.meterCode = "M"; p.pointCode = "P" + std::to_string(i);
            p.value = i * 12.5; p.ts = now - 30000;
            p.expireAt = i == 1 ? now + 60000 : (i == 2 ? 0 : now - 1000);
            store.putLatest(p);
        }
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
        config.appConfigFile = app; config.identityConfigFile = identity;
        config.scadaUpperComputerSafetyEnabled = false;
        const auto request = [&](const std::string& route, const std::string& query = "") {
            const auto response = dm::handleHttpRequest(config, "GET", route, query, "", "", "127.0.0.1");
            require(response.find("HTTP/1.1 200 OK") == 0, "realtime HTTP status");
            const auto body = response.substr(response.find("\r\n\r\n") + 4);
            return std::make_pair(edge_gateway::json::JsonParser(body).parse(), body);
        };
        const auto check = [&](const JsonValue& snapshot, std::size_t count) {
            const auto& values = field(snapshot, "points").asArray().values;
            require(values.size() == count && field(snapshot, "count").asNumber() == count, "snapshot count");
            for (const auto& value : values) {
                const auto index = static_cast<std::uint32_t>(field(*value, "index").asNumber());
                const auto original = store.getLatestByIndex(index, field(snapshot, "ts").asNumber());
                require(bool(original), "fixture point exists");
                require(field(*value, "expireAt").asNumber() == original->expireAt, "original expiry preserved");
                require(field(*value, "ts").asNumber() == original->ts, "original sample timestamp preserved");
                require(field(*value, "quality").asNumber() == original->quality, "quality preserved");
                require(field(*value, "stale").asBool() == original->stale, "stale flag preserved");
                require(field(*value, "value").asNumber() == original->value, "value preserved");
                require(field(*value, "machineCode").asString() == original->machineCode &&
                        field(*value, "meterCode").asString() == original->meterCode &&
                        field(*value, "pointCode").asString() == original->pointCode, "identity preserved");
            }
        };
        const auto snapshot = request("/api/v1/realtime/points");
        check(snapshot.first, 3);
        check(request("/api/v1/realtime/points", "meterCode=M").first, 3);
        check(request("/api/v1/realtime/points", "meterCode=OTHER").first, 0);
        const auto full = request("/api/v1/telemetry/full");
        check(full.first, 3);
        check(edge_gateway::json::JsonParser(field(full.first, "rawPayload").asString()).parse(), 3);
        config.maxRealtimePoints = 1;
        check(request("/api/v1/realtime/points").first, 1);
        require(store.peekPendingWriteCommands().empty(), "read-only request created a command");
        if (argc == 2) { std::ofstream f(argv[1]); f << snapshot.second << '\n'; require(f.good(), "write synthetic evidence"); }
        cleanup();
        std::cout << "HTTP expiry: long/disabled/expired, full/raw, filter, limit, unchanged fields and zero commands passed\n";
    } catch (...) { cleanup(); throw; }
}
