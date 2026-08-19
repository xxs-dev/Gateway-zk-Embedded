#include "edge_gateway/config_loader.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string tempPath() {
#ifdef _WIN32
    char buffer[L_tmpnam] = {};
    if (std::tmpnam(buffer) == nullptr) {
        throw std::runtime_error("failed to create temp file name");
    }
    return std::string(buffer);
#else
    return "/tmp/gateway_config_loader_test.json";
#endif
}

void verifyDeviceCollectBackgroundTaskConfig() {
    const auto config = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"MTR_TEST\","
        "\"deviceName\":\"Meter Test\","
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1,"
        "\"transport\":{\"frameIntervalMs\":500,\"readRetryCount\":2,\"wakeupBytes\":4}},"
        "\"collect\":{"
        "\"maxBatchRegisters\":16,"
        "\"maxRequestRegisters\":16,"
        "\"maxTasksPerMeterPerCycle\":2,"
        "\"realtimeMaxTasksPerMeterPerCycle\":7,"
        "\"adaptiveSplitLeafProbeBudget\":4,"
        "\"realtimeAdaptiveSplitLeafProbeBudget\":11,"
        "\"backgroundTaskIntervalMs\":2500,"
        "\"maxBackgroundTasksPerMeterPerCycle\":3,"
        "\"failureBadQualityThreshold\":5,"
        "\"realtimeMaxBackgroundTasksPerMeterPerCycle\":9"
        "},"
        "\"meters\":[]"
        "}"
    );
    require(config.collect.realtimeMaxTasksPerMeterPerCycle == 7,
        "realtime task budget should parse");
    require(config.collect.realtimeAdaptiveSplitLeafProbeBudget == 11,
        "realtime adaptive split budget should parse");
    require(config.collect.backgroundTaskIntervalMs == 2500, "background task interval should parse");
    require(
        config.collect.maxBackgroundTasksPerMeterPerCycle == 3,
        "background task budget should parse"
    );
    require(
        config.collect.realtimeMaxBackgroundTasksPerMeterPerCycle == 9,
        "realtime background task budget should parse"
    );
    require(config.collect.failureBadQualityThreshold == 5,
        "failure bad-quality threshold should parse");
    require(config.protocol.transport.frameIntervalMs == 500,
        "frame interval should parse");
    require(config.protocol.transport.readRetryCount == 2,
        "read retry count should parse");
    require(config.protocol.transport.wakeupBytes == 4,
        "serial wakeup byte count should parse");

    const auto pointPriority = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"MTR_TEST\","
        "\"deviceName\":\"Meter Test\","
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1},"
        "\"points\":[{"
        "\"index\":1001,"
        "\"pointCode\":\"P1\","
        "\"name\":\"Point 1\","
        "\"collectPriority\":2,"
        "\"read\":{\"enable\":true}"
        "}],"
        "\"meters\":[]"
        "}"
    );
    require(
        pointPriority.points.size() == 1 && pointPriority.points.front().collectPriority == 2,
        "point collect priority should parse"
    );

    const auto normalized = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"MTR_TEST\","
        "\"deviceName\":\"Meter Test\","
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1},"
        "\"collect\":{"
        "\"maxBatchRegisters\":500,"
        "\"maxRequestRegisters\":500,"
        "\"maxTasksPerMeterPerCycle\":4,"
        "\"realtimeMaxTasksPerMeterPerCycle\":1,"
        "\"adaptiveSplitLeafProbeBudget\":5,"
        "\"realtimeAdaptiveSplitLeafProbeBudget\":1,"
        "\"backgroundTaskIntervalMs\":-1,"
        "\"maxBackgroundTasksPerMeterPerCycle\":2,"
        "\"failureBadQualityThreshold\":0,"
        "\"realtimeMaxBackgroundTasksPerMeterPerCycle\":-1"
        "},"
        "\"meters\":[]"
        "}"
    );
    require(
        normalized.collect.backgroundTaskIntervalMs == 0,
        "negative background task interval should normalize to zero"
    );
    require(
        normalized.collect.realtimeMaxTasksPerMeterPerCycle == 4,
        "small realtime task budget should normalize to the normal task budget"
    );
    require(
        normalized.collect.maxBackgroundTasksPerMeterPerCycle == 2,
        "normal background task budget should remain configured"
    );
    require(
        normalized.collect.realtimeMaxBackgroundTasksPerMeterPerCycle == 2,
        "small realtime background budget should normalize to the normal background budget"
    );
    require(
        normalized.collect.realtimeAdaptiveSplitLeafProbeBudget == 5,
        "small realtime adaptive split budget should normalize to the normal split budget"
    );
    require(
        normalized.collect.failureBadQualityThreshold == 1,
        "small failure bad-quality threshold should normalize to one"
    );
    require(normalized.collect.maxRequestRegisters == 125,
        "Modbus register request limit should normalize to 125");
    require(normalized.collect.maxBatchRegisters == 125,
        "Modbus batch limit should normalize to the request limit");

    bool rejectedInvalidTcpTimeout = false;
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(
            "{"
            "\"schemaVersion\":\"1.1.0\","
            "\"machineCode\":\"GW_TEST\","
            "\"meterCode\":\"MTR_TEST\","
            "\"deviceName\":\"Meter Test\","
            "\"protocol\":{\"type\":\"modbus_tcp\",\"slave\":1,"
            "\"tcp\":{\"host\":\"127.0.0.1\",\"port\":502,\"connectTimeoutMs\":0,\"timeoutMs\":0}},"
            "\"meters\":[]"
            "}"
        );
    } catch (const std::invalid_argument&) {
        rejectedInvalidTcpTimeout = true;
    }
    require(rejectedInvalidTcpTimeout, "non-positive Modbus TCP timeouts should be rejected");

    const auto virtualDevice = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"EMS_CORE\","
        "\"deviceName\":\"EMS Core\","
        "\"protocol\":{\"type\":\"ems_virtual\",\"slave\":1,"
        "\"tcp\":{\"host\":\"127.0.0.1\",\"port\":0,\"connectTimeoutMs\":1000,\"timeoutMs\":1000}},"
        "\"meters\":[]"
        "}"
    );
    require(virtualDevice.protocol.tcp.port == 0,
        "virtual devices should preserve a zero TCP placeholder port");

    bool rejectedInvalidTcpPort = false;
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(
            "{"
            "\"schemaVersion\":\"1.1.0\","
            "\"machineCode\":\"GW_TEST\","
            "\"meterCode\":\"MTR_TEST\","
            "\"deviceName\":\"Meter Test\","
            "\"protocol\":{\"type\":\"modbus_tcp\",\"slave\":1,"
            "\"tcp\":{\"host\":\"127.0.0.1\",\"port\":0,\"connectTimeoutMs\":1000,\"timeoutMs\":1000}},"
            "\"meters\":[]"
            "}"
        );
    } catch (const std::invalid_argument&) {
        rejectedInvalidTcpPort = true;
    }
    require(rejectedInvalidTcpPort, "Modbus TCP should reject a zero endpoint port");
}

