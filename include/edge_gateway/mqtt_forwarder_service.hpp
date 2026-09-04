#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

class PowerControlOwnership;

class MqttForwarderService {
public:
    MqttForwarderService(
        MqttForwardConfig forwardConfig,
        PointStoreRouter& router,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::string healthFile = {}
    );
    ~MqttForwarderService();

    MqttForwarderService(const MqttForwarderService&) = delete;
    MqttForwarderService& operator=(const MqttForwarderService&) = delete;

    void start();
    void stop();
    bool isRunning() const;
    void runOnce(std::int64_t nowMs);

    static MqttConfig makeTxOnlyMqttConfig(
        const MqttForwardConfig& forwardConfig,
        const std::string& machineCode
    );
    static MqttConfig makeMqttConfig(
        const MqttForwardConfig& forwardConfig,
        const std::string& machineCode
    );
    static MqttConfig makePrimaryFullMqttConfig(
        const MqttConfig& primaryConfig,
        const MqttFullUploadWorkerConfig& workerConfig,
        const std::string& machineCode
    );

private:
    void publishLoop();
    void pollIncomingCommands(std::int64_t nowMs);
    void handleCommandMessage(const MqttIncomingMessage& message, std::int64_t nowMs);
    void publishControlReply(
        const std::string& id,
        int type,
        bool accepted,
        bool duplicate,
        const std::string& mode,
        bool hasTargetKw,
        double targetKw,
        std::uint32_t generation,
        const std::string& message,
        std::int64_t nowMs
    ) const;
    void releaseOwnSession();
    bool writeHealth(
        bool healthy,
        const std::string& error,
        std::size_t valueCount,
        std::int64_t nowMs
    ) const;

    MqttForwardConfig forwardConfig_;
    PointStoreRouter& router_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::string healthFile_;
    std::int64_t lastPublishMs_ = 0;
    std::int64_t lastAttemptMs_ = 0;
    std::int64_t nextAttemptMs_ = 0;
    mutable std::int64_t lastHealthMs_ = 0;
    int retryDelayMs_ = 0;
    std::uint64_t consecutiveFailures_ = 0;
    bool lastHealthy_ = false;
    bool publishInProgress_ = false;
    std::size_t lastValueCount_ = 0;
    std::string lastError_;
    bool scheduleInitialized_ = false;
    std::atomic<bool> running_{false};
    std::thread loopThread_;
    std::unique_ptr<PowerControlOwnership> ownership_;
};

}  // namespace edge_gateway
