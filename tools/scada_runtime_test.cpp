#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/scada_project_loader.hpp"
#include "edge_gateway/scada_runtime_map.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void makeDirectory(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
}

void removeTree(const std::string& path) {
#ifdef _WIN32
    if (std::system(("rmdir /s /q \"" + path + "\"").c_str()) != 0) {
        std::cerr << "warning: failed to remove test directory " << path << std::endl;
    }
#else
    if (std::system(("rm -rf '" + path + "'").c_str()) != 0) {
        std::cerr << "warning: failed to remove test directory " << path << std::endl;
    }
#endif
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create test file: " + path);
    output << content;
}

std::string createProjectDirectory(bool duplicateRoute = false) {
    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    const auto root = std::string("/tmp/gateway-scada-test-") + unique;
    makeDirectory(root);
    makeDirectory(root + "/screens");
    writeFile(root + "/manifest.json", R"({
      "schemaVersion":"2.0",
      "projectId":"site-a",
      "projectName":"\u50a8\u80fd\u7ad9",
      "packageVersion":"1.0.0",
      "entryScreen":"overview",
      "packageRole":"project"
    })");
    writeFile(root + "/topology.json", R"({
      "mode":"integrated",
      "scadaHost":"edge",
      "emsHost":"edge",
      "dataTransport":"sharedMemory",
      "offlinePolicy":"continueLocal"
    })");
    writeFile(root + "/nodes.json", R"([
      {"nodeId":"edge-a","machineCode":"COMM202600999","displayName":"Edge A","roles":["acquisition","control","safety"]}
    ])");
    writeFile(root + "/tags.json", R"([
      {"tagId":"pcs.power","nodeId":"edge-a","deviceId":"pcs-1","meterCode":"PCS001","pointCode":"ACTIVE_POWER","semanticRole":"pcs.activePower","displayName":"Power","unit":"kW","dataType":"float64","access":"read","indexFallback":100},
      {"tagId":"pcs.setpoint","nodeId":"edge-a","deviceId":"pcs-1","meterCode":"PCS001","pointCode":"POWER_SET","semanticRole":"pcs.powerSetpoint","displayName":"Setpoint","unit":"kW","dataType":"float64","access":"readWrite","indexFallback":101}
    ])");
    const auto duplicate = duplicateRoute
        ? R"(,{"nodeId":"edge-a","tagId":"pcs.setpoint","sharedMemoryName":"gateway_scada_runtime_test","index":100,"writable":true,"dataType":"float64","unit":"kW"})"
        : R"(,{"nodeId":"edge-a","tagId":"pcs.setpoint","sharedMemoryName":"gateway_scada_runtime_test","index":101,"writable":true,"dataType":"float64","unit":"kW"})";
    writeFile(root + "/runtime-map.json", std::string(R"([
      {"nodeId":"edge-a","tagId":"pcs.power","sharedMemoryName":"gateway_scada_runtime_test","index":100,"writable":false,"dataType":"float64","unit":"kW"}
    )") + duplicate + "]");
    writeFile(root + "/screens/overview.json", R"({
      "screenId":"overview",
      "title":"Overview",
      "width":1920,
      "height":1080,
      "widgets":[
        {"widgetId":"power-card","type":"metricCard","title":"Power","geometry":{"x":10,"y":20,"width":220,"height":90},"zIndex":1,"visible":true,"bindings":[{"nodeId":"edge-a","tagId":"pcs.power","slot":"value"}],"properties":{"qtTextColor":"#20DBE9"}}
      ]
    })");
    return root;
}

edge_gateway::DeviceConfig buildDeviceConfig(const std::string& storeName) {
    edge_gateway::DeviceConfig config;
    config.machineCode = "COMM202600999";
    config.meterCode = "PCS001";
    config.deviceName = "PCS";
    config.memoryStore.sharedMemoryName = storeName;

    edge_gateway::PointDefinition power;
    power.index = 100;
    power.pointCode = "ACTIVE_POWER";
    power.enabled = true;
    power.read.enable = true;
    power.read.cachePolicy.storeLatest = true;
    power.read.cachePolicy.ttlMs = 600000;

    edge_gateway::PointDefinition setpoint = power;
    setpoint.index = 101;
    setpoint.pointCode = "POWER_SET";
    setpoint.write.enable = true;
    config.points = {power, setpoint};
    return config;
}

