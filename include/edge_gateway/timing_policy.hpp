#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct ResolvedTimingConfig {
    bool semanticPolicyActive = false;
    std::string profile = "custom";
    int minimumPollIntervalMs = 1;
    int collectIntervalMs = 500;
    int pointIntervalMs = 500;
    int requestGapMs = 0;
    int responseTimeoutMs = 1000;
    int retryCount = 1;
    int interfaceCheckIntervalMs = 1000;
    int receiveWaitMs = 50;
    int maxAgeMs = -1;
    std::string deliveryMode;
    int deliveryMaxLatencyMs = -1;
    int deliveryBatchWindowMs = -1;
    int deliveryHeartbeatMs = -1;
};

class TimingPolicyResolver {
public:
    static ResolvedTimingConfig resolve(
        const DeviceConfig& config,
        const TimingPolicyConfig* projectDefault = nullptr
    );

    static ResolvedTimingConfig apply(
        DeviceConfig& config,
        const TimingPolicyConfig* projectDefault = nullptr
    );

    static void applyMqtt(
        MqttDriverConfig& config,
        const TimingPolicyConfig& projectDefault
    );

    static void applyAppServices(AppConfig& config);

    static int minimumPollIntervalMs(const std::string& protocolType);
};

}  // namespace edge_gateway