void verifyTimingPolicyConfig() {
    const auto config = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"MTR_TEST\","
        "\"deviceName\":\"Meter Test\","
        "\"timingPolicy\":{"
        "\"profile\":\"highFrequency\","
        "\"acquisition\":{\"targetFreshnessMs\":150,\"maxAgeMs\":2000},"
        "\"delivery\":{\"mode\":\"onChange\",\"batchWindowMs\":20,\"heartbeatMs\":60000},"
        "\"overrides\":{\"requestGapMs\":3,\"responseTimeoutMs\":250,\"retryCount\":2,\"receiveWaitMs\":15}"
        "},"
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1},"
        "\"meters\":[{\"meterCode\":\"M1\",\"deviceName\":\"Meter 1\",\"onlineTimeoutMs\":7000,\"points\":["
        "{\"index\":1,\"pointCode\":\"p1\",\"enabled\":true,\"read\":{\"enable\":true,\"intervalMs\":750,\"cachePolicy\":{\"ttlMs\":9000},\"can\":{\"receiveTimeoutMs\":8000}}}"
        "]}]"
        "}"
    );
    require(config.timingPolicy.configured, "timing policy should be marked configured");
    require(config.timingPolicy.profile == "highFrequency", "timing profile should parse");
    require(config.timingPolicy.acquisition.targetFreshnessMs == 150, "timing freshness should parse");
    require(config.timingPolicy.overrides.requestGapMs == 3, "timing request gap should parse");
    const auto& meter = config.meters.front();
    require(meter.onlineTimeoutExplicit, "explicit meter online timeout should be tracked");
    require(meter.points.front().read.intervalExplicit, "explicit point interval should be tracked");
    require(meter.points.front().read.cachePolicy.ttlExplicit, "explicit point TTL should be tracked");
    require(meter.points.front().read.can.receiveTimeoutExplicit, "explicit CAN timeout should be tracked");
}

void verifyNorthboundConfig() {
    const auto config = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"MTR_TEST\","
        "\"deviceName\":\"Meter Test\","
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1},"
        "\"northboundServer\":{"
        "\"enabled\":true,"
        "\"mode\":\"mapped\","
        "\"protocol\":\"modbus_tcp\","
        "\"bindHost\":\"127.0.0.1\","
        "\"port\":1502,"
        "\"requestTimeoutMs\":500,"
        "\"maxClients\":4,"
        "\"writesEnabled\":false,"
        "\"allowedClientCidrs\":[\"127.0.0.1/32\"]"
        "},"
        "\"points\":[{"
        "\"index\":1001,"
        "\"pointCode\":\"P1\","
        "\"name\":\"Point 1\","
        "\"read\":{\"enable\":true,\"length\":2,\"dataType\":\"float32\",\"scale\":0.1,\"byteOrder\":\"ABCD\"},"
        "\"northbound\":{"
        "\"enabled\":true,"
        "\"unitId\":2,"
        "\"area\":\"input_register\","
        "\"address\":300,"
        "\"length\":2,"
        "\"dataType\":\"float32\","
        "\"scale\":1,"
        "\"offset\":0,"
        "\"byteOrder\":\"ABCD\","
        "\"stalePolicy\":\"zero\""
        "}"
        "}],"
        "\"meters\":[]"
        "}"
    );
    require(config.northboundServer.enabled, "northbound server should parse");
    require(config.northboundServer.bindHost == "127.0.0.1", "northbound bind host should parse");
    require(config.northboundServer.port == 1502, "northbound port should parse");
    require(config.northboundServer.allowedClientCidrs.size() == 1, "northbound cidr list should parse");
    require(config.points.size() == 1, "northbound point should parse");
    require(config.points.front().northbound.enabled, "northbound mapping should parse");
    require(config.points.front().northbound.readFunction == 4, "northbound area should infer function 4");
    require(config.points.front().northbound.address == 300, "northbound address should parse");
    require(config.points.front().northbound.stalePolicy == "zero", "northbound stale policy should parse");
}

