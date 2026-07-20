#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/scada_control_lease.hpp"
#include "edge_gateway/scada_upper_computer_safety.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write test file: " + path);
    output << content;
}

void makeDirectory(const std::string& path) {
#ifndef _WIN32
    (void)::mkdir(path.c_str(), 0700);
#else
    (void)path;
#endif
}

void removeTree(const std::string& path) {
#ifndef _WIN32
    const auto result = std::system(("rm -rf '" + path + "'").c_str());
    if (result != 0) std::cerr << "warning: failed to remove " << path << std::endl;
#else
    (void)path;
#endif
}

edge_gateway::DeviceConfig deviceConfig(const std::string& storeName) {
    edge_gateway::DeviceConfig config;
    config.machineCode = "COMM202600999";
    config.meterCode = "PCS001";
    config.memoryStore.sharedMemoryName = storeName;
    edge_gateway::PointDefinition point;
    point.index = 101;
    point.pointCode = "POWER_SET";
    point.enabled = true;
    point.read.enable = true;
    point.write.enable = true;
    config.points.push_back(point);
    return config;
}

void createProject(const std::string& root, const std::string& storeName) {
    makeDirectory(root);
    writeFile(root + "/manifest.json", R"({
      "schemaVersion":"2.0","projectId":"upper-safe","projectName":"Upper Safe",
      "packageVersion":"1.0.0","entryScreen":"","packageRole":"edgeNode"
    })");
    writeFile(root + "/topology.json", R"({
      "mode":"upperComputer","scadaHost":"windows","emsHost":"windows",
      "dataTransport":"directPreferredMqttFallback","offlinePolicy":"edgeSafeState",
      "upperComputerOfflinePolicy":{
        "timeoutMs":1000,"action":"executeConfiguredActions",
        "retainLocalSafetyRules":true,"requireFreshLeaseForControl":true,
        "safetyActions":[
          {"actionId":"safe-power","nodeId":"edge-a","tagId":"pcs.setpoint","value":0,"highPriority":true}
        ]
      }
    })");
    writeFile(root + "/nodes.json", R"([
      {"nodeId":"edge-a","machineCode":"COMM202600999","displayName":"Edge A","roles":["control","safety"]}
    ])");
    writeFile(root + "/tags.json", R"([
      {"tagId":"pcs.setpoint","nodeId":"edge-a","deviceId":"pcs-1","meterCode":"PCS001",
       "pointCode":"POWER_SET","semanticRole":"pcs.powerSetpoint","displayName":"Power Setpoint",
       "unit":"kW","dataType":"float64","access":"readWrite","indexFallback":101}
    ])");
    writeFile(root + "/runtime-map.json", std::string(R"([
      {"nodeId":"edge-a","tagId":"pcs.setpoint","sharedMemoryName":")") + storeName +
        R"(","index":101,"writable":true,"dataType":"float64","unit":"kW"}
    ])");
}

void verifyTimeoutTriggersExplicitActionOnce() {
    const auto root = std::string("/tmp/gateway-scada-safety-test-") + std::to_string(std::rand());
    const auto project = root + "/project";
    const auto lease = root + "/run/lease.json";
    const std::string storeName = "gateway_scada_safety_test";
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    makeDirectory(root);
    createProject(project, storeName);
    try {
        edge_gateway::MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = storeName;
        storeConfig.maxLatestPoints = 16;
        edge_gateway::MemoryPointStore store(storeConfig);
        const auto device = deviceConfig(storeName);
        store.registerDevicePoints({device});

        edge_gateway::PointStoreRouter router;
        router.addStore(storeName, store);
        router.addRoutesFromDeviceConfigs({device}, storeName);

        edge_gateway::SystemMonitorConfig::ScadaUpperComputerSafetyConfig config;
        config.enabled = true;
        config.projectDirectory = project;
        config.leaseFile = lease;
        config.reloadIntervalMs = 1000;
        edge_gateway::ScadaUpperComputerSafetyMonitor monitor(config, "COMM202600999", router);

        const auto armed = monitor.runOnce(1000);
        require(armed.active && armed.heartbeatFresh && !armed.triggered, "activation grace period should be fresh");
        require(store.peekPendingWriteCommands().empty(), "safety action fired before timeout");

        const auto triggered = monitor.runOnce(2001);
        require(triggered.triggered && triggered.completed, "offline safety action was not submitted");
        const auto pending = store.peekPendingWriteCommands();
        require(pending.size() == 1, "offline safety action should enqueue exactly once");
        require(pending.front().index == 101 && pending.front().value == 0.0, "offline safety action route/value mismatch");
        require(pending.front().highPriority, "offline safety action must preserve high priority");

        const auto repeated = monitor.runOnce(2500);
        require(!repeated.triggered && repeated.completed, "completed safety action was submitted twice");
        require(store.peekPendingWriteCommands().size() == 1, "duplicate offline safety command was queued");
    } catch (...) {
        removeTree(root);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        throw;
    }
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyHeartbeatExtendsLease() {
    const auto root = std::string("/tmp/gateway-scada-safety-heartbeat-") + std::to_string(std::rand());
    const auto project = root + "/project";
    const auto lease = root + "/run/lease.json";
    const std::string storeName = "gateway_scada_safety_heartbeat_test";
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    makeDirectory(root);
    createProject(project, storeName);
    try {
        edge_gateway::MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = storeName;
        storeConfig.maxLatestPoints = 16;
        edge_gateway::MemoryPointStore store(storeConfig);
        const auto device = deviceConfig(storeName);
        store.registerDevicePoints({device});
        edge_gateway::PointStoreRouter router;
        router.addStore(storeName, store);
        router.addRoutesFromDeviceConfigs({device}, storeName);

        edge_gateway::SystemMonitorConfig::ScadaUpperComputerSafetyConfig config;
        config.enabled = true;
        config.projectDirectory = project;
        config.leaseFile = lease;
        config.reloadIntervalMs = 1000;
        edge_gateway::ScadaUpperComputerSafetyMonitor monitor(config, "COMM202600999", router);
        (void)monitor.runOnce(1000);
        std::string leaseMessage;
        require(
            !edge_gateway::ScadaControlLease::isFresh(
                config.projectDirectory, config.leaseFile, "COMM202600999", 1500, &leaseMessage),
            "missing heartbeat should reject an upper-computer control"
        );
        edge_gateway::ScadaUpperComputerSafetyMonitor::writeHeartbeat(lease, "COMM202600999", 1800);
        require(
            edge_gateway::ScadaControlLease::isFresh(
                config.projectDirectory, config.leaseFile, "COMM202600999", 2500, &leaseMessage),
            "fresh heartbeat should permit an upper-computer control"
        );
        const auto fresh = monitor.runOnce(2500);
        require(fresh.active && fresh.heartbeatFresh && !fresh.triggered, "heartbeat did not extend the safety lease");
        require(store.peekPendingWriteCommands().empty(), "fresh heartbeat still triggered safety action");
        require(
            !edge_gateway::ScadaControlLease::isFresh(
                config.projectDirectory, config.leaseFile, "COMM202600999", 3001, &leaseMessage),
            "expired heartbeat should reject an upper-computer control"
        );
    } catch (...) {
        removeTree(root);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        throw;
    }
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

}  // namespace

int main() {
    try {
        verifyTimeoutTriggersExplicitActionOnce();
        verifyHeartbeatExtendsLease();
        std::cout << "scada_upper_computer_safety_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "scada_upper_computer_safety_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
