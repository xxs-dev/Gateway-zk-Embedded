#include "edge_gateway/device_config_validation.hpp"

#include "edge_gateway/modbus_northbound_server.hpp"

namespace edge_gateway {

// A broken northbound definition must not stop collection (the device's main job): it is reported
// here at load time and the northbound server refuses to start with the same message
// (ModbusNorthboundServer::start, status event northbound-refused). Nothing is thrown.
void validateNorthboundConfig(const DeviceConfig& config) {
    for (const auto& problem : northboundConfigProblems(config)) {
        configWarning("northbound server will not start: " + problem);
    }
}

}  // namespace edge_gateway
