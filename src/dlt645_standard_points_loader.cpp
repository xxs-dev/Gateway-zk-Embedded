#include "edge_gateway/dlt645_standard_points_loader.hpp"

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

std::vector<PointDefinition> Dlt645StandardPointsLoader::loadFromFile(const std::string& filePath) {
    return ConfigLoader::loadDlt645StandardPointsFromFile(filePath);
}

}  // namespace edge_gateway
