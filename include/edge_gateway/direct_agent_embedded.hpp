#pragma once

#include <string>

namespace edge_gateway {
namespace direct_agent {

int runFromConfigFile(const std::string& configPath);
void requestStop();

}  // namespace direct_agent
}  // namespace edge_gateway
