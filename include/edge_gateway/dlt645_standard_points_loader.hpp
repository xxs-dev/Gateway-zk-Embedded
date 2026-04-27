#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class Dlt645StandardPointsLoader {
public:
    static std::vector<PointDefinition> loadFromFile(const std::string& filePath);
};

}  // namespace edge_gateway
