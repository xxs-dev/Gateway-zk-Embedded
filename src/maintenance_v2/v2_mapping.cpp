// Maintenance v2 dry-run for the point mapping (see v2_mapping.hpp).
#include "v2_mapping.hpp"

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/point_mapping.hpp"
#include "v2_files.hpp"

#include <algorithm>
#include <fstream>

namespace edge_gateway {
namespace maintenance_v2 {

namespace {

// Content a path will have after the request: "" + false when it will not exist.
bool effective(const std::string& path, const std::map<std::string, std::string>& overrides, const std::vector<std::string>& deleted,
               std::string* out) {
    if (std::find(deleted.begin(), deleted.end(), path) != deleted.end()) return false;
    const auto it = overrides.find(path);
    if (it != overrides.end()) {
        *out = it->second;
        return true;
    }
    return isRegularFile(path) && readFile(path, out);
}

}  // namespace

MappingPlanCheck checkMappingPlan(const std::string& runtimeDir, const std::map<std::string, std::string>& overrides,
                                  const std::vector<std::string>& deleted, const std::string& machineCode) {
    MappingPlanCheck c;
    c.path = runtimeDir + "/logic/point-mapping.json";
    std::string text;
    if (!effective(c.path, overrides, deleted, &text)) return c;
    c.present = true;
    PointMapping mapping;
    try {
        mapping = parsePointMapping(text);
    } catch (const std::exception& ex) {
        c.errors.emplace_back("mapping.format", ex.what());
        return c;
    }
    // ComputeEngine's app config as it will be (a changed one is loaded from a temporary file next to it).
    const auto appPath = runtimeDir + "/apps/mqtt-service.json";
    AppConfig app;
    try {
        std::string appText;
        const auto it = overrides.find(appPath);
        if (it != overrides.end()) {
            const auto temp = appPath + ".mapping-check." + std::to_string(nowMs());
            {
                std::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
                out << it->second;
            }
            try {
                app = ConfigLoader::loadAppConfigFromFile(temp);
            } catch (...) {
                removeFile(temp);
                throw;
            }
            removeFile(temp);
        } else {
            app = ConfigLoader::loadAppConfigFromFile(appPath);
        }
    } catch (const std::exception& ex) {
        c.errors.emplace_back("mapping.points", std::string("cannot load mqtt-service.json (ComputeEngine's point tables): ") + ex.what());
        return c;
    }
    std::vector<DeviceConfig> devices;
    for (const auto& path : app.deviceConfigFiles) {
        std::string deviceText;
        if (!effective(path, overrides, deleted, &deviceText)) continue;  // a missing device file is the loader's error
        try {
            devices.push_back(ConfigLoader::loadFromText(deviceText));
        } catch (const std::exception& ex) {
            c.errors.emplace_back("mapping.points", baseName(path) + ": " + ex.what());
        }
    }
    const auto targets = pointMappingTargets(devices);
    for (const auto& p : checkPointMapping(mapping, targets, machineCode)) c.errors.emplace_back(p.rule, p.message);
    for (const auto& p : checkPointMappingCluster(mapping, app.emsCluster)) c.errors.emplace_back(p.rule, p.message);

    // With a mapping, graphs may only write EMS points (ComputeEngine refuses their device writes at run time).
    if (app.computeEngine.enabled) {
        for (const auto& rule : app.computeEngine.rules) {
            if (!rule.enabled || rule.script.type != "graphEms" || rule.script.graphFile.empty()) continue;
            std::string graph;
            if (!effective(rule.script.graphFile, overrides, deleted, &graph)) continue;
            std::vector<GraphDeviceWrite> writes;
            try {
                writes = graphWriteTargets(graph);
            } catch (const std::exception& ex) {
                c.errors.emplace_back("mapping.graphWrites", baseName(rule.script.graphFile) + ": " + ex.what());
                continue;
            }
            for (const auto& w : writes) {
                const auto t = targets.find(w.index);
                if (t != targets.end() && t->second.protocol == "ems_virtual") continue;
                c.errors.emplace_back("mapping.graphWrites", baseName(rule.script.graphFile) + " node " + w.nodeId + " (" + w.nodeType + ") writes device point " +
                                                                 std::to_string(w.index) + " directly; with a point mapping, write an EMS output point and map it");
            }
        }
    } else {
        c.info.push_back("computeEngine is disabled in mqtt-service.json: the mapping runs once ComputeEngine is enabled");
    }
    std::size_t in = 0, out = 0;
    for (const auto& e : mapping.entries) (e.output ? out : in) += 1;
    if (c.errors.empty()) c.info.push_back(std::to_string(in) + " inputs and " + std::to_string(out) + " outputs match the point tables");
    return c;
}

}  // namespace maintenance_v2
}  // namespace edge_gateway
