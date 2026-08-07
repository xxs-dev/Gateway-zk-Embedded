#include "edge_gateway/timing_policy.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace edge_gateway {

namespace {

struct TimingPreset {
    int freshnessMs;
    int maxAgeMs;
    int requestGapMs;
    int timeoutMs;
    int retryCount;
    int interfaceCheckMs;
    int receiveWaitMs;
    int deliveryMaxLatencyMs;
    int deliveryBatchWindowMs;
    int deliveryHeartbeatMs;
};

std::string normalizeProfile(std::string value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '_' || ch == '-' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized == "ultrafast") return "ultraFast";
    if (normalized == "highfrequency" || normalized == "high") return "highFrequency";
    if (normalized == "realtime") return "realtime";
    if (normalized == "standard") return "standard";
    if (normalized == "normal") return "normal";
    if (normalized == "slow") return "slow";
    if (normalized.empty() || normalized == "inherit") return "inherit";
    if (normalized == "custom") return "custom";
    throw std::invalid_argument("unsupported timingPolicy.profile: " + value);
}

std::string normalizeDeliveryMode(std::string value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '_' || ch == '-' || std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized.empty()) return {};
    if (normalized == "onchange") return "onChange";
    if (normalized == "periodic") return "periodic";
    if (normalized == "hybrid") return "hybrid";
    throw std::invalid_argument("unsupported timingPolicy.delivery.mode: " + value);
}

TimingPreset presetFor(const std::string& profile) {
    if (profile == "ultraFast") return {50, 500, 2, 100, 1, 100, 10, 50, 10, 60000};
    if (profile == "highFrequency") return {200, 2000, 5, 300, 1, 100, 20, 200, 20, 60000};
    if (profile == "realtime") return {500, 5000, 10, 800, 1, 250, 50, 500, 50, 60000};
    if (profile == "standard") return {1000, 10000, 20, 1000, 2, 500, 100, 1000, 100, 60000};
    if (profile == "normal") return {5000, 30000, 50, 2000, 2, 1000, 200, 5000, 500, 60000};
    if (profile == "slow") return {60000, 180000, 100, 3000, 1, 5000, 500, 60000, 1000, 300000};
    throw std::invalid_argument("timing profile has no preset: " + profile);
}

bool hasPreset(const std::string& profile) {
    return profile == "ultraFast" || profile == "highFrequency" ||
        profile == "realtime" || profile == "standard" ||
        profile == "normal" || profile == "slow";
}

bool isIecProtocol(const DeviceConfig& config) {
    const auto& type = config.protocol.type;
    return type == "iec101" || type == "iec103" || type == "iec103_serial" ||
        type == "iec103_tcp" || type == "iec104";
}

bool usesTcpSocket(const DeviceConfig& config) {
    const auto& type = config.protocol.type;
    return type == "modbus_tcp" || type == "iec104" || type == "iec103_tcp" ||
        (type == "iec103" && config.protocol.iec.transportMode == "tcp");
}

bool usesPassiveTcp(const DeviceConfig& config) {
    return config.protocol.type == "iec103" &&
        config.protocol.iec.transportMode == "am5se_passive_tcp";
}

void overridePositive(int& target, int value) {
    if (value > 0) target = value;
}

void overrideNonNegative(int& target, int value) {
    if (value >= 0) target = value;
}

