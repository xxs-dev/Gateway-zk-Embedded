#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

// Load-time checks for device configs. Each protocol owns one function in its own file under
// src/validation/. Rule: throw std::invalid_argument (naming meter, pointCode and the field) only
// for definitions that cannot work or would silently produce wrong values; things that work but are
// suspicious go to configWarning(). Every config shipped in the repository must still load.
void validateDeviceConfig(const DeviceConfig& config);

void validateModbusDeviceConfig(const DeviceConfig& config);
void validateDlt645DeviceConfig(const DeviceConfig& config);
void validateIecDeviceConfig(const DeviceConfig& config);
void validateCanDeviceConfig(const DeviceConfig& config);
void validateDioDeviceConfig(const DeviceConfig& config);
void validateNorthboundConfig(const DeviceConfig& config);

// Points of a config: the meters' points when meters are used, else the top-level points.
std::vector<const PointDefinition*> configPoints(const DeviceConfig& config);
std::string pointLabel(const DeviceConfig& config, const PointDefinition& point);
void configWarning(const std::string& message);

}  // namespace edge_gateway
