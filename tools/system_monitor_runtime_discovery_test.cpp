#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/system_monitor_runtime_discovery.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void ensureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("failed to create test dir: " + path);
    }
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write test file: " + path);
    }
    output << content;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

int main() {
    using namespace edge_gateway;

    const std::string root = "/tmp/system-monitor-runtime-discovery-test-" + std::to_string(getpid());
    const std::string apps = root + "/apps";
    const std::string devices = root + "/devices";
    ensureDir(root);
    ensureDir(apps);
    ensureDir(devices);

    const std::string primaryPath = apps + "/monitor-service.json";
    const std::string mqttPath = apps + "/mqtt-service.json";
    const std::string cameraPath = apps + "/camera-service.json";
    const std::string malformedPath = apps + "/malformed.json";
    const std::string physicalDevicePath = devices + "/physical.json";
    const std::string emsDevicePath = devices + "/ems.json";
    writeFile(physicalDevicePath, "{}\n");
    writeFile(emsDevicePath, "{}\n");
    writeFile(
        primaryPath,
        R"({
          "identityConfigFile":"../device_identity.json",
          "deviceConfigFiles":["../devices/physical.json"],
          "mqttDriver":{"enabled":false,"sharedMemoryNames":["monitor_store"]},
          "systemMonitor":{"enabled":true}
        })"
    );
    writeFile(
        mqttPath,
        R"({
          "deviceConfigFiles":["../devices/physical.json","../devices/ems.json"],
          "mqttDriver":{"enabled":true,"sharedMemoryNames":["physical_store","ems_store"]},
          "computeEngine":{
            "enabled":true,
            "sharedMemoryNames":["physical_store","ems_store"],
            "outputDefaultSharedMemoryName":"compute_store"
          },
          "localDisplay":{"enabled":true,"sharedMemoryNames":["display_store"]}
        })"
    );
    writeFile(
        cameraPath,
        R"({"cameraService":{"enabled":true,"sharedMemoryName":"camera_store"}})"
    );
    writeFile(malformedPath, "{not-json");

    const auto primary = ConfigLoader::loadAppConfigFromFile(primaryPath);
    const auto result = discoverSystemMonitorRuntimeDependencies(primaryPath, primary);
    const std::string physicalConfig = apps + "/../devices/physical.json";
    const std::string emsConfig = apps + "/../devices/ems.json";

    require(result.deviceConfigFiles.size() == 2, "device config paths should be deduplicated");
    require(contains(result.deviceConfigFiles, physicalConfig), "physical device config missing");
    require(contains(result.deviceConfigFiles, emsConfig), "EMS device config missing");
    require(contains(result.sharedMemoryNames, "monitor_store"), "primary monitor store missing");
    require(contains(result.sharedMemoryNames, "physical_store"), "MQTT physical store missing");
    require(contains(result.sharedMemoryNames, "ems_store"), "MQTT EMS store missing");
    require(contains(result.sharedMemoryNames, "compute_store"), "compute output store missing");
    require(contains(result.sharedMemoryNames, "display_store"), "local display store missing");
    require(contains(result.sharedMemoryNames, "camera_store"), "camera store missing");
    require(result.cameraServices.size() == 1, "camera routes should be discovered");
    require(result.warnings.size() == 1, "malformed sibling config should produce one warning");
    require(contains(result.configFiles, mqttPath), "sibling app config should be pullable");
    require(contains(result.configFiles, emsConfig), "EMS device config should be pullable");

    unlink(primaryPath.c_str());
    unlink(mqttPath.c_str());
    unlink(cameraPath.c_str());
    unlink(malformedPath.c_str());
    unlink(physicalDevicePath.c_str());
    unlink(emsDevicePath.c_str());
    rmdir(devices.c_str());
    rmdir(apps.c_str());
    rmdir(root.c_str());
    return 0;
}
