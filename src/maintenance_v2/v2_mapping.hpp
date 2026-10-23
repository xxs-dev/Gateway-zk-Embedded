#pragma once

// Maintenance v2 dry-run for the point mapping (logic/point-mapping.json, CQ15): the mapping that will be on the
// device after this request is checked against the point tables ComputeEngine will load (mqtt-service.json's
// deviceConfigFiles, with the request's new contents) and against the strategy graphs' direct device writes.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace edge_gateway {
namespace maintenance_v2 {

struct MappingPlanCheck {
    bool present = false;  // a mapping file exists after this request
    std::string path;      // runtime/logic/point-mapping.json
    std::vector<std::pair<std::string, std::string>> errors;  // rule, message
    std::vector<std::string> info;
};

MappingPlanCheck checkMappingPlan(const std::string& runtimeDir, const std::map<std::string, std::string>& overrides,
                                  const std::vector<std::string>& deleted, const std::string& machineCode);

}  // namespace maintenance_v2
}  // namespace edge_gateway
