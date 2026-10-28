#include "edge_gateway/gateway_daemon.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>

#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/dlt645_collector.hpp"
#include "edge_gateway/modbus_northbound_server.hpp"
#include "edge_gateway/modbus_collector.hpp"

namespace edge_gateway {

GatewayDaemon::GatewayDaemon(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::shared_ptr<IGpioPort> gpioPort,
    std::string realtimeMeterLeaseFile
) : GatewayDaemon(
        std::move(config),
        store,
        [modbusClient, dlt645Client, lineSchedule = std::make_shared<ModbusLineSchedule>()](
            const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            if (runtimeConfig.protocol.type == "dlt645_2007") {
                return std::unique_ptr<ICollector>(
                    new Dlt645Collector(runtimeConfig, runtimeStore, dlt645Client)
                );
            }
            if (runtimeConfig.protocol.type == "local_dio") {
                throw std::invalid_argument("local_dio requires DioDriver");
            }
            // All meters of this daemon share one line.
            return std::unique_ptr<ICollector>(
                new Collector(runtimeConfig, runtimeStore, modbusClient, nullptr, {}, lineSchedule)
            );
        },
        [modbusClient, mqttPublisher, gpioPort](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            return std::unique_ptr<ICommandExecutor>(
                new CommandExecutor(runtimeConfig, runtimeStore, modbusClient, mqttPublisher, gpioPort)
            );
        },
        ServiceStartStop(),
        std::move(mqttPublisher),
        std::move(realtimeMeterLeaseFile)
    ) {
    if (config_.northboundServer.enabled) {
        std::shared_ptr<ModbusNorthboundServer> server(
            new ModbusNorthboundServer(config_, store_)
        );
        auxiliaryService_ = [this, server](bool start) {
            if (start) {
                server->start();
                const auto refusal = server->startError();
                if (!refusal.empty()) {
                    // Surface the refusal on the daemon status channel, not only stderr.
                    std::string escaped;
                    for (const auto ch : refusal) {
                        if (ch == '"' || ch == '\\') {
                            escaped.push_back('\\');
                        }
                        escaped.push_back(ch);
                    }
                    publishStatusEvent(
                        "northbound-refused",
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count(),
                        std::string(R"("message":")") + escaped + "\"");
                }
            } else {
                server->stop();
            }
        };
    }
}

}  // namespace edge_gateway
