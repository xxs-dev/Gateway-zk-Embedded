#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class ConfigLoader {
public:
    static DeviceConfig loadFromFile(const std::string& filePath);
    static DeviceConfig loadFromText(const std::string& text);
    static DeviceConfig loadFromFile(const std::string& filePath, const DeviceIdentity& identity);
    static std::vector<DeviceConfig> loadMany(const std::vector<std::string>& filePaths);
    static std::vector<DeviceConfig> loadMany(
        const std::vector<std::string>& filePaths,
        const DeviceIdentity& identity
    );
    static AppConfig loadAppConfigFromFile(const std::string& filePath);
    static DeviceIdentity loadDeviceIdentityFromFile(const std::string& filePath);
    static void applyIdentity(DeviceConfig& config, const DeviceIdentity& identity);
    static MqttDriverConfig loadMqttDriverConfigFromFile(const std::string& filePath);
    static std::vector<PointDefinition> loadDlt645StandardPointsFromFile(const std::string& filePath);
};

}  // namespace edge_gateway
