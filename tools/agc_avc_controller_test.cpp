#include "edge_gateway/agc_avc_controller.hpp"
#include "edge_gateway/agc_avc_service.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/power_control_ownership.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            std::string(message) + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected)
        );
    }
}

edge_gateway::AgcAvcConfig baseConfig() {
    using namespace edge_gateway;
    AgcAvcConfig config;
    config.enabled = true;
    config.shadowMode = true;
    config.submitWrites = false;
    config.cycleMs = 200;
    config.commandTimeoutMs = 5000;
    config.inputMaxAgeMs = 1500;
    config.stationLimits.ratedActivePowerKw = 100.0;
    config.stationLimits.ratedApparentPowerKva = 100.0;
    config.stationLimits.maxImportPowerKw = 100.0;
    config.stationLimits.maxExportPowerKw = 100.0;
    config.stationLimits.maxReactivePowerKvar = 100.0;
    config.agc.kp = 0.0;
    config.agc.ki = 0.0;
    config.agc.minKw = -100.0;
    config.agc.maxKw = 100.0;
    config.agc.riseKwPerSec = 1000.0;
    config.agc.fallKwPerSec = 1000.0;
    config.avc.kpQ = 0.0;
    config.avc.kiQ = 0.0;
    config.avc.minKvar = -100.0;
    config.avc.maxKvar = 100.0;
    config.avc.riseKvarPerSec = 1000.0;
    config.avc.fallKvarPerSec = 1000.0;
    AgcAvcPcsConfig pcs;
    pcs.machineCode = "GW_TEST";
    pcs.meterCode = "PCS_1";
    pcs.ratedActivePowerKw.commissionedLimit = 100.0;
    pcs.ratedApparentPowerKva.commissionedLimit = 100.0;
    pcs.ratedReactivePowerKvar = 100.0;
    pcs.maxChargePowerKw = 100.0;
    pcs.maxDischargePowerKw = 100.0;
    pcs.riseKwPerSec = 1000.0;
    pcs.fallKwPerSec = 1000.0;
    pcs.riseKvarPerSec = 1000.0;
    pcs.fallKvarPerSec = 1000.0;
    pcs.fallbackToStaticLimit = true;
    config.pcs.push_back(pcs);
    return config;
}

edge_gateway::AgcAvcCycleInput baseInput(std::int64_t now) {
    using namespace edge_gateway;
    AgcAvcCycleInput input;
    input.nowMs = now;
    input.remoteEnable = true;
    input.command.sequence = 1;
    input.command.issuedAtMs = now - 100;
    input.command.targetPkw = 0.0;
    input.command.targetQkvar = 0.0;
    input.command.avcMode = "reactivePower";
    AgcAvcPcsRuntimeInput pcs;
    pcs.meterCode = "PCS_1";
    pcs.online = true;
    pcs.ready = true;
    pcs.chargeAllowed = true;
    pcs.dischargeAllowed = true;
    pcs.soc = 50.0;
    input.pcs.push_back(pcs);
    return input;
}

class FakePointBus final : public edge_gateway::IAgcAvcPointBus {
public:
    edge_gateway::Optional<edge_gateway::PointStoreRoute> routeByIndex(std::uint32_t index) const override {
        const auto it = routes.find(index);
        return it == routes.end() ? edge_gateway::Optional<edge_gateway::PointStoreRoute>() : it->second;
    }

