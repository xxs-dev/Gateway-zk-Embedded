#include "edge_gateway/mqtt_forwarder_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "edge_gateway/legacy_telemetry_payload.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

void clearControlTopics(MqttConfig& mqtt) {
    mqtt.telemetryTopic.clear();
    mqtt.realtimeTelemetryTopic.clear();
    mqtt.realtimeRequestTopic.clear();
    mqtt.changeEventTopic.clear();
    mqtt.alarmTopic.clear();
    mqtt.statusTopic.clear();
    mqtt.commandRequestTopic.clear();
    mqtt.commandReplyTopic.clear();
    mqtt.otaRequestTopic.clear();
    mqtt.otaReplyTopic.clear();
    mqtt.otaStatusTopic.clear();
    mqtt.systemMonitorRequestTopic.clear();
    mqtt.systemMonitorReplyTopic.clear();
    mqtt.systemMonitorTelemetryTopic.clear();
    mqtt.systemMonitorAlertTopic.clear();
    mqtt.systemMonitorPointTopic.clear();
    mqtt.diagRequestTopic.clear();
    mqtt.diagReplyTopic.clear();
    mqtt.configPullRequestTopic.clear();
    mqtt.configPullReplyTopic.clear();
    mqtt.configApplyRequestTopic.clear();
    mqtt.configApplyReplyTopic.clear();
    mqtt.configDeleteRequestTopic.clear();
    mqtt.configDeleteReplyTopic.clear();
    mqtt.configRestoreRequestTopic.clear();
    mqtt.configRestoreReplyTopic.clear();
    mqtt.recordingRequestTopic.clear();
    mqtt.recordingReplyTopic.clear();
    mqtt.recordingStatusTopic.clear();
    mqtt.recordingAckTopic.clear();
}

}  // namespace

MqttForwarderService::MqttForwarderService(
    MqttForwardConfig forwardConfig,
    PointStoreRouter& router,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::string healthFile
)
    : forwardConfig_(std::move(forwardConfig)),
      router_(router),
      publisher_(std::move(publisher)),
      healthFile_(std::move(healthFile)) {
    if (!publisher_) {
        throw std::invalid_argument("mqtt forwarder requires a publisher");
    }
    if (forwardConfig_.payloadFormat != "compactArray" &&
        forwardConfig_.payloadFormat != "object" &&
        forwardConfig_.payloadFormat != "legacy") {
        throw std::invalid_argument("mqttForward.payloadFormat must be compactArray, object or legacy");
    }
    if (forwardConfig_.enabled && forwardConfig_.pointIndexes.empty()) {
        throw std::invalid_argument("mqttForward.pointIndexes must not be empty when enabled");
    }
    std::unordered_set<std::uint32_t> uniqueIndexes;
    for (const auto index : forwardConfig_.pointIndexes) {
        if (!uniqueIndexes.insert(index).second) {
            throw std::invalid_argument(
                "mqttForward.pointIndexes must not contain duplicate index " + std::to_string(index)
            );
        }
        if (forwardConfig_.enabled && router_.routes().find(index) == router_.routes().end()) {
            throw std::invalid_argument(
                "mqttForward.pointIndexes contains unrouted index " + std::to_string(index)
            );
        }
    }
    std::unordered_set<std::uint32_t> mappedIndexes;
    for (const auto& mapping : forwardConfig_.legacyTelemetryPointMappings) {
        if (!mappedIndexes.insert(mapping.index).second) {
            throw std::invalid_argument(
                "mqttForward.legacyTelemetryPointMappings must not contain duplicate index " +
                std::to_string(mapping.index)
            );
        }
        if (uniqueIndexes.find(mapping.index) == uniqueIndexes.end()) {
            throw std::invalid_argument(
                "mqttForward.legacyTelemetryPointMappings index is missing from pointIndexes: " +
                std::to_string(mapping.index)
            );
        }
    }
    if (forwardConfig_.payloadFormat != "legacy" &&
        (forwardConfig_.legacyTelemetryMappedOnly ||
         !forwardConfig_.legacyTelemetryPointMappings.empty())) {
        throw std::invalid_argument(
            "mqttForward legacy mapping options require payloadFormat=legacy"
        );
    }
    if (forwardConfig_.enabled &&
        forwardConfig_.payloadFormat == "legacy" &&
        forwardConfig_.legacyTelemetryMappedOnly &&
        forwardConfig_.legacyTelemetryPointMappings.empty()) {
        throw std::invalid_argument(
            "mqttForward.legacyTelemetryPointMappings must not be empty when legacyTelemetryMappedOnly=true"
        );
    }
}

