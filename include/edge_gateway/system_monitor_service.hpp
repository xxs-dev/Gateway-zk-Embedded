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
        struct CellularStatus {
            bool enabled = false;
            bool present = false;
            bool registered = false;
            bool connected = false;
            bool simReady = false;
            bool toolsAvailable = false;
            std::string operatorName;
            std::string accessTech;
            std::string interfaceName;
            std::string ipAddress;
            std::string gateway;
            std::string dns;
            std::string simStatus;
            std::string imei;
            std::string imsi;
            std::string iccid;
            std::string modemDevice;
            std::string lastError;
            double signalPercent = -1.0;
            double rssiDbm = 0.0;
            double rsrpDbm = 0.0;
            double rsrqDb = 0.0;
            double sinrDb = 0.0;
            std::uint64_t rxBytes = 0;
            std::uint64_t txBytes = 0;
            double rxRateBps = 0.0;
            double txRateBps = 0.0;
            std::int64_t ts = 0;
        };

        double cpuUsage = 0.0;
        double memUsage = 0.0;
        double diskUsage = 0.0;
        double load1 = 0.0;
        int processCount = 0;
        CellularStatus cellular;
    };

    struct MonitorLease {
        std::string sessionId;
        std::string meterCode;
        int intervalMs = 5000;
        std::int64_t expireAtMs = 0;
    };

    void loop();
    void processIncomingMessages(std::int64_t nowMs);
    void handleMonitorRequest(const std::string& payload, std::int64_t nowMs);
    void handleDiagRequest(const std::string& payload, std::int64_t nowMs);
    void handleConfigPullRequest(const std::string& payload, std::int64_t nowMs);
    Sample collectSample() const;
    Sample::CellularStatus collectCellularStatus(std::int64_t nowMs) const;
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
    void writeRealtimeMeterLeases(std::int64_t nowMs) const;
    bool hasActiveLease(std::int64_t nowMs) const;
    bool activeLeaseRequiresAllPoints(std::int64_t nowMs) const;
    std::vector<std::string> activeMeterCodes(std::int64_t nowMs) const;
    int effectiveIntervalMs(std::int64_t nowMs) const;
    bool isCommandAllowed(const std::string& command) const;
    std::string executeDiagCommand(const std::string& command, const std::string& arg, int* exitCode) const;
    void publishConfigPullReply(const std::string& payload) const;
    std::string buildConfigPullReply(const std::string& requestId, std::int64_t nowMs) const;

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
    std::int64_t lastPointSnapshotMs_ = 0;
    std::map<std::string, std::int64_t> lastAlertPublishMs_;
    std::uint64_t lastCpuTotal_ = 0;
    std::uint64_t lastCpuIdle_ = 0;
    bool cpuBaselineReady_ = false;
    mutable std::int64_t lastCellularProbeMs_ = 0;
    mutable Sample::CellularStatus lastCellularStatus_;
};

}  // namespace edge_gateway
