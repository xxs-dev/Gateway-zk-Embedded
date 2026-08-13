#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "edge_gateway/system_monitor_direct_maintenance.hpp"
#include "edge_gateway/memory_point_store.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

#ifndef _WIN32
void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    require(output.is_open(), "cannot create test config: " + path);
    output << content;
    require(output.good(), "cannot write test config: " + path);
}

int reserveLoopbackPort() {
    const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(socketFd >= 0, "cannot create port reservation socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(
        ::bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "cannot reserve loopback port"
    );
    socklen_t length = sizeof(address);
    require(
        ::getsockname(socketFd, reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "cannot read reserved loopback port"
    );
    const int port = ntohs(address.sin_port);
    ::close(socketFd);
    return port;
}

std::string httpGet(int port, const std::string& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(socketFd >= 0, "cannot create HTTP test socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(socketFd);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const std::string request = "GET " + path +
            " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        require(
            ::send(socketFd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()),
            "cannot send HTTP test request"
        );
        std::string response;
        char buffer[4096];
        while (true) {
            const auto count = ::recv(socketFd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            response.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(socketFd);
        return response;
    }
    throw std::runtime_error("direct maintenance server did not start");
}

void unlinkSharedMemory(const std::string& name) {
    const auto normalized = name.empty() || name.front() == '/' ? name : "/" + name;
    if (::shm_unlink(normalized.c_str()) != 0 && errno != ENOENT) {
        throw std::runtime_error("cannot remove test shared memory: " + std::string(std::strerror(errno)));
    }
}

bool sharedMemoryExists(const std::string& name) {
    const auto normalized = name.empty() || name.front() == '/' ? name : "/" + name;
    const int fd = ::shm_open(normalized.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

void verifyRequiredStoreReadinessIsFailClosed() {
    const auto pid = static_cast<unsigned long long>(::getpid());
    const std::string root = "/tmp/gateway-direct-contract-" + std::to_string(pid);
    const std::string apps = root + "/apps";
    const std::string devices = root + "/devices";
    const std::string appConfig = apps + "/monitor-service.json";
    const std::string deviceConfig = devices + "/primary.json";
    const std::string storeName = "gateway_direct_required_" + std::to_string(pid);
    const std::uint32_t pointIndex = 990001;
    require(::mkdir(root.c_str(), 0700) == 0 || errno == EEXIST, "cannot create test directory");
    require(::mkdir(apps.c_str(), 0700) == 0 || errno == EEXIST, "cannot create test apps directory");
    require(::mkdir(devices.c_str(), 0700) == 0 || errno == EEXIST, "cannot create test devices directory");
    unlinkSharedMemory(storeName);
    writeFile(
        deviceConfig,
        "{\"schemaVersion\":\"1.1.0\",\"machineCode\":\"GW0001\","
        "\"protocol\":{\"type\":\"computed\"},"
        "\"memoryStore\":{\"sharedMemoryName\":\"" + storeName + "\"},"
        "\"meters\":[{\"meterCode\":\"METER_DIRECT_TEST\",\"points\":[{"
        "\"index\":" + std::to_string(pointIndex) + ","
        "\"pointCode\":\"POINT_DIRECT_TEST\",\"name\":\"Direct test point\","
        "\"enabled\":true,\"fullUpload\":true,\"read\":{\"enable\":true},"
        "\"write\":{\"enable\":false}}]}]}\n"
    );
    writeFile(
        appConfig,
        "{\"runtimeMode\":\"gateway\",\"deviceConfigFiles\":[\"" + deviceConfig + "\"],"
        "\"mqtt\":{\"clientId\":\"GW_DIRECT_TEST\"},"
        "\"mqttDriver\":{\"sharedMemoryName\":\"" + storeName +
        "\",\"sharedMemoryNames\":[\"" + storeName + "\"]}}\n"
    );

    edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
    config.enabled = true;
    config.listenHosts = {"127.0.0.1"};
    config.listenPort = reserveLoopbackPort();
    config.appConfigFile = appConfig;

    std::atomic<int> serverResult{-1};
    std::thread server([&]() {
        serverResult = edge_gateway::system_monitor_direct_maintenance::runFromConfig(config);
    });
    std::unique_ptr<edge_gateway::MemoryPointStore> producer;
    try {
        const auto health = httpGet(config.listenPort, "/api/v1/health");
        require(health.find("HTTP/1.1 200 OK") == 0, "direct maintenance health check failed");
        for (const auto* endpoint : {"/api/v1/telemetry/full", "/api/v1/realtime/points"}) {
            const auto response = httpGet(config.listenPort, endpoint);
            require(
                response.find("HTTP/1.1 503 Service Unavailable") == 0,
                std::string(endpoint) + " returned a partial success for a missing required store"
            );
            require(
                response.find(storeName) != std::string::npos,
                std::string(endpoint) + " did not identify the unavailable store: " + response
            );
            require(!sharedMemoryExists(storeName), std::string(endpoint) + " created the missing store");
        }

        producer.reset(new edge_gateway::MemoryPointStore(storeName));
        edge_gateway::PointValue value;
        value.index = pointIndex;
        value.machineCode = "GW0001";
        value.meterCode = "METER_DIRECT_TEST";
        value.pointCode = "POINT_DIRECT_TEST";
        value.value = 42.0;
        value.quality = 1;
        value.ts = 1770000000000LL;
        producer->putLatest(value);
        for (const auto* endpoint : {"/api/v1/telemetry/full", "/api/v1/realtime/points"}) {
            const auto response = httpGet(config.listenPort, endpoint);
            require(
                response.find("HTTP/1.1 200 OK") == 0,
                std::string(endpoint) + " did not recover after the first point value"
            );
            require(
                response.find("\"count\":1") != std::string::npos &&
                    response.find("\"pointCode\":\"POINT_DIRECT_TEST\"") != std::string::npos,
                std::string(endpoint) + " returned an incomplete recovered snapshot"
            );
        }
    } catch (...) {
        edge_gateway::system_monitor_direct_maintenance::requestStop();
        server.join();
        producer.reset();
        unlinkSharedMemory(storeName);
        ::unlink(appConfig.c_str());
        ::unlink(deviceConfig.c_str());
        ::rmdir(apps.c_str());
        ::rmdir(devices.c_str());
        ::rmdir(root.c_str());
        throw;
    }
    edge_gateway::system_monitor_direct_maintenance::requestStop();
    server.join();
    require(serverResult == 0, "direct maintenance server exited with an error");
    producer.reset();
    unlinkSharedMemory(storeName);
    ::unlink(appConfig.c_str());
    ::unlink(deviceConfig.c_str());
    ::rmdir(apps.c_str());
    ::rmdir(devices.c_str());
    ::rmdir(root.c_str());
}

void verifyEnabledCameraUsesDefaultStoreWhenNameIsEmpty() {
    const auto pid = static_cast<unsigned long long>(::getpid());
    const std::string root = "/tmp/gateway-direct-camera-contract-" + std::to_string(pid);
    const std::string apps = root + "/apps";
    const std::string devices = root + "/devices";
    const std::string appConfig = apps + "/monitor-service.json";
    const std::string siblingAppConfig = apps + "/camera-service.json";
    const std::string primaryDeviceConfig = devices + "/primary.json";
    const std::string siblingDeviceConfig = devices + "/sibling.json";
    const std::string primaryStoreName = "gateway_direct_primary_device_" + std::to_string(pid);
    const std::string siblingStoreName = "gateway_direct_sibling_device_" + std::to_string(pid);
    const std::string cameraStoreName = "gateway_point_store";
    const std::uint32_t primaryPointIndex = 990101;
    const std::uint32_t siblingPointIndex = 990102;
    const std::uint32_t cameraPointIndex = 990103;
    require(::mkdir(root.c_str(), 0700) == 0 || errno == EEXIST, "cannot create camera test directory");
    require(::mkdir(apps.c_str(), 0700) == 0 || errno == EEXIST, "cannot create camera apps directory");
    require(::mkdir(devices.c_str(), 0700) == 0 || errno == EEXIST, "cannot create camera devices directory");
    unlinkSharedMemory(primaryStoreName);
    unlinkSharedMemory(siblingStoreName);
    unlinkSharedMemory(cameraStoreName);
    writeFile(
        primaryDeviceConfig,
        "{\"schemaVersion\":\"1.1.0\",\"machineCode\":\"GW0001\","
        "\"protocol\":{\"type\":\"computed\"},"
        "\"memoryStore\":{\"sharedMemoryName\":\"" + primaryStoreName + "\"},"
        "\"meters\":[{\"meterCode\":\"METER_PRIMARY\",\"points\":[{"
        "\"index\":" + std::to_string(primaryPointIndex) + ","
        "\"pointCode\":\"POINT_PRIMARY\",\"name\":\"Primary device point\","
        "\"enabled\":true,\"fullUpload\":true,\"read\":{\"enable\":true},"
        "\"write\":{\"enable\":false}}]}]}\n"
    );
    writeFile(
        siblingDeviceConfig,
        "{\"schemaVersion\":\"1.1.0\",\"machineCode\":\"GW0001\","
        "\"protocol\":{\"type\":\"computed\"},"
        "\"memoryStore\":{\"sharedMemoryName\":\"" + siblingStoreName + "\"},"
        "\"meters\":[{\"meterCode\":\"METER_SIBLING\",\"points\":[{"
        "\"index\":" + std::to_string(siblingPointIndex) + ","
        "\"pointCode\":\"POINT_SIBLING\",\"name\":\"Sibling device point\","
        "\"enabled\":true,\"fullUpload\":true,\"read\":{\"enable\":true},"
        "\"write\":{\"enable\":false}}]}]}\n"
    );
    writeFile(
        appConfig,
        "{\"runtimeMode\":\"gateway\",\"deviceConfigFiles\":[\"" + primaryDeviceConfig + "\"],"
        "\"mqtt\":{\"clientId\":\"GW0001\"},"
        "\"mqttDriver\":{\"sharedMemoryName\":\"" + primaryStoreName +
        "\",\"sharedMemoryNames\":[\"" + primaryStoreName + "\"]}}\n"
    );
    writeFile(
        siblingAppConfig,
        "{\"runtimeMode\":\"gateway\",\"deviceConfigFiles\":[\"" + siblingDeviceConfig + "\"],"
        "\"cameraService\":{\"enabled\":true,\"sharedMemoryName\":\"\","
        "\"cameras\":[{\"cameraCode\":\"CAM_DIRECT_TEST\",\"enabled\":true,"
        "\"statusPointIndexes\":{\"online\":" + std::to_string(cameraPointIndex) + "}}]}}\n"
    );

    edge_gateway::SystemMonitorConfig::DirectMaintenanceConfig config;
    config.enabled = true;
    config.listenHosts = {"127.0.0.1"};
    config.listenPort = reserveLoopbackPort();
    config.appConfigFile = appConfig;

    std::unique_ptr<edge_gateway::MemoryPointStore> primaryProducer;
    std::unique_ptr<edge_gateway::MemoryPointStore> siblingProducer;
    std::unique_ptr<edge_gateway::MemoryPointStore> cameraProducer;
    std::thread server;
    std::atomic<int> serverResult{-1};
    try {
        primaryProducer.reset(new edge_gateway::MemoryPointStore(primaryStoreName));
        siblingProducer.reset(new edge_gateway::MemoryPointStore(siblingStoreName));
        cameraProducer.reset(new edge_gateway::MemoryPointStore(cameraStoreName));

        edge_gateway::PointValue primaryValue;
        primaryValue.index = primaryPointIndex;
        primaryValue.machineCode = "GW0001";
        primaryValue.meterCode = "METER_PRIMARY";
        primaryValue.pointCode = "POINT_PRIMARY";
        primaryValue.value = 21.0;
        primaryValue.quality = 1;
        primaryValue.ts = 1770000000100LL;
        primaryProducer->putLatest(primaryValue);

        edge_gateway::PointValue siblingValue;
        siblingValue.index = siblingPointIndex;
        siblingValue.machineCode = "GW0001";
        siblingValue.meterCode = "METER_SIBLING";
        siblingValue.pointCode = "POINT_SIBLING";
        siblingValue.value = 22.0;
        siblingValue.quality = 1;
        siblingValue.ts = 1770000000150LL;
        siblingProducer->putLatest(siblingValue);

        edge_gateway::PointValue cameraValue;
        cameraValue.index = cameraPointIndex;
        cameraValue.machineCode = "GW0001";
        cameraValue.meterCode = "CAM_DIRECT_TEST";
        cameraValue.pointCode = "camera_online";
        cameraValue.value = 1.0;
        cameraValue.quality = 1;
        cameraValue.ts = 1770000000200LL;
        cameraProducer->putLatest(cameraValue);

        server = std::thread([&]() {
            serverResult = edge_gateway::system_monitor_direct_maintenance::runFromConfig(config);
        });
        const auto health = httpGet(config.listenPort, "/api/v1/health");
        require(health.find("HTTP/1.1 200 OK") == 0, "camera direct maintenance health check failed");
        for (const auto* endpoint : {"/api/v1/telemetry/full", "/api/v1/realtime/points"}) {
            const auto response = httpGet(config.listenPort, endpoint);
            require(
                response.find("HTTP/1.1 200 OK") == 0,
                std::string(endpoint) + " did not open the enabled camera's default store: " + response
            );
            require(
                response.find("\"count\":3") != std::string::npos &&
                    response.find("\"pointCode\":\"POINT_PRIMARY\"") != std::string::npos &&
                    response.find("\"pointCode\":\"POINT_SIBLING\"") != std::string::npos &&
                    response.find("\"pointCode\":\"camera_online\"") != std::string::npos,
                std::string(endpoint) + " did not return primary, sibling, and camera points"
            );
        }
    } catch (...) {
        edge_gateway::system_monitor_direct_maintenance::requestStop();
        if (server.joinable()) {
            server.join();
        }
        cameraProducer.reset();
        siblingProducer.reset();
        primaryProducer.reset();
        unlinkSharedMemory(cameraStoreName);
        unlinkSharedMemory(siblingStoreName);
        unlinkSharedMemory(primaryStoreName);
        ::unlink(appConfig.c_str());
        ::unlink(siblingAppConfig.c_str());
        ::unlink(primaryDeviceConfig.c_str());
        ::unlink(siblingDeviceConfig.c_str());
        ::rmdir(apps.c_str());
        ::rmdir(devices.c_str());
        ::rmdir(root.c_str());
        throw;
    }
    edge_gateway::system_monitor_direct_maintenance::requestStop();
    server.join();
    require(serverResult == 0, "camera direct maintenance server exited with an error");
    cameraProducer.reset();
    siblingProducer.reset();
    primaryProducer.reset();
    unlinkSharedMemory(cameraStoreName);
    unlinkSharedMemory(siblingStoreName);
    unlinkSharedMemory(primaryStoreName);
    ::unlink(appConfig.c_str());
    ::unlink(siblingAppConfig.c_str());
    ::unlink(primaryDeviceConfig.c_str());
    ::unlink(siblingDeviceConfig.c_str());
    ::rmdir(apps.c_str());
    ::rmdir(devices.c_str());
    ::rmdir(root.c_str());
}
#endif

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
#ifndef _WIN32
        verifyRequiredStoreReadinessIsFailClosed();
        verifyEnabledCameraUsesDefaultStoreWhenNameIsEmpty();
#endif
        std::cout << "system_monitor_direct_maintenance_contract_test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "system_monitor_direct_maintenance_contract_test failed: " << ex.what() << "\n";
        return 1;
    }
}