MqttForwarderService::~MqttForwarderService() {
    stop();
}

MqttConfig MqttForwarderService::makeTxOnlyMqttConfig(
    const MqttForwardConfig& forwardConfig,
    const std::string& machineCode
) {
    MqttConfig mqtt;
    mqtt.enabled = forwardConfig.enabled;
    mqtt.protocolVersion = forwardConfig.protocolVersion.empty() ? "mqtt3" : forwardConfig.protocolVersion;
    mqtt.broker = forwardConfig.broker;
    mqtt.clientId = forwardConfig.clientId.empty()
        ? (machineCode.empty() ? "forward" : machineCode + "-forward")
        : forwardConfig.clientId;
    mqtt.topicMachineCode = machineCode;
    mqtt.username = forwardConfig.username;
    mqtt.password = forwardConfig.password;
    mqtt.fullTelemetryTopic = forwardConfig.fullTelemetryTopic;
    mqtt.qos = forwardConfig.qos;
    mqtt.controlQos = forwardConfig.qos;
    mqtt.tls = forwardConfig.tls;
    mqtt.offlineBufferEnabled = false;
    clearControlTopics(mqtt);
    return mqtt;
}

void MqttForwarderService::start() {
    if (!forwardConfig_.enabled || running_.exchange(true)) {
        return;
    }
    loopThread_ = std::thread([this]() { publishLoop(); });
}

void MqttForwarderService::stop() {
    running_.store(false);
    if (loopThread_.joinable()) {
        loopThread_.join();
    }
}

bool MqttForwarderService::isRunning() const {
    return running_.load();
}

void MqttForwarderService::runOnce(std::int64_t nowMs) {
    if (!forwardConfig_.enabled) {
        return;
    }
    if (forwardConfig_.intervalMs <= 0) {
        writeHealth(false, "mqttForward.intervalMs must be greater than 0", 0, nowMs);
        return;
    }
    if (lastPublishMs_ != 0 && nowMs - lastPublishMs_ < forwardConfig_.intervalMs) {
        return;
    }
    lastPublishMs_ = nowMs;
    try {
        const auto values = router_.getLatestByIndexesStrict(forwardConfig_.pointIndexes, nowMs);
        if (forwardConfig_.payloadFormat == "legacy") {
            publisher_->publishJsonMessage(
                forwardConfig_.fullTelemetryTopic,
                buildLegacyTelemetryPayload(
                    values,
                    forwardConfig_.legacyTelemetryPointMappings,
                    forwardConfig_.legacyTelemetryMappedOnly,
                    nowMs
                )
            );
        } else {
            publisher_->publishFullSnapshot(
                forwardConfig_.fullTelemetryTopic,
                values,
                forwardConfig_.payloadFormat
            );
        }
        writeHealth(true, {}, values.size(), nowMs);
    } catch (const std::exception& ex) {
        writeHealth(false, ex.what(), 0, nowMs);
        std::cerr << "mqtt forwarder full snapshot failed error=" << ex.what() << std::endl;
    }
}

void MqttForwarderService::publishLoop() {
    while (running_.load()) {
        runOnce(currentTimeMs());
        sleepInterruptibly(running_, std::max(1, forwardConfig_.intervalMs));
    }
}

void MqttForwarderService::writeHealth(
    bool healthy,
    const std::string& error,
    std::size_t valueCount,
    std::int64_t nowMs
) const {
    if (healthFile_.empty()) {
        return;
    }
    const std::string payload =
        std::string("{\"healthy\":") + (healthy ? "true" : "false") +
        ",\"ts\":" + std::to_string(nowMs) +
        ",\"valueCount\":" + std::to_string(valueCount) +
        ",\"error\":\"" + escapeJson(error) + "\"}";
    const std::string tempPath = healthFile_ + ".tmp";
    {
        std::ofstream output(tempPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!output) {
            return;
        }
        output << payload;
        if (!output) {
            return;
        }
    }
    std::remove(healthFile_.c_str());
    std::rename(tempPath.c_str(), healthFile_.c_str());
}

}  // namespace edge_gateway
