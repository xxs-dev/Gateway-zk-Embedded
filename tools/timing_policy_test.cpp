#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "edge_gateway/timing_policy.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

edge_gateway::PointDefinition readablePoint(std::uint32_t index, bool explicitInterval) {
    edge_gateway::PointDefinition point;
    point.index = index;
    point.enabled = true;
    point.read.enable = true;
    point.read.intervalMs = explicitInterval ? 750 : 500;
    point.read.intervalExplicit = explicitInterval;
    return point;
}

void appliesSemanticProfileWithoutOverwritingPointOverride() {
    edge_gateway::DeviceConfig config;
    config.protocol.type = "modbus_rtu";
    config.protocol.transport.frameIntervalMs = -1;
    config.collect.defaultIntervalMs = 500;
    config.timingPolicy.configured = true;
    config.timingPolicy.profile = "highFrequency";
    config.points = {readablePoint(1, false), readablePoint(2, true)};

    const auto resolved = edge_gateway::TimingPolicyResolver::apply(config);

    require(resolved.collectIntervalMs == 200, "highFrequency should resolve to 200ms");
    require(config.collect.defaultIntervalMs == 200, "collect interval should be applied");
    require(config.protocol.transport.frameIntervalMs == 5, "request gap should be protocol-safe and independent");
    require(config.points[0].read.intervalMs == 200, "inherited point should follow semantic profile");
    require(config.points[1].read.intervalMs == 750, "explicit point interval must be preserved");
}

void keepsLegacyCollectionAndSeparatesMissingFrameGap() {
    edge_gateway::DeviceConfig config;
    config.protocol.type = "modbus_rtu";
    config.protocol.transport.frameIntervalMs = -1;
    config.collect.defaultIntervalMs = 5000;

    const auto resolved = edge_gateway::TimingPolicyResolver::apply(config);

    require(!resolved.semanticPolicyActive, "legacy config should stay custom");
    require(config.collect.defaultIntervalMs == 5000, "legacy collect interval should be preserved");
    require(config.protocol.transport.frameIntervalMs == 0, "frame gap must not inherit the collect interval");
}

void rejectsSilentlyUnreachableIntervals() {
    edge_gateway::DeviceConfig config;
    config.protocol.type = "modbus_rtu";
    config.collect.defaultIntervalMs = 1;
    bool rejected = false;
    try {
        (void)edge_gateway::TimingPolicyResolver::apply(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "unreachable Modbus interval should fail instead of being silently clamped");
}

void appliesIecTcpAndPassiveTcpTimeouts() {
    edge_gateway::DeviceConfig tcpConfig;
    tcpConfig.protocol.type = "iec103";
    tcpConfig.protocol.iec.transportMode = "tcp";
    tcpConfig.protocol.tcp.timeoutMs = 2000;
    tcpConfig.protocol.iec.pollTimeoutMs = 1500;
    tcpConfig.timingPolicy.configured = true;
    tcpConfig.timingPolicy.profile = "highFrequency";

    (void)edge_gateway::TimingPolicyResolver::apply(tcpConfig);
    require(tcpConfig.protocol.tcp.timeoutMs == 300, "IEC103 TCP socket timeout should follow the timing profile");
    require(tcpConfig.protocol.iec.pollTimeoutMs == 300, "IEC103 poll timeout should follow the timing profile");
    require(tcpConfig.protocol.iec.idleReadTimeoutMs == 20, "IEC103 receive wait should follow the timing profile");

    edge_gateway::DeviceConfig passiveConfig;
    passiveConfig.protocol.type = "iec103";
    passiveConfig.protocol.iec.transportMode = "am5se_passive_tcp";
    passiveConfig.protocol.transport.timeoutMs = 4000;
    passiveConfig.protocol.iec.pollTimeoutMs = 1800;
    passiveConfig.timingPolicy.configured = true;
    passiveConfig.timingPolicy.profile = "realtime";

    const auto resolved = edge_gateway::TimingPolicyResolver::apply(passiveConfig);
    require(resolved.responseTimeoutMs == 800, "passive IEC103 should resolve a network response timeout");
    require(passiveConfig.protocol.iec.pollTimeoutMs == 800, "passive IEC103 poll timeout should follow the timing profile");
    require(passiveConfig.protocol.iec.idleReadTimeoutMs == 50, "passive IEC103 receive wait should follow the timing profile");
}

void capsDeliverySchedulersByMaximumLatency() {
    edge_gateway::AppConfig config;
    config.timingPolicy.configured = true;
    config.timingPolicy.profile = "custom";
    config.timingPolicy.delivery.mode = "periodic";
    config.timingPolicy.delivery.batchWindowMs = 200;
    config.timingPolicy.delivery.maxLatencyMs = 50;
    config.timingPolicy.delivery.heartbeatMs = 5000;
    config.mqttDriver.scanIntervalMs = 1000;
    config.eventEngine.scanIntervalMs = 1000;

    edge_gateway::TimingPolicyResolver::applyAppServices(config);

    require(config.mqttDriver.scanIntervalMs == 50, "MQTT scheduler must not exceed delivery.maxLatencyMs");
    require(config.eventEngine.scanIntervalMs == 50, "EventEngine scheduler must not exceed delivery.maxLatencyMs");
    require(config.mqttDriver.fullUploadIntervalMs == 5000, "delivery heartbeat should drive full calibration");
}

void capsExistingSchedulerWithoutBatchWindow() {
    edge_gateway::AppConfig config;
    config.timingPolicy.configured = true;
    config.timingPolicy.profile = "custom";
    config.timingPolicy.delivery.maxLatencyMs = 40;
    config.mqttDriver.scanIntervalMs = 500;
    config.eventEngine.scanIntervalMs = 500;

    edge_gateway::TimingPolicyResolver::applyAppServices(config);

    require(config.mqttDriver.scanIntervalMs == 40, "maximum latency should cap an existing MQTT interval");
    require(config.eventEngine.scanIntervalMs == 40, "maximum latency should cap an existing event interval");
}

}  // namespace

int main() {
    try {
        appliesSemanticProfileWithoutOverwritingPointOverride();
        keepsLegacyCollectionAndSeparatesMissingFrameGap();
        rejectsSilentlyUnreachableIntervals();
        appliesIecTcpAndPassiveTcpTimeouts();
        capsDeliverySchedulersByMaximumLatency();
        capsExistingSchedulerWithoutBatchWindow();
        std::cout << "timing policy tests passed" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