void verifyLoadResolveReadAndWrite() {
    const auto root = createProjectDirectory(false);
    const std::string storeName = "gateway_scada_runtime_test";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
    try {
        const auto project = edge_gateway::ScadaProjectLoader::loadFromDirectory(root);
        require(project.manifest.projectName == "\xE5\x82\xA8\xE8\x83\xBD\xE7\xAB\x99", "unicode project name mismatch");
        require(project.screens.size() == 1, "screen should load");

        edge_gateway::MemoryStoreConfig storeConfig;
        storeConfig.sharedMemoryName = storeName;
        storeConfig.maxLatestPoints = 32;
        edge_gateway::MemoryPointStore store(storeConfig);
        const auto device = buildDeviceConfig(storeName);
        store.registerDevicePoints({device});

        edge_gateway::PointStoreRouter router;
        router.addStore(storeName, store);
        router.addRoutesFromDeviceConfigs({device}, storeName);

        edge_gateway::PointValue power;
        power.index = 100;
        power.machineCode = "COMM202600999";
        power.meterCode = "PCS001";
        power.pointCode = "ACTIVE_POWER";
        power.value = 12.5;
        power.quality = 1;
        power.ts = 1000;
        power.expireAt = 601000;
        require(router.putLatestByIndex(power).accepted, "failed to seed power value");

        edge_gateway::ScadaRuntimeMap runtime(project, "COMM202600999", router);
        const auto bySemantic = runtime.resolver().resolveSemantic("pcs-1", "pcs.activePower");
        require(static_cast<bool>(bySemantic) && bySemantic->mapping.index == 100, "semantic resolution failed");
        const auto values = runtime.readScreen(project.screens.front(), 1000);
        require(values.size() == 1 && values.front().value == 12.5, "screen batch read failed");

        edge_gateway::PendingWriteCommand command;
        command.cmdId = "SCADA_TEST_1";
        command.source = "scada-test";
        command.value = 10.0;
        command.ts = 1000;
        const auto readOnly = runtime.submitWrite("pcs.power", command);
        require(!readOnly.accepted && readOnly.message == "SCADA tag is read-only", "read-only write should fail");
        const auto writable = runtime.submitWrite("pcs.setpoint", command);
        require(writable.accepted, "writable SCADA command should reach PointStoreRouter");
        const auto pending = store.peekPendingWriteCommands();
        require(pending.size() == 1 && pending.front().index == 101, "SCADA write index mismatch");
    } catch (...) {
        removeTree(root);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
        throw;
    }
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeName);
}

void verifyDuplicateRuntimeRouteRejected() {
    const auto root = createProjectDirectory(true);
    try {
        bool rejected = false;
        try {
            (void)edge_gateway::ScadaProjectLoader::loadFromDirectory(root);
        } catch (const std::runtime_error& ex) {
            rejected = std::string(ex.what()).find("duplicate SCADA runtime route") != std::string::npos;
        }
        require(rejected, "duplicate runtime route should be rejected");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void verifyWritableMappingAndStateRuleValidation() {
    const auto root = createProjectDirectory(false);
    try {
        const auto baseline = edge_gateway::ScadaProjectLoader::loadFromDirectory(root);

        auto invalidMapping = baseline;
        invalidMapping.runtimeMappings[1].writable = false;
        bool mappingRejected = false;
        try {
            edge_gateway::ScadaProjectLoader::validate(invalidMapping);
        } catch (const std::runtime_error& ex) {
            mappingRejected = std::string(ex.what()).find("writable SCADA tag") != std::string::npos;
        }
        require(mappingRejected, "read-only runtime mapping for writable tag should fail");

        auto invalidRule = baseline;
        edge_gateway::ScadaStateRule rule;
        rule.code = "running";
        edge_gateway::ScadaStateCondition condition;
        condition.nodeId = "edge-a";
        condition.tagId = "pcs.power";
        condition.comparison = "contains";
        condition.value = "12";
        rule.conditions.push_back(condition);
        invalidRule.screens[0].widgets[0].stateRules.push_back(rule);
        bool ruleRejected = false;
        try {
            edge_gateway::ScadaProjectLoader::validate(invalidRule);
        } catch (const std::runtime_error& ex) {
            ruleRejected = std::string(ex.what()).find("state rule comparison") != std::string::npos;
        }
        require(ruleRejected, "invalid state rule comparison should fail");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

void verifySameIndexAcrossIndependentStores() {
    const auto root = createProjectDirectory(false);
    const std::string storeAName = "gateway_scada_duplicate_a";
    const std::string storeBName = "gateway_scada_duplicate_b";
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeAName);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeBName);
    try {
        edge_gateway::MemoryStoreConfig storeAConfig;
        storeAConfig.sharedMemoryName = storeAName;
        storeAConfig.maxLatestPoints = 16;
        edge_gateway::MemoryStoreConfig storeBConfig = storeAConfig;
        storeBConfig.sharedMemoryName = storeBName;
        edge_gateway::MemoryPointStore storeA(storeAConfig);
        edge_gateway::MemoryPointStore storeB(storeBConfig);

        auto deviceA = buildDeviceConfig(storeAName);
        auto deviceB = buildDeviceConfig(storeBName);
        deviceA.points.resize(1);
        deviceB.points.resize(1);
        deviceA.meterCode = "EMS_CORE";
        deviceB.meterCode = "PCS001";
        storeA.registerDevicePoints({deviceA});
        storeB.registerDevicePoints({deviceB});

        edge_gateway::PointStoreRouter router;
        router.addStore(storeAName, storeA);
        router.addStore(storeBName, storeB);
        router.addRoutesFromDeviceConfigs({deviceA, deviceB}, storeAName);
        require(static_cast<bool>(router.routeByLocation(storeAName, 100)), "store A route is missing");
        require(static_cast<bool>(router.routeByLocation(storeBName, 100)), "store B route is missing");

        edge_gateway::PointValue valueA;
        valueA.index = 100;
        valueA.value = 11.0;
        valueA.quality = 1;
        valueA.ts = 1000;
        valueA.expireAt = 601000;
        storeA.putLatest(valueA);
        auto valueB = valueA;
        valueB.value = 22.0;
        storeB.putLatest(valueB);

        auto project = edge_gateway::ScadaProjectLoader::loadFromDirectory(root);
        project.runtimeMappings.front().sharedMemoryName = storeBName;
        edge_gateway::ScadaRuntimeMap runtime(project, "COMM202600999", router);
        const auto selected = runtime.readTag("pcs.power", 1000);
        require(static_cast<bool>(selected) && selected->value == 22.0,
            "SCADA did not read the route from its configured shared memory");

        auto duplicate = *router.routeByLocation(storeBName, 100);
        duplicate.pointCode = "DUPLICATE_IN_SAME_STORE";
        bool rejected = false;
        try {
            router.addRoute(duplicate);
        } catch (const std::invalid_argument& ex) {
            rejected = std::string(ex.what()).find("duplicate point route location") != std::string::npos;
        }
        require(rejected, "same index in the same shared memory should still be rejected");
    } catch (...) {
        removeTree(root);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeAName);
        edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeBName);
        throw;
    }
    removeTree(root);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeAName);
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(storeBName);
}

