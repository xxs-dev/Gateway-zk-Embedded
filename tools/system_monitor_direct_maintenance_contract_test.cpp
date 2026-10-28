#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/json_value.hpp"
#include "edge_gateway/memory_point_store.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

void verifyDisabledCameraDoesNotCreateStore() {
    char directory[] = "/tmp/gateway-realtime-camera-XXXXXX";
    require(::mkdtemp(directory) != nullptr, "private realtime fixture directory");
    const std::string root(directory);
    const auto suffix = root.substr(root.find_last_of('/') + 1);
    const auto primary = suffix + "-primary";
    const auto camera = suffix + "-camera";
    const auto app = root + "/app.json";
    const auto cleanup = [&] {
        ::shm_unlink(("/" + primary).c_str());
        ::shm_unlink(("/" + camera).c_str());
        std::remove(app.c_str());
        ::rmdir(root.c_str());
    };
    try {
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
        config.appConfigFile = app;
        config.scadaUpperComputerSafetyEnabled = false;
        for (const bool enabled : {false, true}) {
            {
                std::ofstream file(app);
                file << "{\"deviceConfigFiles\":[],\"identityConfigFile\":\"\","
                     << "\"mqttDriver\":{\"sharedMemoryName\":\"" << primary << "\"},"
                     << "\"cameraService\":{\"enabled\":" << (enabled ? "true" : "false")
                     << ",\"sharedMemoryName\":\"" << camera << "\"}}";
                require(file.good(), "write realtime fixture app");
            }
            const auto response = edge_gateway::system_monitor_direct_maintenance::handleHttpRequest(
                config, "GET", "/api/v1/realtime/points", "", "", "", "127.0.0.1");
            require(startsWith(response, "HTTP/1.1 200"), "real realtime handler must succeed");
            require(::access(("/dev/shm/" + primary).c_str(), F_OK) == 0,
                    "realtime handler must open configured primary store");
            const bool cameraExists = ::access(("/dev/shm/" + camera).c_str(), F_OK) == 0;
            require(cameraExists == enabled, "disabled camera must not create a shared memory store");
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}

void verifyFullTelemetryRequiresExistingStores() {
    char directory[] = "/tmp/gateway-full-telemetry-XXXXXX";
    require(::mkdtemp(directory) != nullptr, "private full telemetry fixture");
    const std::string root(directory);
    const auto name = root.substr(root.find_last_of('/') + 1);
    const auto app = root + "/monitor.json";
    const auto device = root + "/device.cfg";
    const auto identity = root + "/device_identity.json";
    const auto cleanup = [&] {
        ::shm_unlink(("/" + name).c_str());
        std::remove(app.c_str());
        std::remove(device.c_str());
        std::remove(identity.c_str());
        ::rmdir(root.c_str());
    };
    try {
        { std::ofstream f(identity); f << "{\"machineCode\":\"GW_TEST\"}"; }
        { std::ofstream f(device);
          f << "{\"machineCode\":\"GW_TEST\",\"protocol\":{\"type\":\"computed\"},"
               "\"memoryStore\":{\"sharedMemoryName\":\"" << name << "\"},"
               "\"meters\":[{\"meterCode\":\"TEST\",\"points\":[{\"index\":990001,"
               "\"pointCode\":\"FULL_TEST\",\"enabled\":true,\"fullUpload\":true,"
               "\"read\":{\"enable\":true},\"write\":{\"enable\":false}}]}]}"; }
        { std::ofstream f(app);
          f << "{\"identityConfigFile\":\"" << identity << "\",\"deviceConfigFiles\":[\"" << device << "\"],"
               "\"mqttDriver\":{\"sharedMemoryName\":\"" << name << "\"}}"; }
        edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
        config.appConfigFile = app;
        config.scadaUpperComputerSafetyEnabled = false;
        const auto request = [&] {
            return edge_gateway::system_monitor_direct_maintenance::handleHttpRequest(
                config, "GET", "/api/v1/telemetry/full", "", "", "", "127.0.0.1");
        };
        const auto missing = request();
        require(!startsWith(missing, "HTTP/1.1 200"), "missing store must reject a full snapshot");
        require(::access(("/dev/shm/" + name).c_str(), F_OK) != 0,
                "full snapshot must not create a missing producer store");
        {
            edge_gateway::MemoryPointStore store(name);
            edge_gateway::PointDefinition point;
            point.index = 990001;
            point.pointCode = "FULL_TEST";
            point.enabled = point.fullUpload = true;
            point.read.cachePolicy.storeLatest = true;
            store.registerPoint("GW_TEST", "TEST", point);
            edge_gateway::PointValue value;
            value.index = point.index;
            value.machineCode = "GW_TEST";
            value.meterCode = "TEST";
            value.pointCode = point.pointCode;
            value.value = 42;
            value.quality = 1;
            value.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            store.putLatest(value);
            const auto present = request();
            require(startsWith(present, "HTTP/1.1 200") && present.find("FULL_TEST") != std::string::npos,
                    "existing producer must appear in the full snapshot: " + present);
        }
        ::shm_unlink(("/" + name).c_str());
        require(!startsWith(request(), "HTTP/1.1 200"), "removed producer must not yield successful empty snapshot");
    } catch (...) { cleanup(); throw; }
    cleanup();
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
                             const std::string& password, const std::string& root, bool ownerMode) {
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
#ifdef EDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS
    if (ownerMode) {
        dm::setDirectDiagFaultForTest(true, false);
        const auto signalled = send(prefix + "\"cmdId\":\"DIRECT_LEADER_SIGNAL\",\"command\":\"free_m\"}");
        require(startsWith(signalled, "HTTP/1.1 200") &&
                signalled.find("\"reason\":\"signal\"") != std::string::npos &&
                signalled.find("\"signal\":15") != std::string::npos &&
                signalled.find("\"exitCode\":143") != std::string::npos &&
                signalled.find("\"cleanupConfirmed\":true") != std::string::npos,
                "owned command leader signal must be classified from wait status");
        dm::setDirectDiagFaultForTest(false, true);
        const auto unknown = send(prefix + "\"cmdId\":\"DIRECT_UNKNOWN\",\"command\":\"uptime\"}");
        require(unknown.find("\"reason\":\"cleanup_unknown\"") != std::string::npos &&
                unknown.find("\"cleanupConfirmed\":false") != std::string::npos &&
                dm::directDiagOwnerActiveForTest() && dm::directDiagOwnerUnavailableForTest(),
                "early unknown must retain a poisoned live owner");
        const auto refused = send(prefix + "\"cmdId\":\"DIRECT_REFUSED\",\"command\":\"uptime\"}");
        require(startsWith(refused, "HTTP/1.1 503") &&
                refused.find("\"reason\":\"cleanup_unknown\"") != std::string::npos &&
                refused.find("\"cmdId\":") == std::string::npos,
                "uncertain owner must reject new admission without terminal cmdId");
        const int portProbe = ::socket(AF_INET, SOCK_STREAM, 0);
        require(portProbe >= 0, "owner loop private port allocation");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(portProbe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "owner loop private port bind");
        socklen_t addressSize = sizeof(address);
        require(::getsockname(portProbe, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0,
                "owner loop private port query");
        auto serverConfig = config;
        serverConfig.enabled = true;
        serverConfig.listenHosts = {"127.0.0.1"};
        serverConfig.listenPort = ntohs(address.sin_port);
        ::close(portProbe);
        int serverExit = -1;
        std::thread server([&] { serverExit = dm::runFromConfig(serverConfig); });
        try {
            const auto health = loopbackRequest(serverConfig.listenPort,
                "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
            require(startsWith(health, "HTTP/1.1 200"), "owner recovery server health");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (dm::directDiagOwnerActiveForTest() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!dm::directDiagOwnerActiveForTest() && dm::directDiagOwnerUnavailableForTest(),
                    "server loop must reap retained owner without reopening admission");
            require(startsWith(send(prefix + "\"cmdId\":\"DIRECT_STILL_REFUSED\",\"command\":\"uptime\"}"),
                               "HTTP/1.1 503"), "poisoned admission reopened after owner reap");
        } catch (...) { dm::requestStop(); server.join(); throw; }
        dm::requestStop();
        server.join();
        require(serverExit == 0, "owner recovery server did not stop cleanly");
        require(::setenv("PATH", priorPath.c_str(), 1) == 0, "restore owner fixture PATH");
        for (const auto& name : {"uptime", "journalctl", "top", "free"}) std::remove((bin + "/" + name).c_str());
        std::remove(counter.c_str());
        require(::rmdir(bin.c_str()) == 0, "owner fixture cleanup");
        return;
    }
#else
    (void)ownerMode;
#endif
    require(startsWith(send("{\"passwordSha256\":\"wrong\",\"machineCode\":\"GW_TEST\",\"cmdId\":\"UNAUTHORIZED\",\"command\":\"uptime\"}"),
                       "HTTP/1.1 401"), "direct diagnostic authentication changed");
    require(startsWith(send(std::string("{\"passwordSha256\":\"") + password +
        "\",\"machineCode\":\"WRONG\",\"cmdId\":\"WRONG_NODE\",\"command\":\"uptime\"}"), "HTTP/1.1 400"),
        "direct diagnostic must bind loaded identity");
    const auto request = prefix + "\"cmdId\":\"DIRECT_ONE\",\"command\":\"uptime\"}";
    const auto first = send(request);
    require(startsWith(first, "HTTP/1.1 200") && first.find("\"reason\":\"exit\"") != std::string::npos &&
            first.find("\"arg\":\"\"") != std::string::npos &&
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
    script("free", "printf '\\344\\270\\255\\377'\n");
    const auto utf8 = send(prefix + "\"cmdId\":\"DIRECT_UTF8\",\"command\":\"free_m\"}");
    const auto bodyAt = utf8.find("\r\n\r\n");
    require(bodyAt != std::string::npos, "direct UTF-8 reply missing HTTP body");
    const auto parsedUtf8 = edge_gateway::json::JsonParser(utf8.substr(bodyAt + 4), 16, 64).parse();
    const auto* utf8Output = parsedUtf8.find("stdout");
    require(utf8Output && utf8Output->asString() == std::string("\xe4\xb8\xad\xef\xbf\xbd", 6),
            "direct diagnostic invalid UTF-8 did not become valid JSON");
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
    for (int i = 0; i < 10; ++i) {
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

void verifyProductionConfigGate(const edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig& base,
        const std::string& passwordSha256) {
    namespace dm = edge_gateway::system_monitor_direct_maintenance;
    char privateRoot[] = "/tmp/direct-production-config-XXXXXX";
    require(::mkdtemp(privateRoot) != nullptr, "private direct config root creation failed");
    const std::string root = privateRoot;
    const auto write = [](const std::string& path, const std::string& content) {
        std::ofstream file(path);
        require(file.is_open(), "private config file creation failed");
        file << content;
    };
    const auto read = [](const std::string& path) {
        std::ifstream file(path);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto entries = [](const std::string& directory) {
        DIR* dir = ::opendir(directory.c_str());
        require(dir != nullptr, "private config inventory failed");
        std::vector<std::string> names;
        while (auto* entry = ::readdir(dir)) {
            if (std::string(entry->d_name) != "." && entry->d_name != std::string(".."))
                names.emplace_back(entry->d_name);
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());
        return names;
    };
    for (const auto& name : {"ordinary", "production"}) {
        const auto home = root + "/" + name;
        const auto runtime = home + "/config/runtime";
        const auto apps = runtime + "/apps";
        const auto devices = runtime + "/devices";
        require(::mkdir(home.c_str(), 0700) == 0 && ::mkdir((home + "/config").c_str(), 0700) == 0 &&
                ::mkdir(runtime.c_str(), 0700) == 0 && ::mkdir(apps.c_str(), 0700) == 0 &&
                ::mkdir(devices.c_str(), 0700) == 0 && ::mkdir((home + "/data").c_str(), 0700) == 0,
                "private direct runtime creation failed");
        if (std::string(name) == "production")
            require(::mkdir((home + "/data/runtime-production").c_str(), 0700) == 0,
                    "private production journal marker creation failed");
        const auto identity = runtime + "/device_identity.json";
        const auto app = apps + "/monitor-service.json";
        const auto device = devices + "/meter_1.json";
        write(identity, "{\"machineCode\":\"GW_TEST\"}\n");
        write(app, "{\"runtimeMode\":\"gateway\",\"identityConfigFile\":\"" + identity +
            "\",\"deviceConfigFiles\":[\"" + device + "\"]}\n");
        const std::string original = "{\"machineCode\":\"GW_TEST\",\"protocol\":{\"type\":\"modbus_tcp\"},\"meters\":[]}";
        const std::string changed = "{\"machineCode\":\"GW_TEST\",\"deviceName\":\"changed\",\"protocol\":{\"type\":\"modbus_tcp\"},\"meters\":[]}";
        write(device, original);
        auto config = base;
        config.identityConfigFile = identity;
        config.appConfigFile = app;
        config.otaAppConfigFile = app;
        const auto send = [&](const std::string& route, const std::string& body) {
            return dm::handleHttpRequest(config, "POST", route, "", "", body, "10.0.0.6");
        };
        const auto prefix = std::string("{\"passwordSha256\":\"") + passwordSha256 +
            "\",\"requestId\":\"CFG_PRIVATE\",\"machineCode\":\"GW_TEST\",";
        const auto apply = prefix + "\"dryRun\":false,\"files\":[{\"path\":\"" + device +
            "\",\"encoding\":\"utf8\",\"content\":\"{\\\"machineCode\\\":\\\"GW_TEST\\\",\\\"deviceName\\\":\\\"changed\\\",\\\"protocol\\\":{\\\"type\\\":\\\"modbus_tcp\\\"},\\\"meters\\\":[]}\"}]}";
        const auto operation = prefix + "\"dryRun\":false,\"path\":\"" + device + "\"}";
        if (std::string(name) == "ordinary") {
            const auto applied = send("/api/v1/config/apply", apply);
            require(startsWith(applied, "HTTP/1.1 200") &&
                    applied.find("\"success\":true") != std::string::npos && read(device) == changed,
                    "ordinary direct apply did not write the valid device config");
            const auto deleted = send("/api/v1/config/delete", operation);
            require(startsWith(deleted, "HTTP/1.1 200") &&
                    deleted.find("\"success\":true") != std::string::npos &&
                    ::access(device.c_str(), F_OK) != 0 && entries(devices).size() == 2,
                    "ordinary direct apply/delete did not create two recoverable backups");
            const auto restored = send("/api/v1/config/restore", operation);
            require(startsWith(restored, "HTTP/1.1 200") &&
                    restored.find("\"success\":true") != std::string::npos && read(device) == changed,
                    "ordinary direct restore did not recover the exact backed-up config");
        } else {
            const auto backup = device + ".bak.9999999999999";
            write(backup, changed);
            const auto before = entries(devices);
            const auto appBefore = entries(apps);
            const auto runtimeBefore = entries(runtime);
            const auto identityBefore = read(identity);
            const auto appContentBefore = read(app);
            const auto backupBefore = read(backup);
            for (const auto& operationRequest : {
                    std::make_pair("/api/v1/config/apply", apply),
                    std::make_pair("/api/v1/config/delete", operation),
                    std::make_pair("/api/v1/config/restore", operation)}) {
                const auto blocked = send(operationRequest.first, operationRequest.second);
                require(startsWith(blocked, "HTTP/1.1 400") &&
                        blocked.find("production config mutation requires an approved provision transaction") != std::string::npos &&
                        read(device) == original && read(backup) == backupBefore &&
                        read(identity) == identityBefore && read(app) == appContentBefore &&
                        entries(devices) == before && entries(apps) == appBefore &&
                        entries(runtime) == runtimeBefore,
                        "production direct mutation changed a valid config or backup inventory");
            }
            auto dryRun = apply;
            const auto flag = dryRun.find("\"dryRun\":false");
            require(flag != std::string::npos, "private dry-run fixture invalid");
            dryRun.replace(flag, std::string("\"dryRun\":false").size(), "\"dryRun\":true");
            const auto checked = send("/api/v1/config/apply", dryRun);
            require(startsWith(checked, "HTTP/1.1 200") &&
                    checked.find("\"success\":true") != std::string::npos &&
                    read(device) == original && read(backup) == backupBefore && entries(devices) == before,
                    "production direct dry-run should validate without writing");
            const auto snapshot = send("/api/v1/config/snapshot", prefix + "\"scope\":\"config\"}");
            require(startsWith(snapshot, "HTTP/1.1 200") &&
                    snapshot.find("\"success\":true") != std::string::npos &&
                    snapshot.find(device) != std::string::npos,
                    "production direct read-only snapshot was blocked");
        }
    }
}

void verifyMaintenanceCredential(bool ownerMode) {
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
    verifyProductionConfigGate(config, good);
    verifyDirectDiagnostics(config, good, root, ownerMode);
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

int main(int argc, char** argv) {
    try {
        verifyDisabledCameraDoesNotCreateStore();
        verifyFullTelemetryRequiresExistingStores();
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
        verifyMaintenanceCredential(argc == 2 && std::string(argv[1]) == "--owner");
        std::cout << "system_monitor_direct_maintenance_contract_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "system_monitor_direct_maintenance_contract_test failed: " << ex.what() << "\n";
        return 1;
    }
}
