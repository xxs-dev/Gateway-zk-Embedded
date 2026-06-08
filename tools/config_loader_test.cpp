#include "edge_gateway/config_loader.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
        "\"transport\":{\"frameIntervalMs\":500,\"readRetryCount\":2}},"
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

}  // namespace

int main() {
    using namespace edge_gateway;

    const auto path = tempPath();
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output <<
        "{"
        "\"mqtt\":{"
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

    verifyDeviceCollectBackgroundTaskConfig();
    verifyNorthboundConfig();

    std::cout << "config_loader_test passed" << std::endl;
    return 0;
}