void verifyLocalAccessPermissionsValidation() {
    const auto root = createProjectDirectory(false);
    try {
        writeFile(root + "/permissions.json", R"({
          "roles":["operator","maintainer"],
          "localAccess":{
            "sessionTimeoutSeconds":900,
            "protectedScreenPrefixes":["overview"],
            "users":[{
              "username":"operator",
              "salt":"00112233445566778899aabbccddeeff",
              "passwordSha256":"e818d3f9b975db3914dedfeb7f81d07324d445ba6bdacaccf4f2098fbb51c4a6",
              "roles":["operator"]
            }]
          }
        })");
        const auto project = edge_gateway::ScadaProjectLoader::loadFromDirectory(root);
        require(project.permissions.localAccess.enabled(), "local access permissions were not loaded");
        require(project.permissions.localAccess.sessionTimeoutSeconds == 900, "local access timeout mismatch");
        require(project.permissions.localAccess.users.size() == 1, "local access user mismatch");

        auto invalidHash = project;
        invalidHash.permissions.localAccess.users.front().passwordSha256 = "bad";
        bool hashRejected = false;
        try {
            edge_gateway::ScadaProjectLoader::validate(invalidHash);
        } catch (const std::runtime_error& ex) {
            hashRejected = std::string(ex.what()).find("password record") != std::string::npos;
        }
        require(hashRejected, "invalid local password digest should fail");

        auto incomplete = project;
        incomplete.permissions.localAccess.users.clear();
        bool incompleteRejected = false;
        try {
            edge_gateway::ScadaProjectLoader::validate(incomplete);
        } catch (const std::runtime_error& ex) {
            incompleteRejected = std::string(ex.what()).find("both protected screens and users") != std::string::npos;
        }
        require(incompleteRejected, "incomplete local access policy should fail");
    } catch (...) {
        removeTree(root);
        throw;
    }
    removeTree(root);
}

}  // namespace

int main() {
    try {
        verifyLoadResolveReadAndWrite();
        verifySameIndexAcrossIndependentStores();
        verifyDuplicateRuntimeRouteRejected();
        verifyWritableMappingAndStateRuleValidation();
        verifyLocalAccessPermissionsValidation();
        std::cout << "scada_runtime_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "scada_runtime_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