TimingPolicyConfig mergePolicies(
    const TimingPolicyConfig* projectDefault,
    const TimingPolicyConfig& devicePolicy
) {
    TimingPolicyConfig merged;
    const auto apply = [&](const TimingPolicyConfig& source) {
        if (!source.configured) return;
        merged.configured = true;
        const auto profile = normalizeProfile(source.profile);
        if (profile != "inherit") merged.profile = profile;
        overridePositive(merged.acquisition.targetFreshnessMs, source.acquisition.targetFreshnessMs);
        overridePositive(merged.acquisition.maxAgeMs, source.acquisition.maxAgeMs);
        if (!source.delivery.mode.empty()) merged.delivery.mode = normalizeDeliveryMode(source.delivery.mode);
        overridePositive(merged.delivery.maxLatencyMs, source.delivery.maxLatencyMs);
        overrideNonNegative(merged.delivery.batchWindowMs, source.delivery.batchWindowMs);
        overridePositive(merged.delivery.heartbeatMs, source.delivery.heartbeatMs);
        overridePositive(merged.overrides.pollIntervalMs, source.overrides.pollIntervalMs);
        overrideNonNegative(merged.overrides.requestGapMs, source.overrides.requestGapMs);
        overridePositive(merged.overrides.responseTimeoutMs, source.overrides.responseTimeoutMs);
        overrideNonNegative(merged.overrides.retryCount, source.overrides.retryCount);
        overridePositive(merged.overrides.interfaceCheckIntervalMs, source.overrides.interfaceCheckIntervalMs);
        overridePositive(merged.overrides.receiveWaitMs, source.overrides.receiveWaitMs);
        overridePositive(merged.overrides.mqttScanIntervalMs, source.overrides.mqttScanIntervalMs);
    };
    if (projectDefault != nullptr) apply(*projectDefault);
    apply(devicePolicy);
    if (merged.profile == "inherit") merged.profile = "custom";
    return merged;
}

void applyPointTiming(PointDefinition& point, const ResolvedTimingConfig& resolved) {
    if (!resolved.semanticPolicyActive || !point.read.enable) return;
    if (!point.read.intervalExplicit) point.read.intervalMs = resolved.pointIntervalMs;
    if (resolved.maxAgeMs > 0 && !point.read.cachePolicy.ttlExplicit) {
        point.read.cachePolicy.ttlMs = resolved.maxAgeMs;
    }
    if (resolved.maxAgeMs > 0 && !point.read.can.receiveTimeoutExplicit) {
        point.read.can.receiveTimeoutMs = resolved.maxAgeMs;
    }
}

}  // namespace

int TimingPolicyResolver::minimumPollIntervalMs(const std::string& protocolType) {
    if (protocolType == "local_dio") return 1;
    if (protocolType == "can" || protocolType == "can_socketcan") return 5;
    if (protocolType == "computed" || protocolType == "ems_virtual" ||
        protocolType == "agc_avc_virtual") return 10;
    return 20;
}