void verifyDlt645WriteConfig() {
    const auto config = edge_gateway::ConfigLoader::loadFromText(
        R"JSON({
          "schemaVersion": "1.1.0",
          "machineCode": "GW_DLT645",
          "meterCode": "BREAKER_20",
          "deviceName": "Breaker",
          "protocol": { "type": "dlt645_2007" },
          "dlt645": {
            "write": {
              "enabled": true,
              "password": "02000000",
              "operatorCode": "01020304"
            }
          },
          "points": [{
            "index": 200001,
            "pointCode": "breaker_remote_trip",
            "name": "Remote trip",
            "enabled": true,
            "write": {
              "enable": true,
              "min": 0,
              "max": 99,
              "step": 1,
              "dlt645": {
                "di": "06010101",
                "dataType": "dlt645_scheduled_control",
                "byteCount": 2,
                "unit": 2
              }
            }
          }],
          "meters": []
        })JSON"
    );
    require(config.protocol.dlt645.write.enabled, "DLT645 protocol write enable should parse");
    require(config.protocol.dlt645.write.password == "02000000", "DLT645 password should parse");
    require(config.protocol.dlt645.write.operatorCode == "01020304", "DLT645 operator code should parse");
    require(config.points.size() == 1, "DLT645 writable point should parse");
    require(config.points.front().write.dlt645.di == "06010101", "DLT645 write DI should parse");
    require(
        config.points.front().write.dlt645.dataType == "dlt645_scheduled_control",
        "DLT645 write data type should parse"
    );
    require(config.points.front().write.dlt645.byteCount == 2, "DLT645 write byte count should parse");
    require(config.points.front().write.dlt645.unit == 2, "DLT645 write unit should parse");

    bool missingPasswordRejected = false;
    try {
        (void)edge_gateway::ConfigLoader::loadFromText(
            R"JSON({
              "protocol": { "type": "dlt645_2007" },
              "dlt645": { "write": { "enabled": true, "operatorCode": "00000000" } },
              "meters": []
            })JSON"
        );
    } catch (const std::invalid_argument&) {
        missingPasswordRejected = true;
    }
    require(missingPasswordRejected, "enabled DLT645 write without password must be rejected");

    const auto templatePath = tempPath();
    std::ofstream templateOutput(templatePath.c_str(), std::ios::binary | std::ios::trunc);
    templateOutput << R"JSON({
      "points": [{
        "pointCode": "breaker_remote_close",
        "name": "Remote close",
        "desc": "",
        "access": "write",
        "category": "command",
        "di": "",
        "dataType": "dlt645_scheduled_control",
        "byteCount": 0,
        "enabledByDefault": true,
        "write": {
          "enable": true,
          "min": 0,
          "max": 99,
          "step": 1,
          "dlt645": {
            "di": "06010201",
            "dataType": "dlt645_scheduled_control",
            "byteCount": 2,
            "unit": 2
          }
        }
      }, {
        "pointCode": "device_online",
        "name": "Breaker online",
        "desc": "",
        "category": "status",
        "di": "",
        "dataType": "device_online",
        "byteCount": 0,
        "storeLatest": true,
        "storeHistory": false,
        "reportOnChange": true,
        "enabledByDefault": true
      }]
    })JSON";
    templateOutput.close();
    const auto templatePoints = edge_gateway::ConfigLoader::loadDlt645StandardPointsFromFile(templatePath);
    std::remove(templatePath.c_str());
    require(templatePoints.size() == 2, "DLT645 standard template control and online points should load");
    require(!templatePoints.front().read.enable, "DLT645 write-only template point must not be collected");
    require(templatePoints.front().write.enable, "DLT645 template write enable should load");
    require(
        templatePoints.front().write.dlt645.di == "06010201",
        "DLT645 standard template write DI should load"
    );
    require(templatePoints.back().read.enable, "DLT645 online point should be registered as readable");
    require(
        templatePoints.back().read.dataType == "device_online",
        "DLT645 online point data type should load"
    );
    require(templatePoints.back().index == 2, "DLT645 standard point order must remain stable");
}

void verifyDlt645StandardPointValueMap() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
          "protocol": "dlt645_2007",
          "points": [{
            "pointCode": "breaker_alarm_overcurrent",
            "name": "过流告警",
            "category": "alarm",
            "di": "04001504",
            "dataType": "dlt645_bitfield_le",
            "byteCount": 12,
            "bit": 20,
            "valueMap": { "0": "正常", "1": "过流告警" }
          }]
        })JSON";
    output.close();
    const auto points = edge_gateway::ConfigLoader::loadDlt645StandardPointsFromFile(path);
    std::remove(path.c_str());

    require(points.size() == 1, "DLT645 standard point should parse");
    require(points.front().read.bit == 20, "DLT645 standard point bit should parse");
    require(points.front().valueMap.size() == 2, "DLT645 standard point valueMap should parse");
    require(points.front().valueMap.at("0") == "正常", "DLT645 normal label should parse");
    require(points.front().valueMap.at("1") == "过流告警", "DLT645 alarm label should parse");
}

