#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

class MqttDriverService {
public:
    MqttDriverService(
        MqttConfig mqttConfig,
        MqttDriverConfig driverConfig,
        std::vector<DeviceConfig> deviceConfigs,
        MemoryPointStore& store,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::unique_ptr<OtaService> otaService = nullptr
    );
    MqttDriverService(
        MqttConfig mqttConfig,
        MqttDriverConfig driverConfig,
        std::vector<DeviceConfig> deviceConfigs,
        PointStoreRouter& router,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::unique_ptr<OtaService> otaService = nullptr
    );
    ~MqttDriverService();

    MqttDriverService(const MqttDriverService&) = delete;
    MqttDriverService& operator=(const MqttDriverService&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    void runScanOnce(std::int64_t nowMs);
    void runEventReplayOnce(std::int64_t nowMs);
    void publishFullSnapshotNow(std::int64_t nowMs);
    void publishOnDemandNow(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs);

private:
    struct PointRoute {
        std::string machineCode;
        std::string meterCode;
        std::string pointCode;
        bool writable = false;
    };

    void processIncomingMessages(std::int64_t nowMs);
    void replayPendingOtaStatuses();
    void replayEventOutboxIfNeeded(std::int64_t nowMs);
    bool shouldDeferSnapshotForEventBacklog(std::int64_t nowMs);
    void handleCommandRequest(const std::string& payload, std::int64_t nowMs);
    void handleOtaRequest(const std::string& payload, std::int64_t nowMs);
    void publishStatusEvent(
        const std::string& event,
        std::int64_t ts,
        const std::string& detailsJson = std::string()
    ) const;
    void scanLoop();
    void replayLoop();
    std::vector<StoredPointValue> filterValues(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs) const;
    void enrichValue(StoredPointValue& value) const;
    std::vector<StoredPointValue> enrichValues(std::vector<StoredPointValue> values) const;

    MqttConfig mqttConfig_;
    MqttDriverConfig driverConfig_;
    std::unique_ptr<PointStoreRouter> ownedRouter_;
    PointStoreRouter& router_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::unique_ptr<OtaService> otaService_;
    std::unordered_set<std::string> machineCodes_;
    std::unordered_map<std::uint32_t, PointRoute> pointRoutes_;
    std::int64_t lastFullUploadMs_ = 0;
    std::int64_t lastEventOutboxReplayMs_ = 0;
    std::int64_t lastOtaReplayAttemptMs_ = 0;
    std::int64_t lastSnapshotDeferredMs_ = 0;
    std::atomic<bool> running_{false};
    std::thread scanThread_;
    std::thread replayThread_;
};

}  // namespace edge_gateway
