#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/collector.hpp"
#include "edge_gateway/command_executor_interface.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/priority_control_lease.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace edge_gateway {

class GatewayDaemon {
public:
    using CollectorFactory = std::function<std::unique_ptr<ICollector>(const DeviceConfig&, MemoryPointStore&)>;
    using CommandExecutorFactory = std::function<std::unique_ptr<ICommandExecutor>(const DeviceConfig&, MemoryPointStore&)>;
    using ServiceStartStop = std::function<void(bool start)>;

    GatewayDaemon(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<Dlt645Client> dlt645Client = nullptr,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr,
        std::shared_ptr<IGpioPort> gpioPort = nullptr,
        std::string realtimeMeterLeaseFile = std::string()
    );

    GatewayDaemon(
        DeviceConfig config,
        MemoryPointStore& store,
        CollectorFactory collectorFactory,
        CommandExecutorFactory commandExecutorFactory,
        ServiceStartStop auxiliaryService,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr,
        std::string realtimeMeterLeaseFile = std::string()
    );
    ~GatewayDaemon();

    GatewayDaemon(const GatewayDaemon&) = delete;
    GatewayDaemon& operator=(const GatewayDaemon&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    void collectOnce(std::int64_t nowMs);
    std::size_t flushPersistentOnce();
    std::size_t processWritebackOnce(std::int64_t nowMs);

private:
    struct RuntimeDevice {
        DeviceConfig config;
        std::unique_ptr<ICollector> collector;
        std::unique_ptr<ICommandExecutor> executor;
    };

    void collectLoop();
    void persistLoop();
    void writebackLoop();
    int collectLoopIntervalMs() const;
    std::size_t collectRuntimeMeterBatchSize() const;
    bool priorityControlBlocked(std::int64_t nowMs) const;
    std::vector<std::size_t> activeRealtimeDeviceIndexes(std::int64_t nowMs);
    std::vector<std::string> activeRealtimeMeterCodes(std::int64_t nowMs);
    void publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson = std::string()) const;
    void initializeRuntimeDevices();

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::vector<RuntimeDevice> runtimeDevices_;
    std::unordered_map<std::uint32_t, std::size_t> indexToRuntimeDevice_;
    std::unordered_map<std::string, std::size_t> meterCodeToRuntimeDevice_;
    std::size_t collectCursor_ = 0;
    std::string realtimeMeterLeaseFile_;
    PriorityControlLease priorityControlLease_;
    std::int64_t realtimeLeaseLastReadMs_ = 0;
    std::int64_t realtimeLeaseExpireAtMs_ = 0;
    std::vector<std::string> realtimeLeaseMeterCodes_;
    SqliteSampleWriter sqliteWriter_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    CollectorFactory collectorFactory_;
    CommandExecutorFactory commandExecutorFactory_;
    ServiceStartStop auxiliaryService_;
    std::atomic<bool> running_{false};
    std::thread collectThread_;
    std::thread persistThread_;
    std::thread writebackThread_;
};

}  // namespace edge_gateway
