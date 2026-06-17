#pragma once

#include <string>

#include "edge_gateway/models.hpp"

namespace edge_gateway {
namespace system_monitor_direct_maintenance {

int runFromConfig(const SystemMonitorConfig::DirectMaintenanceConfig& config);
void requestStop();

}  // namespace system_monitor_direct_maintenance
}  // namespace edge_gateway
