#include "edge_gateway/device_config_validation.hpp"
#include "edge_gateway/dlt645_version.hpp"

#include <iostream>

namespace edge_gateway {

std::vector<const PointDefinition*> configPoints(const DeviceConfig& config) {
    std::vector<const PointDefinition*> points;
    for (const auto& meter : config.meters) {
        for (const auto& point : meter.points) {
            points.push_back(&point);
        }
    }
    for (const auto& point : config.points) {
        points.push_back(&point);
    }
    return points;
}

std::string pointLabel(const DeviceConfig& config, const PointDefinition& point) {
    for (const auto& meter : config.meters) {
        for (const auto& candidate : meter.points) {
            if (&candidate == &point) {
                return "meter " + meter.meterCode + " point " + point.pointCode + " (index " +
                    std::to_string(point.index) + ")";
            }
        }
    }
    return "point " + point.pointCode + " (index " + std::to_string(point.index) + ")";
}

void configWarning(const std::string& message) {
    std::cerr << "config warning: " << message << std::endl;
}

void validateDeviceConfig(const DeviceConfig& config) {
    const auto& type = config.protocol.type;
    if (type == "modbus_rtu" || type == "modbus_tcp") {
        validateModbusDeviceConfig(config);
    } else if (isDlt645Protocol(type)) {
        validateDlt645DeviceConfig(config);
    } else if (type.compare(0, 3, "iec") == 0) {
        validateIecDeviceConfig(config);
    } else if (type == "can_socketcan") {
        validateCanDeviceConfig(config);
    } else if (type == "local_dio") {
        validateDioDeviceConfig(config);
    }
    validateNorthboundConfig(config);
}

}  // namespace edge_gateway
