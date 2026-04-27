#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

class SystemMonitorService {
public:
    SystemMonitorService(
        SystemMonitorConfig monitorConfig,
        MqttConfig mqttConfig,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::string machineCode,
        std::vector<std::string> configFiles = {},
        PointStoreRouter* router = nullptr
    );
    ~SystemMonitorService();

    SystemMonitorService(const SystemMonitorService&) = delete;
    SystemMonitorService& operator=(const SystemMonitorService&) = delete;

    void start();
    void stop();
    bool isRunning() const;
    void runOnce(std::int64_t nowMs);

private:
    struct Sample {
        double cpuUsage = 0.0;
        double memUsage = 0.0;
        double diskUsage = 0.0;
        double load1 = 0.0;
        int processCount = 0;
    };

    struct MonitorLease {
        std::string sessionId;
        int intervalMs = 5000;
        std::int64_t expireAtMs = 0;
    };

    void loop();
    void processIncomingMessages(std::int64_t nowMs);
    void handleMonitorRequest(const std::string& payload, std::int64_t nowMs);
    void handleDiagRequest(const std::string& payload, std::int64_t nowMs);
    void handleConfigPullRequest(const std::string& payload, std::int64_t nowMs);
    Sample collectSample() const;
    void publishTelemetry(const Sample& sample, std::int64_t nowMs);
    void evaluateAlerts(const Sample& sample, std::int64_t nowMs);
    void publishAlert(
        const std::string& metric,
        double value,
        double threshold,
        bool active,
        std::int64_t nowMs,
        const std::string& message
    );
    void publishReply(const std::string& payload);
    void publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson = std::string()) const;
    void publishPointSnapshot(std::int64_t nowMs);
    bool hasActiveLease(std::int64_t nowMs) const;
    int effectiveIntervalMs(std::int64_t nowMs) const;
    bool isCommandAllowed(const std::string& command) const;
    std::string executeDiagCommand(const std::string& command, const std::string& arg, int* exitCode) const;
    void publishConfigPullReply(const std::string& payload) const;

    SystemMonitorConfig monitorConfig_;
    MqttConfig mqttConfig_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::string machineCode_;
    std::vector<std::string> configFiles_;
    PointStoreRouter* router_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::map<std::string, MonitorLease> leases_;
    std::int64_t lastTelemetryMs_ = 0;
    std::map<std::string, std::int64_t> lastAlertPublishMs_;
    std::uint64_t lastCpuTotal_ = 0;
    std::uint64_t lastCpuIdle_ = 0;
    bool cpuBaselineReady_ = false;
};

}  // namespace edge_gateway
