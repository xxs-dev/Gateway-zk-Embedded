#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace edge_gateway {

enum class ScadaDeploymentMode {
    Integrated,
    UpperComputer
};

enum class ScadaTagAccess {
    Read,
    Write,
    ReadWrite
};

struct ScadaManifest {
    std::string schemaVersion = "2.0";
    std::string projectId;
    std::string projectName;
    std::string packageVersion = "1.0.0";
    std::string entryScreen;
    std::string packageRole = "project";
};

struct ScadaTopology {
    ScadaDeploymentMode mode = ScadaDeploymentMode::Integrated;
    std::string scadaHost = "edge";
    std::string emsHost = "edge";
    std::string dataTransport = "sharedMemory";
    std::string offlinePolicy = "continueLocal";
    bool retainLocalSafetyRules = true;
    bool requireFreshLeaseForControl = true;
    int offlineTimeoutMs = 10000;
    std::string offlineAction = "zeroPower";
};

struct ScadaNode {
    std::string nodeId;
    std::string machineCode;
    std::string displayName;
    std::string connectionProfileId;
    std::vector<std::string> roles;
};

struct ScadaTag {
    std::string tagId;
    std::string nodeId;
    std::string deviceId;
    std::string meterCode;
    std::string pointCode;
    std::string semanticRole;
    std::string displayName;
    std::string unit;
    std::string dataType = "float64";
    ScadaTagAccess access = ScadaTagAccess::Read;
    std::uint32_t indexFallback = 0;
};

struct ScadaRuntimeMapping {
    std::string nodeId;
    std::string tagId;
    std::string sharedMemoryName;
    std::uint32_t index = 0;
    bool writable = false;
    std::string dataType = "float64";
    std::string unit;
};

struct ScadaGeometry {
    double x = 0.0;
    double y = 0.0;
    double width = 180.0;
    double height = 92.0;
};

struct ScadaTagReference {
    std::string nodeId;
    std::string tagId;
    std::string slot = "value";
};

struct ScadaWidgetAction {
    std::string type = "none";
    std::string targetScreen;
    std::string nodeId;
    std::string tagId;
    std::string value;
    bool requiresConfirmation = true;
    bool highPriority = false;
};

struct ScadaStateCondition {
    std::string nodeId;
    std::string tagId;
    std::string comparison = "eq";
    std::string value;
};

struct ScadaStateRule {
    std::string code;
    std::string label;
    std::string color = "#AAB3BD";
    std::string image;
    int priority = 0;
    std::string match = "all";
    std::vector<ScadaStateCondition> conditions;
};

struct ScadaWidget {
    std::string widgetId;
    std::string type = "value";
    std::string title;
    ScadaGeometry geometry;
    int zIndex = 0;
    bool visible = true;
    std::vector<ScadaTagReference> bindings;
    std::vector<ScadaStateRule> stateRules;
    ScadaWidgetAction action;
    std::unordered_map<std::string, std::string> properties;
};

struct ScadaScreen {
    std::string screenId;
    std::string title;
    int width = 1920;
    int height = 1080;
    std::string background;
    std::vector<ScadaWidget> widgets;
};

struct ScadaAlarm {
    std::string alarmId;
    std::string nodeId;
    std::string tagId;
    std::string severity = "warning";
    std::string comparison = "eq";
    std::string threshold;
    int delayMs = 0;
    double deadband = 0.0;
    bool requiresAcknowledgement = false;
};

struct ScadaTrend {
    std::string trendId;
    int sampleIntervalMs = 1000;
    int maxPoints = 3600;
    std::vector<ScadaTagReference> series;
};

struct ScadaProject {
    ScadaManifest manifest;
    ScadaTopology topology;
    std::vector<ScadaNode> nodes;
    std::vector<ScadaTag> tags;
    std::vector<ScadaRuntimeMapping> runtimeMappings;
    std::vector<ScadaScreen> screens;
    std::vector<ScadaAlarm> alarms;
    std::vector<ScadaTrend> trends;
    std::string rootDirectory;
};

}  // namespace edge_gateway
