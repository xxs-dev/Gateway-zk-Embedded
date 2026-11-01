// Shared fixture for the AGC/AVC service tests: base configuration, an in-memory point bus and the
// point layout used by the service tests (1..15 inputs, 16/17 active/reactive PCS targets).
#pragma once

#include "edge_gateway/agc_avc_controller.hpp"
#include "edge_gateway/agc_avc_service.hpp"
#include "edge_gateway/models.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace agc_test {

inline edge_gateway::AgcAvcConfig baseConfig() {
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
        const std::string& cmdId
    ) const override {
        const auto it = results.find(cmdId);
        return it == results.end() ? edge_gateway::Optional<edge_gateway::WritebackResultRecord>()
                                   : edge_gateway::Optional<edge_gateway::WritebackResultRecord>(it->second);
    }
    std::unordered_map<std::string, edge_gateway::WritebackResultRecord> results;

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

// Interlocks, command mailbox and one PCS with active target 16 and reactive target 17. The command
// is sequence 1 with a 20 kW target issued 100 ms before now.
inline void configureServicePoints(edge_gateway::AgcAvcConfig& config, FakePointBus& bus, std::int64_t now) {
    using namespace edge_gateway;
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

    config.commandMailbox.platformDispatch = true;
    for (std::uint32_t index = 1; index <= 17; ++index) {
        bus.add(index, 0.0, now - 10, index >= 16);
    }
    for (const std::uint32_t index : {4U, 5U, 6U}) {
        bus.values[index].origin = LatestValueOrigin::PlatformDispatch;
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

}

}  // namespace agc_test