void verifyPointNormalizeConfig() {
    const auto config = edge_gateway::ConfigLoader::loadFromText(
        "{"
        "\"schemaVersion\":\"1.1.0\","
        "\"machineCode\":\"GW_TEST\","
        "\"meterCode\":\"PCS_1\","
        "\"deviceName\":\"PCS\","
        "\"protocol\":{\"type\":\"modbus_rtu\",\"slave\":1},"
        "\"points\":[{"
        "\"index\":1001,"
        "\"pointCode\":\"PCS_VENDOR_RUN_STATE\","
        "\"name\":\"vendor state\","
        "\"fullUpload\":true,"
        "\"read\":{\"enable\":true,\"dataType\":\"uint16\"},"
        "\"normalize\":{"
        "\"enabled\":true,"
        "\"type\":\"enum\","
        "\"targetIndex\":910001,"
        "\"targetPointCode\":\"PCS_RUN_STATE_STD\","
        "\"targetSemanticRole\":\"pcs.runState\","
        "\"targetName\":\"运行状态\","
        "\"unknownValue\":255,"
        "\"unknownLabel\":\"未知\","
        "\"mappings\":["
        "{\"rawValue\":1,\"rawLabel\":\"停止\",\"standardValue\":0,\"standardLabel\":\"停机\"},"
        "{\"rawValue\":\"2\",\"rawLabel\":\"待机\",\"standardValue\":1,\"standardLabel\":\"待机\"}"
        "],"
        "\"faultRules\":["
        "{\"sourcePointCode\":\"PCS_FAULT\",\"triggerValue\":\"1\",\"standardValue\":3,\"standardLabel\":\"故障\"}"
        "]"
        "}"
        "}],"
        "\"meters\":[]"
        "}"
    );

    require(config.points.size() == 1, "normalize point should parse");
    const auto& normalize = config.points.front().normalize;
    require(normalize.enabled, "normalize enabled should parse");
    require(normalize.type == "enum", "normalize type should parse");
    require(normalize.targetIndex == 910001, "normalize target index should parse");
    require(normalize.targetPointCode == "PCS_RUN_STATE_STD", "normalize target pointCode should parse");
    require(normalize.targetSemanticRole == "pcs.runState", "normalize semantic role should parse");
    require(normalize.unknownValue == 255.0, "normalize unknown value should parse");
    require(normalize.mappings.size() == 2, "normalize mappings should parse");
    require(normalize.mappings.front().rawValue == "1", "numeric raw value should parse as string");
    require(normalize.mappings.back().standardValue == 1.0, "standard value should parse");
    require(normalize.faultRules.size() == 1, "normalize fault rules should parse");
    require(normalize.faultRules.front().sourcePointCode == "PCS_FAULT", "fault source point should parse");
}

void verifyLocalDisplayStateBindingConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "localDisplay": {
        "enabled": true,
        "layout": {
          "pages": [{
            "pageCode": "overview",
            "widgets": [{
              "id": "battery_state",
              "type": "statusLamp",
              "stateBinding": {
                "labelWidget": "BatteryStateText",
                "defaultState": { "code": "UNKNOWN", "label": "unknown", "color": "#AAB3BD" },
                "states": [
                  {
                    "code": "RUNNING", "label": "running", "color": "#2F9D78", "priority": 50, "match": "all",
                    "conditions": [{ "meterCode": "BMS_1", "pointCode": "run", "index": 1454, "operator": "in", "value": "1,2" }]
                  },
                  {
                    "code": "FAULT", "label": "fault", "color": "#D64545", "priority": 100, "match": "any",
                    "conditions": [{ "meterCode": "BMS_1", "pointCode": "fault", "index": 1453, "operator": "eq", "value": "1" }]
                  }
                ]
              }
            }]
          }]
        }
      }
    })JSON";
    output.close();

    const auto config = edge_gateway::ConfigLoader::loadAppConfigFromFile(path).localDisplay;
    std::remove(path.c_str());
    require(config.layout.pages.size() == 1, "local display page should parse");
    const auto& widget = config.layout.pages.front().widgets.front();
    require(widget.stateBinding.labelWidget == "BatteryStateText", "state label widget should parse");
    require(widget.stateBinding.states.size() == 2, "state rules should parse");
    require(widget.stateBinding.states.front().code == "FAULT", "state rules should sort by descending priority");
    require(widget.stateBinding.states.front().conditions.front().comparison == "eq", "state comparison should parse");
    require(widget.pointIndexes.size() == 2, "state condition indexes should join display read indexes");
}

void verifyLocalDisplayScadaConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "localDisplay": {
        "enabled": true,
        "scada": {
          "enabled": true,
          "projectDirectory": "/opt/modbus-gateway/scada/releases/site-a-1.0.0",
          "packageFile": "/opt/modbus-gateway/scada/site-a-1.0.0.kyscada",
          "nodeId": "edge-a",
          "autoReload": false
        }
      }
    })JSON";
    output.close();

    const auto config = edge_gateway::ConfigLoader::loadAppConfigFromFile(path).localDisplay.scada;
    std::remove(path.c_str());
    require(config.enabled, "local display SCADA should parse enabled");
    require(
        config.projectDirectory == "/opt/modbus-gateway/scada/releases/site-a-1.0.0",
        "local display SCADA project directory should parse"
    );
    require(config.nodeId == "edge-a", "local display SCADA nodeId should parse");
    require(!config.autoReload, "local display SCADA autoReload should parse");
}

void verifyScadaUpperComputerSafetyConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "systemMonitor": {
        "scadaUpperComputerSafety": {
          "enabled": true,
          "projectDirectory": "/srv/scada/current",
          "leaseFile": "/run/scada-upper.lease",
          "reloadIntervalMs": 2500
        },
        "directMaintenance": { "enabled": true }
      }
    })JSON";
    output.close();

    const auto config = edge_gateway::ConfigLoader::loadAppConfigFromFile(path).systemMonitor;
    std::remove(path.c_str());
    require(config.scadaUpperComputerSafety.enabled, "SCADA upper-computer safety should parse enabled");
    require(config.scadaUpperComputerSafety.projectDirectory == "/srv/scada/current", "SCADA project directory should parse");
    require(config.scadaUpperComputerSafety.leaseFile == "/run/scada-upper.lease", "SCADA lease file should parse");
    require(config.scadaUpperComputerSafety.reloadIntervalMs == 2500, "SCADA reload interval should parse");
    require(config.directMaintenance.scadaUpperComputerSafetyEnabled, "direct maintenance should share SCADA safety enablement");
    require(config.directMaintenance.scadaUpperComputerLeaseFile == "/run/scada-upper.lease", "direct maintenance should share SCADA lease file");
    require(config.directMaintenance.scadaUpperComputerProjectDirectory == "/srv/scada/current", "direct maintenance should share SCADA project directory");
}

void verifySystemMonitorCpuAlertConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "systemMonitor": {
        "cpuAlertThreshold": 93,
        "cpuAlertRecoveryThreshold": 84,
        "cpuAlertConsecutiveSamples": 4,
        "cpuRecoveryConsecutiveSamples": 2
      }
    })JSON";
    output.close();

    const auto config = edge_gateway::ConfigLoader::loadAppConfigFromFile(path).systemMonitor;
    std::remove(path.c_str());
    require(config.cpuAlertThreshold == 93.0, "CPU alert threshold should parse");
    require(config.cpuAlertRecoveryThreshold == 84.0, "CPU recovery threshold should parse");
    require(config.cpuAlertConsecutiveSamples == 4, "CPU trigger samples should parse");
    require(config.cpuRecoveryConsecutiveSamples == 2, "CPU recovery samples should parse");

    const auto invalidPath = tempPath();
    std::ofstream invalidOutput(invalidPath.c_str(), std::ios::binary | std::ios::trunc);
    invalidOutput << R"JSON({
      "systemMonitor": {
        "cpuAlertThreshold": 150,
        "cpuAlertRecoveryThreshold": 120,
        "cpuAlertConsecutiveSamples": 0,
        "cpuRecoveryConsecutiveSamples": 1000
      }
    })JSON";
    invalidOutput.close();

    const auto bounded = edge_gateway::ConfigLoader::loadAppConfigFromFile(invalidPath).systemMonitor;
    std::remove(invalidPath.c_str());
    require(bounded.cpuAlertThreshold == 100.0, "CPU alert threshold should be bounded");
    require(bounded.cpuAlertRecoveryThreshold == 90.0, "CPU recovery threshold should stay below trigger");
    require(bounded.cpuAlertConsecutiveSamples == 1, "CPU trigger samples should be bounded");
    require(bounded.cpuRecoveryConsecutiveSamples == 60, "CPU recovery samples should be bounded");
}

void verifyIec103RecordingTransferConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "mqtt": {
        "recordingRequestTopic": "recording/request/custom",
        "recordingReplyTopic": "recording/reply/custom",
        "recordingStatusTopic": "recording/status/custom",
        "recordingAckTopic": "recording/ack/custom"
      },
      "systemMonitor": {
        "recordingTransfer": {
          "enabled": true,
          "queueFile": "/tmp/recording-queue.tsv",
          "workDirectory": "/tmp/recording-work",
          "curlExecutable": "/usr/bin/curl",
          "allowInsecureHttp": true,
          "requestTimeoutSec": 7,
          "retryBaseSec": 4,
          "retryMaxSec": 2
        }
      }
    })JSON";
    output.close();

    const auto app = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    std::remove(path.c_str());
    require(app.mqtt.recordingRequestTopic == "recording/request/custom", "recording request topic should parse");
    require(app.mqtt.recordingReplyTopic == "recording/reply/custom", "recording reply topic should parse");
    require(app.mqtt.recordingStatusTopic == "recording/status/custom", "recording status topic should parse");
    require(app.mqtt.recordingAckTopic == "recording/ack/custom", "recording ACK topic should parse");
    require(app.systemMonitor.recordingTransfer.enabled, "recording transfer should parse enabled");
    require(app.systemMonitor.recordingTransfer.queueFile == "/tmp/recording-queue.tsv", "recording queue path should parse");
    require(app.systemMonitor.recordingTransfer.workDirectory == "/tmp/recording-work", "recording work path should parse");
    require(app.systemMonitor.recordingTransfer.curlExecutable == "/usr/bin/curl", "recording curl executable should parse");
    require(app.systemMonitor.recordingTransfer.allowInsecureHttp, "recording HTTP test override should parse");
    require(app.systemMonitor.recordingTransfer.requestTimeoutSec == 10, "recording timeout should be bounded");
    require(app.systemMonitor.recordingTransfer.retryBaseSec == 4, "recording retry base should parse");
    require(app.systemMonitor.recordingTransfer.retryMaxSec == 4, "recording retry max should not be below base");
}