ResolvedTimingConfig TimingPolicyResolver::resolve(
    const DeviceConfig& config,
    const TimingPolicyConfig* projectDefault
) {
    const auto policy = mergePolicies(projectDefault, config.timingPolicy);
    ResolvedTimingConfig resolved;
    resolved.semanticPolicyActive = policy.configured;
    resolved.profile = policy.profile;
    resolved.minimumPollIntervalMs = minimumPollIntervalMs(config.protocol.type);
    resolved.collectIntervalMs = config.collect.defaultIntervalMs;
    resolved.pointIntervalMs = config.collect.defaultIntervalMs;
    resolved.requestGapMs = config.protocol.transport.frameIntervalMs >= 0
        ? config.protocol.transport.frameIntervalMs
        : 0;
    resolved.responseTimeoutMs = usesPassiveTcp(config)
        ? config.protocol.iec.pollTimeoutMs
        : usesTcpSocket(config)
            ? config.protocol.tcp.timeoutMs
            : config.protocol.transport.timeoutMs;
    resolved.retryCount = config.protocol.transport.readRetryCount;
    resolved.interfaceCheckIntervalMs = config.collect.interfaceCheckIntervalMs;
    resolved.receiveWaitMs = config.collect.receiveWaitMs;

    if (policy.configured && hasPreset(policy.profile)) {
        const auto preset = presetFor(policy.profile);
        resolved.collectIntervalMs = preset.freshnessMs;
        resolved.pointIntervalMs = preset.freshnessMs;
        resolved.maxAgeMs = preset.maxAgeMs;
        resolved.requestGapMs = preset.requestGapMs;
        resolved.responseTimeoutMs = preset.timeoutMs;
        resolved.retryCount = preset.retryCount;
        resolved.interfaceCheckIntervalMs = preset.interfaceCheckMs;
        resolved.receiveWaitMs = preset.receiveWaitMs;
        resolved.deliveryMode = "onChange";
        resolved.deliveryMaxLatencyMs = preset.deliveryMaxLatencyMs;
        resolved.deliveryBatchWindowMs = preset.deliveryBatchWindowMs;
        resolved.deliveryHeartbeatMs = preset.deliveryHeartbeatMs;
    }

    overridePositive(resolved.collectIntervalMs, policy.acquisition.targetFreshnessMs);
    overridePositive(resolved.pointIntervalMs, policy.acquisition.targetFreshnessMs);
    overridePositive(resolved.maxAgeMs, policy.acquisition.maxAgeMs);
    if (!policy.delivery.mode.empty()) resolved.deliveryMode = policy.delivery.mode;
    overridePositive(resolved.deliveryMaxLatencyMs, policy.delivery.maxLatencyMs);
    overrideNonNegative(resolved.deliveryBatchWindowMs, policy.delivery.batchWindowMs);
    overridePositive(resolved.deliveryHeartbeatMs, policy.delivery.heartbeatMs);
    overridePositive(resolved.collectIntervalMs, policy.overrides.pollIntervalMs);
    overridePositive(resolved.pointIntervalMs, policy.overrides.pollIntervalMs);
    overrideNonNegative(resolved.requestGapMs, policy.overrides.requestGapMs);
    overridePositive(resolved.responseTimeoutMs, policy.overrides.responseTimeoutMs);
    overrideNonNegative(resolved.retryCount, policy.overrides.retryCount);
    overridePositive(resolved.interfaceCheckIntervalMs, policy.overrides.interfaceCheckIntervalMs);
    overridePositive(resolved.receiveWaitMs, policy.overrides.receiveWaitMs);

    if (resolved.collectIntervalMs < resolved.minimumPollIntervalMs) {
        throw std::invalid_argument(
            "timing poll interval " + std::to_string(resolved.collectIntervalMs) +
            "ms is below protocol minimum " +
            std::to_string(resolved.minimumPollIntervalMs) + "ms for " +
            config.protocol.type);
    }
    if (resolved.pointIntervalMs < resolved.minimumPollIntervalMs) {
        throw std::invalid_argument("timing point interval is below protocol minimum");
    }
    if (resolved.requestGapMs < 0 || resolved.responseTimeoutMs <= 0 ||
        resolved.retryCount < 0 || resolved.interfaceCheckIntervalMs <= 0 ||
        resolved.receiveWaitMs <= 0) {
        throw std::invalid_argument("timing policy contains invalid effective values");
    }
    if (resolved.maxAgeMs > 0 && resolved.maxAgeMs < resolved.pointIntervalMs) {
        throw std::invalid_argument("timingPolicy.acquisition.maxAgeMs must not be lower than the effective point interval");
    }
    return resolved;
}

ResolvedTimingConfig TimingPolicyResolver::apply(
    DeviceConfig& config,
    const TimingPolicyConfig* projectDefault
) {
    const auto resolved = resolve(config, projectDefault);
    config.collect.defaultIntervalMs = resolved.collectIntervalMs;
    config.collect.interfaceCheckIntervalMs = resolved.interfaceCheckIntervalMs;
    config.collect.receiveWaitMs = resolved.receiveWaitMs;
    config.protocol.transport.frameIntervalMs = resolved.requestGapMs;
    config.protocol.transport.timeoutMs = resolved.responseTimeoutMs;
    config.protocol.transport.readRetryCount = resolved.retryCount;
    if (usesTcpSocket(config)) {
        config.protocol.tcp.timeoutMs = resolved.responseTimeoutMs;
    }
    if (resolved.semanticPolicyActive && isIecProtocol(config)) {
        config.protocol.iec.pollTimeoutMs = resolved.responseTimeoutMs;
        config.protocol.iec.idleReadTimeoutMs = resolved.receiveWaitMs;
    }

    for (auto& point : config.points) applyPointTiming(point, resolved);
    for (auto& meter : config.meters) {
        if (resolved.semanticPolicyActive && resolved.maxAgeMs > 0 && !meter.onlineTimeoutExplicit) {
            meter.onlineTimeoutMs = resolved.maxAgeMs;
        }
        for (auto& point : meter.points) applyPointTiming(point, resolved);
    }
    return resolved;
}

