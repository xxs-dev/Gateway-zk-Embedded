#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace edge_gateway {

class CanDriverService {
public:
    CanDriverService(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );
    ~CanDriverService();

    CanDriverService(const CanDriverService&) = delete;
    CanDriverService& operator=(const CanDriverService&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    std::size_t processReceiveOnce(int timeoutMs);
    std::size_t processWritebackOnce(std::int64_t nowMs);
    std::size_t flushPersistentOnce();
    void updateOnlineStatus(std::int64_t nowMs);

private:
    struct RuntimeDevice {
        DeviceConfig config;
        std::int64_t lastSeenTs = 0;
        bool online = false;
        int onlineTimeoutMs = 5000;
        std::vector<std::string> onlineFrameIds;
    };

    struct RuntimePoint {
        std::size_t deviceIndex = 0;
        PointDefinition point;
    };

    void initializeRuntimeDevices();
    void configureInterface() const;
    void openSocket();
    void closeSocket();
    void receiveLoop();
    void writebackLoop();
    void persistLoop();
    void publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson = std::string()) const;
    void publishPointValue(const RuntimeDevice& device, const PointDefinition& point, const DecodedValue& decoded, std::int64_t ts);
    void publishOnlinePoint(RuntimeDevice& device, bool online, std::int64_t ts);
    bool frameBelongsToDevice(const RuntimeDevice& device, std::uint32_t frameId, bool extended) const;
    static std::int64_t nowMs();

    DeviceConfig config_;
    MemoryPointStore& store_;
    SqliteSampleWriter sqliteWriter_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::vector<RuntimeDevice> runtimeDevices_;
    std::vector<RuntimePoint> runtimePoints_;
    std::unordered_map<std::uint32_t, std::size_t> indexToRuntimePoint_;
    std::atomic<bool> running_{false};
    int socketFd_ = -1;
    std::thread receiveThread_;
    std::thread writebackThread_;
    std::thread persistThread_;
};

}  // namespace edge_gateway
