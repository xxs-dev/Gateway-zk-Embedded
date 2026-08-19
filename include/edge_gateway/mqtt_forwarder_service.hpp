#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

class MqttForwarderService {
public:
    MqttForwarderService(
        MqttForwardConfig forwardConfig,
        MqttDriverConfig driverConfig,
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

private:
    void publishLoop();
    void writeHealth(
        bool healthy,
        const std::string& error,
        std::size_t valueCount,
        std::int64_t nowMs
    ) const;

    MqttForwardConfig forwardConfig_;
    MqttDriverConfig driverConfig_;
    PointStoreRouter& router_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::string healthFile_;
    std::int64_t lastPublishMs_ = 0;
    std::atomic<bool> running_{false};
    std::thread loopThread_;
};

}  // namespace edge_gateway
