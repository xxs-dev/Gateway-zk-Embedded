#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ConfigLoader {
public:
    static DeviceConfig loadFromFile(const std::string& filePath);
    static std::vector<DeviceConfig> loadMany(const std::vector<std::string>& filePaths);
    static AppConfig loadAppConfigFromFile(const std::string& filePath);
    static MqttDriverConfig loadMqttDriverConfigFromFile(const std::string& filePath);
    static std::vector<PointDefinition> loadDlt645StandardPointsFromFile(const std::string& filePath);
};

}  // namespace edge_gateway
