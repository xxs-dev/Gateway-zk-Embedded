#pragma once

#include <string>

#include "edge_gateway/scada_models.hpp"

namespace edge_gateway {

class ScadaProjectLoader {
public:
    static ScadaProject loadFromDirectory(const std::string& directory);
    static void validate(const ScadaProject& project);
};

}  // namespace edge_gateway