void TimingPolicyResolver::applyMqtt(
    MqttDriverConfig& config,
    const TimingPolicyConfig& projectDefault
) {
    if (!projectDefault.configured) return;
    const auto policy = mergePolicies(&projectDefault, TimingPolicyConfig{});
    int scanIntervalMs = config.scanIntervalMs;
    int maxLatencyMs = -1;
    std::string deliveryMode;
    if (hasPreset(policy.profile)) {
        const auto preset = presetFor(policy.profile);
        scanIntervalMs = preset.deliveryBatchWindowMs;
        maxLatencyMs = preset.deliveryMaxLatencyMs;
        deliveryMode = "onChange";
    }
    if (!policy.delivery.mode.empty()) deliveryMode = policy.delivery.mode;
    overridePositive(maxLatencyMs, policy.delivery.maxLatencyMs);
    overrideNonNegative(scanIntervalMs, policy.delivery.batchWindowMs);
    overridePositive(scanIntervalMs, policy.overrides.mqttScanIntervalMs);
    if (maxLatencyMs > 0) scanIntervalMs = std::min(scanIntervalMs, maxLatencyMs);
    if (scanIntervalMs < 10) {
        throw std::invalid_argument("timing MQTT scan interval must be at least 10ms");
    }
    config.deliveryMode = deliveryMode.empty() ? std::string("hybrid") : deliveryMode;
    config.deliveryMaxLatencyMs = maxLatencyMs;
    config.scanIntervalMs = scanIntervalMs;
    if (policy.delivery.heartbeatMs > 0) {
        config.fullUploadIntervalMs = policy.delivery.heartbeatMs;
    } else if (hasPreset(policy.profile)) {
        config.fullUploadIntervalMs = presetFor(policy.profile).deliveryHeartbeatMs;
    }
}

void TimingPolicyResolver::applyAppServices(AppConfig& config) {
    if (!config.timingPolicy.configured) return;
    const auto policy = mergePolicies(&config.timingPolicy, TimingPolicyConfig{});
    applyMqtt(config.mqttDriver, config.timingPolicy);

    int freshnessMs = -1;
    int eventScanMs = config.eventEngine.scanIntervalMs;
    int deliveryMaxLatencyMs = -1;
    std::string deliveryMode;
    if (hasPreset(policy.profile)) {
        const auto preset = presetFor(policy.profile);
        freshnessMs = preset.freshnessMs;
        eventScanMs = preset.deliveryBatchWindowMs;
        deliveryMaxLatencyMs = preset.deliveryMaxLatencyMs;
        deliveryMode = "onChange";
    }
    if (!policy.delivery.mode.empty()) deliveryMode = policy.delivery.mode;
    overridePositive(deliveryMaxLatencyMs, policy.delivery.maxLatencyMs);
    overridePositive(freshnessMs, policy.acquisition.targetFreshnessMs);
    overrideNonNegative(eventScanMs, policy.delivery.batchWindowMs);
    if (deliveryMaxLatencyMs > 0) {
        eventScanMs = std::min(eventScanMs, deliveryMaxLatencyMs);
    }
    if (freshnessMs > 0) {
        config.computeEngine.scanIntervalMs = std::max(10, freshnessMs);
    }
    config.eventEngine.scanIntervalMs = std::max(10, eventScanMs);
    config.eventEngine.deliveryMode = deliveryMode.empty() ? std::string("hybrid") : deliveryMode;
    config.eventEngine.deliveryMaxLatencyMs = deliveryMaxLatencyMs;
}

}  // namespace edge_gateway
