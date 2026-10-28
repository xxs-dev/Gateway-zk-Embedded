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
        [modbusClient, dlt645Client](const DeviceConfig& runtimeConfig, MemoryPointStore& runtimeStore) {
            if (runtimeConfig.protocol.type == "dlt645_2007") {
                return std::unique_ptr<ICollector>(
                    new Dlt645Collector(runtimeConfig, runtimeStore, dlt645Client)
                );
            }
            if (runtimeConfig.protocol.type == "local_dio") {
                throw std::invalid_argument("local_dio requires DioDriver");
            }
            return std::unique_ptr<ICollector>(
                new Collector(runtimeConfig, runtimeStore, modbusClient)
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
        // Northbound problems (config refusal, bind failure) are reported on the daemon status
        // channel and never stop collection.
        std::shared_ptr<ModbusNorthboundServer> server(new ModbusNorthboundServer(
            config_,
            store_,
            [this](const std::string& event, const std::string& message) {
                std::string escaped;
                for (const auto ch : message) {
                    if (ch == '"' || ch == '\\') {
                        escaped.push_back('\\');
                    }
                    escaped.push_back(static_cast<unsigned char>(ch) < 0x20 ? ' ' : ch);
                }
                publishStatusEvent(
                    event,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count(),
                    std::string(R"("message":")") + escaped + "\"");
            }
        ));
        auxiliaryService_ = [server](bool start) {
            if (start) {
                server->start();
            } else {
                server->stop();
            }
        };
    }
}

}  // namespace edge_gateway