void verifyDeliveryRuntimeConfig() {
    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << R"JSON({
      "mqttDriver": {
        "deliveryMode": "on_change",
        "deliveryMaxLatencyMs": 40
      },
      "eventEngine": {
        "deliveryMode": "periodic",
        "deliveryMaxLatencyMs": 50
      }
    })JSON";
    output.close();

    const auto app = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    std::remove(path.c_str());
    require(app.mqttDriver.deliveryMode == "onChange", "MQTT delivery mode should normalize");
    require(app.mqttDriver.deliveryMaxLatencyMs == 40, "MQTT maximum delivery latency should parse");
    require(app.eventEngine.deliveryMode == "periodic", "EventEngine delivery mode should parse");
    require(app.eventEngine.deliveryMaxLatencyMs == 50, "EventEngine maximum delivery latency should parse");
}

void verifyCameraAuthenticationFailsClosed() {
    const auto path = tempPath();
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << R"JSON({
          "cameraService": {
            "enabled": true,
            "media": {
              "auth": {
                "enabled": true,
                "mode": "basic",
                "username": "injected-user",
                "password": ""
              }
            }
          }
        })JSON";
    }
    bool rejectedMissingPassword = false;
    try {
        (void)edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    } catch (const std::runtime_error& ex) {
        rejectedMissingPassword = std::string(ex.what()).find("requires injected") != std::string::npos;
    }
    std::remove(path.c_str());
    require(rejectedMissingPassword, "enabled camera basic auth should reject a missing password");

    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << R"JSON({
          "cameraService": {
            "enabled": true,
            "cameras": [{
              "cameraCode": "CAM_TEST",
              "sourceAuth": {
                "enabled": true,
                "mode": "token_query",
                "token": ""
              }
            }]
          }
        })JSON";
    }
    bool rejectedMissingToken = false;
    try {
        (void)edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    } catch (const std::runtime_error& ex) {
        rejectedMissingToken = std::string(ex.what()).find("requires an injected token") != std::string::npos;
    }
    std::remove(path.c_str());
    require(rejectedMissingToken, "enabled camera token auth should reject a missing token");

    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << R"JSON({
          "cameraService": {
            "enabled": true,
            "media": {"auth": {"enabled": false}}
          }
        })JSON";
    }
    const auto publicConfig = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
    std::remove(path.c_str());
    require(!publicConfig.cameraService.media.auth.enabled,
        "explicitly public camera media should not require credentials");
}