    std::vector<edge_gateway::StoredPointValue> readSnapshot(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t
    ) const override {
        std::vector<edge_gateway::StoredPointValue> result;
        for (const auto index : indexes) {
            const auto it = values.find(index);
            if (it != values.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    edge_gateway::CommandSubmitResult publishVirtual(edge_gateway::PointValue value) override {
        published.push_back(value);
        edge_gateway::CommandSubmitResult result;
        result.accepted = routes.count(value.index) != 0;
        return result;
    }

    edge_gateway::CommandSubmitResult submitControl(const edge_gateway::PendingWriteCommand& command) override {
        submitted.push_back(command);
        edge_gateway::CommandSubmitResult result;
        result.accepted = true;
        result.route = routes.at(command.index);
        return result;
    }

    edge_gateway::Optional<edge_gateway::WritebackResultRecord> readWriteResult(
        const edge_gateway::PointStoreRoute&,
        const std::string&
    ) const override {
        return edge_gateway::Optional<edge_gateway::WritebackResultRecord>();
    }

    void add(std::uint32_t index, double value, std::int64_t ts, bool writable = false) {
        edge_gateway::PointStoreRoute route;
        route.index = index;
        route.machineCode = "GW_TEST";
        route.meterCode = "PCS_1";
        route.pointCode = "P" + std::to_string(index);
        route.sharedMemoryName = "test";
        route.writable = writable;
        routes[index] = route;
        edge_gateway::StoredPointValue point;
        point.index = index;
        point.value = value;
        point.quality = 1;
        point.ts = ts;
        values[index] = point;
    }

    std::unordered_map<std::uint32_t, edge_gateway::PointStoreRoute> routes;
    std::unordered_map<std::uint32_t, edge_gateway::StoredPointValue> values;
    std::vector<edge_gateway::PointValue> published;
    std::vector<edge_gateway::PendingWriteCommand> submitted;
};

void testPqEnvelope() {
    auto config = baseConfig();
    edge_gateway::AgcAvcController controller(config);
    auto input = baseInput(10000);
    input.command.targetPkw = 80.0;
    input.command.targetQkvar = 80.0;
    const auto output = controller.step(input);
    require(output.state == edge_gateway::AgcAvcRuntimeState::Active, "P/Q envelope should remain active");
    requireNear(output.effectiveTargetPkw, 80.0, 0.001, "active power first must keep P");
    requireNear(output.effectiveTargetQkvar, 60.0, 0.001, "P/Q capability circle must clip Q");
}

void testDynamicCapacityAndRedistribution() {
    auto config = baseConfig();
    config.stationLimits.ratedActivePowerKw = 200.0;
    config.stationLimits.ratedApparentPowerKva = 200.0;
    config.stationLimits.maxExportPowerKw = 200.0;
    config.agc.maxKw = 200.0;
    config.pcs[0].fallbackToStaticLimit = false;
    auto second = config.pcs[0];
    second.meterCode = "PCS_2";
    config.pcs.push_back(second);
    edge_gateway::AgcAvcController controller(config);
    auto input = baseInput(20000);
    input.command.targetPkw = 100.0;
    input.pcs[0].hasAvailableDischarge = true;
    input.pcs[0].availableDischargeKw = 20.0;
    auto runtime2 = input.pcs[0];
    runtime2.meterCode = "PCS_2";
    runtime2.availableDischargeKw = 80.0;
    input.pcs.push_back(runtime2);
    const auto output = controller.step(input);
    requireNear(output.pcs[0].targetPkw, 20.0, 0.001, "first PCS must respect dynamic capability");
    requireNear(output.pcs[1].targetPkw, 80.0, 0.001, "remaining target must be redistributed");
    requireNear(output.unservedActivePowerKw, 0.0, 0.001, "redistributed target should be fully served");
}

void testExpiredCommand() {
    auto config = baseConfig();
    edge_gateway::AgcAvcController controller(config);
    auto active = baseInput(30000);
    active.command.targetPkw = 30.0;
    const auto first = controller.step(active);
    require(first.state == edge_gateway::AgcAvcRuntimeState::Active, "fresh command should activate controller");
    auto expired = active;
    expired.nowMs = 40000;
    const auto second = controller.step(expired);
    require(second.state == edge_gateway::AgcAvcRuntimeState::Standby ||
            second.state == edge_gateway::AgcAvcRuntimeState::RampToZero,
            "expired command must leave active state");
}

void testMinimumStablePowerAndSocDerating() {
    auto config = baseConfig();
    config.stationLimits.ratedActivePowerKw = 200.0;
    config.stationLimits.ratedApparentPowerKva = 200.0;
    config.stationLimits.maxExportPowerKw = 200.0;
    config.agc.maxKw = 200.0;
    config.pcs[0].minStableDischargePowerKw = 10.0;
    auto second = config.pcs[0];
    second.meterCode = "PCS_2";
    config.pcs.push_back(second);
    edge_gateway::AgcAvcController controller(config);
    auto input = baseInput(41000);
    input.command.targetPkw = 15.0;
    auto runtime2 = input.pcs[0];
    runtime2.meterCode = "PCS_2";
    input.pcs.push_back(runtime2);
    const auto output = controller.step(input);
    const auto activeCount = std::count_if(output.pcs.begin(), output.pcs.end(), [](const auto& pcs) {
        return std::abs(pcs.targetPkw) > 1e-6;
    });
    require(activeCount == 1, "minimum stable power should consolidate a small target onto one PCS");
    requireNear(output.effectiveTargetPkw, 15.0, 0.001, "minimum stable allocation must preserve the station target");

    auto deratedConfig = baseConfig();
    edge_gateway::AgcAvcController deratedController(deratedConfig);
    auto deratedInput = baseInput(42000);
    deratedInput.command.targetPkw = 100.0;
    deratedInput.pcs[0].soc = 7.5;
    const auto derated = deratedController.step(deratedInput);
    requireNear(derated.availableActivePowerKw, 50.0, 0.001, "SOC boundary should progressively derate discharge capability");
}

void testAvcRunsAtConfiguredCadence() {
    auto config = baseConfig();
    config.cycleMs = 100;
    config.avcCycleMs = 500;
    edge_gateway::AgcAvcController controller(config);
    auto first = baseInput(43000);
    first.command.targetQkvar = 10.0;
    const auto firstOutput = controller.step(first);
    requireNear(firstOutput.rawTargetQkvar, 10.0, 0.001, "first AVC cycle should calculate target");

    auto early = first;
    early.nowMs += 100;
    early.command.issuedAtMs += 100;
    early.command.targetQkvar = 50.0;
    const auto held = controller.step(early);
    requireNear(held.rawTargetQkvar, 10.0, 0.001, "AVC target should be held between divided cycles");

    auto due = early;
    due.nowMs += 400;
    due.command.issuedAtMs += 400;
    const auto refreshed = controller.step(due);
    requireNear(refreshed.rawTargetQkvar, 50.0, 0.001, "AVC target should refresh when avcCycleMs elapses");
}

void testCommandMailboxRoute() {
    edge_gateway::MemoryPointStore store("agc_avc_mailbox_test_store");
    edge_gateway::PointStoreRouter router;
    router.addStore("agc_avc_mailbox_test_store", store);
    edge_gateway::PointStoreRoute route;
    route.index = 720010;
    route.machineCode = "GW_TEST";
    route.meterCode = "AGC_AVC_CORE";
    route.pointCode = "agc_dispatch_p";
    route.sharedMemoryName = "agc_avc_mailbox_test_store";
    route.commandMailbox = true;
    router.addRoute(route);
    edge_gateway::PendingWriteCommand command;
    command.cmdId = "MAILBOX_1";
    command.index = route.index;
    command.value = 12.5;
    command.source = "gateway-desktop-agc-avc-shadow-test";
    command.ts = 44000;
    const auto submitted = router.submitCommandMailbox(command);
    require(submitted.accepted, "AGC/AVC mailbox route should accept command values without device writeback");
    const auto stored = router.getLatestByIndex(route.index, 44001);
    require(static_cast<bool>(stored), "AGC/AVC mailbox value should be visible in shared latest storage");
    requireNear(stored->value, 12.5, 0.001, "AGC/AVC mailbox should preserve the command value");
}

void testShadowServiceDoesNotWrite() {
    using namespace edge_gateway;
    const std::int64_t now = 50000;
    auto config = baseConfig();
    config.priorityControlLeaseFile.clear();
    config.interlocks.remoteEnable = {"sharedLatest", "site.remoteEnable", "", "", "", 1, true};
    config.interlocks.emergencyStop = {"sharedLatest", "site.emergencyStop", "", "", "", 2, true};
    config.interlocks.fireAlarm = {"sharedLatest", "site.fireAlarm", "", "", "", 3, true};
    config.agc.target = {"sharedCommand", "agc.target", "", "", "", 4, true};
    config.agc.commandSequence = {"sharedCommand", "agc.sequence", "", "", "", 5, true};
    config.agc.commandTimestamp = {"sharedCommand", "agc.timestamp", "", "", "", 6, true};
    config.agc.pccActivePower = {"sharedLatest", "site.pcc.activePower", "", "", "", 7, true};
    config.avc.pccReactivePower = {"sharedLatest", "site.pcc.reactivePower", "", "", "", 8, true};
    auto& pcs = config.pcs[0];
    pcs.points.online = {"sharedLatest", "pcs.online", "", "", "", 9, true};
    pcs.points.ready = {"sharedLatest", "pcs.ready", "", "", "", 10, true};
    pcs.points.soc = {"sharedLatest", "bms.soc", "", "", "", 11, true};
    pcs.points.actualP = {"sharedLatest", "pcs.actualP", "", "", "", 12, true};
    pcs.points.actualQ = {"sharedLatest", "pcs.actualQ", "", "", "", 13, true};
    pcs.points.chargeAllowed = {"sharedLatest", "bms.chargeAllowed", "", "", "", 14, true};
    pcs.points.dischargeAllowed = {"sharedLatest", "bms.dischargeAllowed", "", "", "", 15, true};
    pcs.points.activeTargets = {{"sharedWriteback", "pcs.targetP", "", "", "", 16, true}};
    pcs.points.reactiveTargets = {{"sharedWriteback", "pcs.targetQ", "", "", "", 17, true}};

    FakePointBus bus;
    for (std::uint32_t index = 1; index <= 17; ++index) {
        bus.add(index, 0.0, now - 10, index >= 16);
    }
    bus.values[1].value = 1.0;
    bus.values[4].value = 20.0;
    bus.values[5].value = 1.0;
    bus.values[6].value = static_cast<double>(now - 100);
    bus.values[9].value = 1.0;
    bus.values[10].value = 1.0;
    bus.values[11].value = 50.0;
    bus.values[14].value = 1.0;
    bus.values[15].value = 1.0;

    AgcAvcService service(config, bus);
    const auto output = service.tick(now);
    require(output.state == AgcAvcRuntimeState::Active, "shadow service should calculate active target");
    require(bus.submitted.empty(), "shadow mode must not submit write commands");
    bus.values[4].value = 80.0;
    const auto sameSequence = service.tick(now + 100);
    requireNear(sameSequence.effectiveTargetPkw, 20.0, 0.001, "command fields must remain latched until sequence changes");
    bus.values[5].value = 2.0;
    bus.values[5].ts = now + 200;
    bus.values[6].value = static_cast<double>(now + 100);
    bus.values[6].ts = now + 200;
    const auto nextSequence = service.tick(now + 200);
    requireNear(nextSequence.effectiveTargetPkw, 80.0, 0.001, "new sequence should atomically commit updated command fields");

    const std::string ownershipPath = "/tmp/agc_avc_service_write_test.json";
    std::remove(ownershipPath.c_str());
    config.shadowMode = false;
    config.submitWrites = true;
    config.ownership.leaseFile = ownershipPath;
    FakePointBus writeBus = bus;
    writeBus.published.clear();
    writeBus.submitted.clear();
    {
        AgcAvcService writeService(config, writeBus);
        const auto writeOutput = writeService.tick(now + 1);
        require(writeOutput.state == AgcAvcRuntimeState::Active, "real service should hold ownership and stay active");
        require(writeBus.submitted.size() == 2, "real mode should submit active and reactive targets");
    }
    std::remove(ownershipPath.c_str());
}

void testPowerControlOwnership() {
    const std::string path = "/tmp/agc_avc_power_ownership_test.json";
    std::remove(path.c_str());
    edge_gateway::PowerControlOwnership agc(path, "agc-avc");
    edge_gateway::PowerControlOwnership mqtt(path, "mqtt-driver");
    require(agc.acquire("pcs-power", "S1", {16, 17}, 1000, 1000), "AGC should acquire free ownership");
    require(mqtt.isBlocked(16, "mqtt-driver", 1200), "other controller should be blocked on owned target");
    require(!mqtt.isBlocked(99, "mqtt-driver", 1200), "unrelated target must remain writable");
    require(!agc.isBlocked(16, "agc-avc", 1200), "ownership holder must not block itself");
    require(!mqtt.isBlocked(16, "mqtt-driver", 2500), "expired ownership must not block");
    agc.release("S1");
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
}

void testPowerControlOwnershipConcurrentAcquire() {
    const std::string path = "/tmp/agc_avc_power_ownership_concurrent_test.json";
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
    edge_gateway::PowerControlOwnership first(path, "first-controller");
    edge_gateway::PowerControlOwnership second(path, "second-controller");
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    bool firstAcquired = false;
    bool secondAcquired = false;
    std::thread firstThread([&]() {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        firstAcquired = first.acquire("pcs-power", "FIRST", {16}, 1000, 1000);
    });
    std::thread secondThread([&]() {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        secondAcquired = second.acquire("pcs-power", "SECOND", {16}, 1000, 1000);
    });
    while (ready.load() < 2) std::this_thread::yield();
    go = true;
    firstThread.join();
    secondThread.join();
    require(firstAcquired != secondAcquired, "cross-process ownership lock must admit exactly one concurrent owner");
    if (firstAcquired) first.release("FIRST");
    if (secondAcquired) second.release("SECOND");
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
}

void testAgcAvcConfigParsing() {
    const std::string path = "/tmp/agc_avc_config_loader_test.json";
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    output << R"JSON({
      "runtimeMode": "agc_avc",
      "agcAvc": {
        "enabled": true,
        "shadowMode": true,
        "submitWrites": false,
        "cycleMs": 250,
        "stationLimits": {"ratedActivePowerKw": 100, "ratedApparentPowerKva": 110},
        "agc": {
          "target": {"source": "sharedCommand", "semanticRole": "agc.target", "index": 720010, "required": true},
          "commandSequence": {"index": 720011},
          "commandTimestamp": {"index": 720012},
          "pccActivePower": {"index": 1001}
        },
        "pcs": [{
          "machineCode": "GW_TEST",
          "meterCode": "PCS_1",
          "ratedActivePowerKw": 100,
          "ratedApparentPowerKva": {
            "commissionedLimit": 110,
            "sourcePoint": {"index": 1010},
            "combinePolicy": "min"
          },
          "maxChargePowerKw": 100,
          "maxDischargePowerKw": 100,
          "points": {
            "online": {"index": 1002},
            "ready": {"index": 1003},
            "actualP": {"index": 1004},
            "actualQ": {"index": 1005},
            "activeTargets": [{"source": "sharedWriteback", "index": 1011}],
            "reactiveTargets": [{"source": "sharedWriteback", "index": 1012}]
          }
        }]
      }
    })JSON";
    output.close();
    const auto config = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    require(config.agcAvc.enabled, "AGC/AVC config should parse enabled flag");
    require(config.agcAvc.cycleMs == 250, "AGC/AVC cycle should parse");
    require(config.agcAvc.pcs.size() == 1, "AGC/AVC PCS list should parse");
    require(config.agcAvc.pcs[0].ratedApparentPowerKva.hasSourcePoint, "capability source point should parse");
    require(config.agcAvc.pcs[0].points.activeTargets[0].source == "sharedWriteback", "writeback source should parse");
    std::remove(path.c_str());
}

}  // namespace

int main() {
    try {
        testPqEnvelope();
        testDynamicCapacityAndRedistribution();
        testExpiredCommand();
        testMinimumStablePowerAndSocDerating();
        testAvcRunsAtConfiguredCadence();
        testCommandMailboxRoute();
        testShadowServiceDoesNotWrite();
        testPowerControlOwnership();
        testPowerControlOwnershipConcurrentAcquire();
        testAgcAvcConfigParsing();
        std::cout << "agc_avc_controller_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "agc_avc_controller_test failed: " << ex.what() << std::endl;
        return 1;
    }
}
