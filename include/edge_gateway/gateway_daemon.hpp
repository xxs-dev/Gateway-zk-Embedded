#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/collector.hpp"
#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace edge_gateway {

class GatewayDaemon {
public:
    GatewayDaemon(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<Dlt645Client> dlt645Client = nullptr,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr,
        std::shared_ptr<IGpioPort> gpioPort = nullptr
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
        std::unique_ptr<Collector> collector;
        std::unique_ptr<CommandExecutor> executor;
    };

    void collectLoop();
    void persistLoop();
    void writebackLoop();
    int collectLoopIntervalMs() const;
    void publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson = std::string()) const;
    void initializeRuntimeDevices(
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<Dlt645Client> dlt645Client,
        std::shared_ptr<IMqttPublisher> mqttPublisher,
        std::shared_ptr<IGpioPort> gpioPort
    );

    DeviceConfig config_;
    MemoryPointStore& store_;
    std::vector<RuntimeDevice> runtimeDevices_;
    std::unordered_map<std::uint32_t, std::size_t> indexToRuntimeDevice_;
    SqliteSampleWriter sqliteWriter_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::shared_ptr<IGpioPort> gpioPort_;
    std::atomic<bool> running_{false};
    std::thread collectThread_;
    std::thread persistThread_;
    std::thread writebackThread_;
};

}  // namespace edge_gateway