void verifyMqttForwardDefaultsAndValidation() {
    const auto load = [](const std::string& json) {
        const auto path = tempPath();
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output << json;
        output.close();
        const auto app = edge_gateway::ConfigLoader::loadAppConfigFromFile(path);
        std::remove(path.c_str());
        return app;
    };

    const auto missing = load(R"JSON({
      "mqtt": {
        "enabled": true,
        "broker": "tcp://127.0.0.1:1883",
        "clientId": "GW_OLD",
        "fullTelemetryTopic": "edge/telemetry/full",
        "commandRequestTopic": "edge/command/request"
      }
    })JSON");
    require(!missing.mqttForward.enabled, "missing mqttForward must default to disabled");
    require(missing.mqttForward.broker.empty(), "missing mqttForward must not invent a broker");
    require(missing.mqttForward.fullTelemetryTopic.empty(), "missing mqttForward must not invent a topic");
    require(missing.mqttForward.pointIndexes.empty(), "missing mqttForward must not invent a point set");
    require(
        missing.mqttForward.payloadFormat == "compactArray",
        "missing mqttForward payloadFormat should use compactArray"
    );
    require(missing.mqttForward.qos == 1, "missing mqttForward qos should keep the safe default");
    require(missing.mqttForward.intervalMs == 60000, "missing mqttForward interval should keep the safe default");
    require(!missing.mqttForward.tls.enabled, "missing mqttForward TLS should stay disabled");
    require(!missing.mqttForward.tls.insecureSkipVerify, "insecureSkipVerify must default to false");
    require(missing.mqtt.enabled, "existing mqtt.enabled must stay unchanged");
    require(missing.mqtt.broker == "tcp://127.0.0.1:1883", "existing mqtt.broker must stay unchanged");
    require(missing.mqtt.clientId == "GW_OLD", "existing mqtt.clientId must stay unchanged");
    require(
        missing.mqtt.fullTelemetryTopic == "edge/telemetry/full",
        "existing mqtt.fullTelemetryTopic must stay unchanged"
    );
    require(
        missing.mqtt.commandRequestTopic == "edge/command/request",
        "existing mqtt control topics must stay unchanged"
    );

    const auto sample = edge_gateway::ConfigLoader::loadAppConfigFromFile(
        "config/examples/mqtt-forward-disabled.json"
    );
    require(!sample.mqttForward.enabled, "sample mqttForward must stay disabled");
    require(sample.mqttForward.broker.empty(), "disabled sample must not point at a live broker");
    require(sample.mqttForward.pointIndexes.empty(), "disabled sample should keep an empty third-party point set");
    require(sample.mqttForward.payloadFormat == "compactArray", "disabled sample should declare compactArray");
    require(sample.mqtt.enabled, "sample must preserve the existing mqtt block");

    const auto enabled = load(R"JSON({
      "mqttForward": {
        "enabled": true,
        "protocolVersion": "mqtt3",
        "broker": "tcp://10.0.0.8:1883",
        "fullTelemetryTopic": "third/full",
        "pointIndexes": [101, 102],
        "payloadFormat": "object",
        "username": "fwd",
        "password": "secret",
        "qos": 1,
        "intervalMs": 15000,
        "tls": {
          "enabled": false,
          "caFile": "",
          "certFile": "",
          "keyFile": "",
          "insecureSkipVerify": false
        }
      }
    })JSON");
    require(enabled.mqttForward.enabled, "enabled mqttForward should parse");
    require(enabled.mqttForward.broker == "tcp://10.0.0.8:1883", "enabled mqttForward broker should parse");
    require(enabled.mqttForward.fullTelemetryTopic == "third/full", "enabled mqttForward topic should parse");
    require(
        enabled.mqttForward.pointIndexes == std::vector<std::uint32_t>({101, 102}),
        "enabled mqttForward pointIndexes should parse in configured order"
    );
    require(enabled.mqttForward.payloadFormat == "object", "enabled mqttForward payloadFormat should parse");
    require(enabled.mqttForward.intervalMs == 15000, "enabled mqttForward interval should parse");
    require(!enabled.mqtt.enabled, "mqttForward must not implicitly enable the main mqtt block");

    const auto expectRejected = [&](const std::string& json, const char* needle, const char* message) {
        bool rejected = false;
        try {
            (void)load(json);
        } catch (const std::exception& ex) {
            rejected = std::string(ex.what()).find(needle) != std::string::npos;
        }
        require(rejected, message);
    };

    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"fullTelemetryTopic":"third/full"}})JSON",
        "broker",
        "enabled mqttForward must require broker"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"tcp://10.0.0.8:1883"}})JSON",
        "fullTelemetryTopic",
        "enabled mqttForward must require fullTelemetryTopic"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"tcp://10.0.0.8:1883","fullTelemetryTopic":"third/full","qos":3}})JSON",
        "qos",
        "mqttForward qos must be 0/1/2"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"tcp://10.0.0.8:1883","fullTelemetryTopic":"third/full","intervalMs":0}})JSON",
        "intervalMs",
        "mqttForward intervalMs must be greater than 0"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"tcp://10.0.0.8:1883","fullTelemetryTopic":"third/full","pointIndexes":[]}})JSON",
        "pointIndexes",
        "enabled mqttForward must require a non-empty pointIndexes array"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":101}})JSON",
        "JSON uint32 array",
        "mqttForward pointIndexes must be an array"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":["101"]}})JSON",
        "uint32 integers",
        "mqttForward pointIndexes must reject strings"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":[-1]}})JSON",
        "uint32 integers",
        "mqttForward pointIndexes must reject negative values"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":[1.5]}})JSON",
        "uint32 integers",
        "mqttForward pointIndexes must reject fractional values"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":[4294967296]}})JSON",
        "uint32 integers",
        "mqttForward pointIndexes must reject values above uint32"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"pointIndexes":[101,101]}})JSON",
        "duplicate index 101",
        "mqttForward pointIndexes must reject duplicates"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"payloadFormat":"array"}})JSON",
        "compactArray or object",
        "mqttForward payloadFormat must reject unsupported aliases"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"payloadFormat":1}})JSON",
        "compactArray or object",
        "mqttForward payloadFormat must be a string"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"tcp://10.0.0.8:1883","fullTelemetryTopic":"third/full","tls":{"enabled":true}}})JSON",
        "caFile",
        "enabled TLS must require a CA file"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":true,"broker":"mqtts://10.0.0.8:8883","fullTelemetryTopic":"third/full"}})JSON",
        "caFile",
        "TLS broker schemes must require a CA file"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"tls":{"certFile":"client.crt"}}})JSON",
        "together",
        "TLS client cert and key must be provided as a pair"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"subscribe":true}})JSON",
        "subscribe",
        "mqttForward must reject a subscribe escape hatch"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"allowControlTopics":true}})JSON",
        "allowControlTopics",
        "mqttForward must reject an allowControlTopics escape hatch"
    );
    expectRejected(
        R"JSON({"mqttForward":{"enabled":false,"commandRequestTopic":"edge/command/request"}})JSON",
        "commandRequestTopic",
        "mqttForward must reject control request topics"
    );
}

void verifyPublicCameraExampleLoadsWithoutCredentials() {
    const auto app = edge_gateway::ConfigLoader::loadAppConfigFromFile(
        "config/examples/camera-service-public-example.json"
    );
    require(!app.cameraService.media.auth.enabled,
        "public camera example media authentication should be disabled");
    require(!app.cameraService.cameras.empty(), "public camera example should include a camera");
    for (const auto& camera : app.cameraService.cameras) {
        require(!camera.sourceAuth.enabled,
            "public camera example source authentication should be disabled");
    }
}
}  // namespace

int main() {
    using namespace edge_gateway;

    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output <<
        "{"
        "\"mqtt\":{"
        "\"connectTimeoutMs\":999999,"
        "\"legacyTelemetryEnabled\":true,"
        "\"legacyTelemetryTopic\":\"ky/peidian/COMM_TEST\","
        "\"legacyTopicMachineCode\":\"COMM_TEST\","
        "\"legacyTelemetryIntervalMs\":10000,"
        "\"legacyTelemetryMappedOnly\":true,"
        "\"legacyTelemetryPointMappings\":["
        "{\"index\":1002,\"meterCode\":\"LEGACY_METER\",\"pointCode\":\"LEGACY_POINT\"}"
        "],"
        "\"maxPayloadBytes\":9999999,"
        "\"offlineBuffer\":{"
        "\"realtimeFileSizeBytes\":9999999999,"
        "\"maxRealtimeMessageBytes\":9999999,"
        "\"maxMemoryMessages\":50000,"
        "\"flushBatchSize\":50000,"
        "\"flushIntervalMs\":600000,"
        "\"replayBatchSize\":50000,"
        "\"maxDiskBytes\":9999999999,"
        "\"eventOutbox\":{"
        "\"retentionMonths\":240,"
        "\"cleanupIntervalHours\":999,"
        "\"replayBatchSize\":50000,"
        "\"maxDiskBytes\":9999999999"
        "}"
        "}"
        "}"
        "}";
    output.close();

    const auto config = ConfigLoader::loadAppConfigFromFile(path).mqtt;
    std::remove(path.c_str());

    require(config.connectTimeoutMs == 60000, "MQTT connect timeout should be bounded");
    require(config.maxPayloadBytes == 1024U * 1024U, "maxPayloadBytes should be bounded");
    require(config.offlineRealtimeFileSizeBytes == 1024ULL * 1024ULL * 1024ULL, "realtime file size should be bounded");
    require(config.offlineMaxRealtimeMessageBytes == 4U * 1024U * 1024U, "max realtime message should be bounded");
    require(config.offlineBufferMaxMemoryMessages == 1000U, "memory message count should be bounded");
    require(config.offlineBufferFlushBatchSize == 1000U, "flush batch should be bounded");
    require(config.offlineBufferFlushIntervalMs == 60000, "flush interval should be bounded");
    require(config.offlineBufferReplayBatchSize == 1000U, "replay batch should be bounded");
    require(config.offlineBufferMaxDiskBytes == 256U * 1024U * 1024U, "offline disk should be bounded");
    require(config.eventOutboxRetentionMonths == 24, "outbox retention should be bounded");
    require(config.eventOutboxCleanupIntervalHours == 168, "outbox cleanup interval should be bounded");
    require(config.eventOutboxReplayBatchSize == 1000U, "outbox replay batch should be bounded");
    require(config.eventOutboxMaxDiskBytes == 256U * 1024U * 1024U, "outbox disk should be bounded");
    require(config.legacyTelemetryEnabled, "legacy telemetry should parse");
    require(config.legacyTelemetryTopic == "ky/peidian/COMM_TEST", "legacy telemetry topic should parse");
    require(config.legacyTopicMachineCode == "COMM_TEST", "legacy topic machine code should parse");
    require(config.legacyTelemetryIntervalMs == 10000, "legacy telemetry interval should parse");
    require(config.legacyTelemetryMappedOnly, "legacy mapped-only mode should parse");
    require(config.legacyTelemetryPointMappings.size() == 1, "legacy telemetry mapping should parse");
    require(config.legacyTelemetryPointMappings.front().index == 1002,
        "legacy telemetry mapping index should parse");
    require(config.legacyTelemetryPointMappings.front().meterCode == "LEGACY_METER",
        "legacy telemetry mapping meter should parse");
    require(config.legacyTelemetryPointMappings.front().pointCode == "LEGACY_POINT",
        "legacy telemetry mapping point should parse");

    verifyDeviceCollectBackgroundTaskConfig();
    verifyTimingPolicyConfig();
    verifyDlt645WriteConfig();
    verifyDlt645StandardPointValueMap();
    verifyNorthboundConfig();
    verifyPointNormalizeConfig();
    verifyLocalDisplayStateBindingConfig();
    verifyLocalDisplayScadaConfig();
    verifyScadaUpperComputerSafetyConfig();
    verifySystemMonitorCpuAlertConfig();
    verifyIec103RecordingTransferConfig();
    verifyDeliveryRuntimeConfig();
    verifyMqttForwardDefaultsAndValidation();
    verifyCameraAuthenticationFailsClosed();
    verifyPublicCameraExampleLoadsWithoutCredentials();

    std::cout << "config_loader_test passed" << std::endl;
    return 0;
}
