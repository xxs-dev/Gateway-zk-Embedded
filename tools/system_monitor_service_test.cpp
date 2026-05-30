#include "edge_gateway/system_monitor_service.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>
#include <unistd.h>

namespace {

class CapturingPublisher : public edge_gateway::IMqttDriverPublisher {
public:
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&, const std::string&) override {}
    void publishChangeEvent(const std::string&, const edge_gateway::StoredPointValue&) override {}
    void publishCommandReply(const std::string&, const edge_gateway::MqttCommandReply&) override {}
    void publishOtaReply(const std::string&, const edge_gateway::OtaReply&) override {}
    void publishOtaStatus(const std::string&, const edge_gateway::OtaStatus&) override {}
    void publishJsonMessage(const std::string& topic, const std::string& payload) override {
        topics.push_back(topic);
        payloads.push_back(payload);
    }
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override {
        auto result = incoming;
        incoming.clear();
        return result;
    }

    std::vector<edge_gateway::MqttIncomingMessage> incoming;
    std::vector<std::string> topics;
    std::vector<std::string> payloads;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write test file: " + path);
    }
    output << content;
}

std::string readFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read test file: " + path);
    }
    std::string content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    return content;
}

void removeFileIfExists(const std::string& path) {
    unlink(path.c_str());
}

void ensureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("failed to create test dir: " + path);
    }
}

int jsonIntField(const std::string& payload, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const auto pos = payload.find(marker);
    if (pos == std::string::npos) {
        return 0;
    }
    auto cursor = pos + marker.size();
    while (cursor < payload.size() && payload[cursor] == ' ') {
        ++cursor;
    }
    return std::atoi(payload.c_str() + cursor);
}

std::string jsonStringField(const std::string& payload, const std::string& key) {
    const std::string marker = "\"" + key + "\":\"";
    const auto pos = payload.find(marker);
    if (pos == std::string::npos) {
        return "";
    }
    auto cursor = pos + marker.size();
    std::string value;
    while (cursor < payload.size()) {
        const char ch = payload[cursor++];
        if (ch == '"') {
            return value;
        }
        if (ch == '\\' && cursor < payload.size()) {
            value.push_back(payload[cursor++]);
        } else {
            value.push_back(ch);
        }
    }
    return "";
}

std::string fromHex(const std::string& hex) {
    std::string result;
    result.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = std::strtol(hex.substr(i, 1).c_str(), nullptr, 16);
        const int low = std::strtol(hex.substr(i + 1, 1).c_str(), nullptr, 16);
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::string configPullReplyPayload(const CapturingPublisher& publisher, const std::string& topic) {
    std::string direct;
    std::map<int, std::string> chunks;
    int chunkCount = 0;
    for (std::size_t i = 0; i < publisher.topics.size(); ++i) {
        if (publisher.topics[i] != topic) {
            continue;
        }
        const auto& payload = publisher.payloads[i];
        if (payload.find("\"chunked\":true") == std::string::npos) {
            direct = payload;
            continue;
        }
        const int index = jsonIntField(payload, "chunkIndex");
        chunkCount = jsonIntField(payload, "chunkCount");
        chunks[index] = fromHex(jsonStringField(payload, "payloadHex"));
    }
    if (chunkCount <= 0) {
        return direct;
    }
    std::string assembled;
    for (int i = 1; i <= chunkCount; ++i) {
        const auto it = chunks.find(i);
        require(it != chunks.end(), "missing config pull reply chunk");
        assembled += it->second;
    }
    return assembled;
}

std::size_t countTopic(const CapturingPublisher& publisher, const std::string& topic) {
    std::size_t count = 0;
    for (const auto& item : publisher.topics) {
        if (item == topic) {
            ++count;
        }
    }
    return count;
}

std::string lastPayloadForTopic(const CapturingPublisher& publisher, const std::string& topic) {
    for (std::size_t i = publisher.topics.size(); i > 0; --i) {
        if (publisher.topics[i - 1] == topic) {
            return publisher.payloads[i - 1];
        }
    }
    return "";
}

}  // namespace

int main() {
    using namespace edge_gateway;

    const std::string root = "/tmp/system-monitor-service-test";
    ensureDir(root);
    const std::string small = root + "/small.json";
    const std::string medium = root + "/medium.json";
    const std::string large = root + "/large.json";
    removeFileIfExists(small);
    removeFileIfExists(medium);
    removeFileIfExists(large);
    removeFileIfExists(root + "/missing.json");
    writeFile(small, "{\"ok\":true}\n");
    writeFile(medium, std::string(512 * 1024 + 1, 'm'));
    writeFile(large, std::string(5 * 1024 * 1024 + 1, 'x'));

    MqttConfig mqtt;
    mqtt.systemMonitorReplyTopic = "reply";
    mqtt.configPullReplyTopic = "config/reply";
    auto publisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService service(
        SystemMonitorConfig{},
        mqtt,
        publisher,
        "GW_TEST",
        std::vector<std::string>{small, medium, large, root + "/missing.json"}
    );

    MqttIncomingMessage request;
    request.type = MqttIncomingType::ConfigPullRequest;
    request.payload = "{\"requestId\":\"REQ_1\",\"machineCode\":\"GW_TEST\"}";
    publisher->incoming.push_back(request);
    service.runOnce(1770000000000LL);

    const std::string reply = configPullReplyPayload(*publisher, mqtt.configPullReplyTopic);
    require(!reply.empty(), "expected config pull reply");
    require(reply.find("\"fileCount\":2") != std::string::npos, "expected two emitted files");
    require(reply.find("\"skippedFiles\":2") != std::string::npos, "expected two skipped files");
    require(reply.find("small.json") != std::string::npos, "small config should be included");
    require(reply.find("medium.json") != std::string::npos, "medium config should be included");
    require(reply.find("large.json") == std::string::npos, "large config should be skipped");
    require(reply.find("missing.json") == std::string::npos, "missing config should be skipped");

    std::vector<std::string> chunkFiles;
    for (int i = 0; i < 4; ++i) {
        const std::string path = root + "/chunk_" + std::to_string(i) + ".json";
        removeFileIfExists(path);
        writeFile(path, std::string(8 * 1024, static_cast<char>('a' + i)));
        chunkFiles.push_back(path);
    }
    auto chunkPublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService chunkService(SystemMonitorConfig{}, mqtt, chunkPublisher, "GW_TEST", chunkFiles);
    request.payload = "{\"requestId\":\"REQ_2\",\"machineCode\":\"GW_TEST\"}";
    chunkPublisher->incoming.push_back(request);
    chunkService.runOnce(1770000001000LL);
    bool sawChunkedReply = false;
    for (const auto& payload : chunkPublisher->payloads) {
        if (payload.find("\"chunked\":true") != std::string::npos) {
            sawChunkedReply = true;
            break;
        }
    }
    require(sawChunkedReply, "expected chunked reply payload");

    const std::string storeName = "system_monitor_service_test_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    MemoryPointStore store(storeName);
    PointStoreRouter router;
    router.addStore(storeName, store);
    PointStoreRoute route;
    route.index = 1001;
    route.machineCode = "GW_TEST";
    route.meterCode = "METER_1";
    route.pointCode = "P_1";
    route.sharedMemoryName = storeName;
    router.addRoute(route);
    PointStoreRoute route2;
    route2.index = 1002;
    route2.machineCode = "GW_TEST";
    route2.meterCode = "METER_2";
    route2.pointCode = "P_2";
    route2.sharedMemoryName = storeName;
    router.addRoute(route2);
    PointValue pointValue;
    pointValue.index = 1001;
    pointValue.value = 12.3;
    pointValue.ts = 1770000002000LL;
    pointValue.expireAt = 1770000602000LL;
    require(router.putLatestByIndex(pointValue).accepted, "failed to seed monitor point value");
    PointValue pointValue2;
    pointValue2.index = 1002;
    pointValue2.value = 45.6;
    pointValue2.ts = 1770000002000LL;
    pointValue2.expireAt = 1770000602000LL;
    require(router.putLatestByIndex(pointValue2).accepted, "failed to seed second monitor point value");

    MqttConfig leaseMqtt;
    leaseMqtt.statusTopic = "status";
    leaseMqtt.systemMonitorReplyTopic = "monitor/reply";
    leaseMqtt.systemMonitorTelemetryTopic = "monitor/telemetry";
    leaseMqtt.systemMonitorPointTopic = "monitor/points";
    SystemMonitorConfig leaseConfig;
    leaseConfig.defaultIntervalMs = 5000;
    leaseConfig.minIntervalMs = 500;
    leaseConfig.realtimeMeterLeaseFile = root + "/realtime-meter-leases.json";
    removeFileIfExists(leaseConfig.realtimeMeterLeaseFile);
    auto leasePublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService leaseService(leaseConfig, leaseMqtt, leasePublisher, "GW_TEST", std::vector<std::string>{}, &router);
    MqttIncomingMessage monitorRequest;
    monitorRequest.type = MqttIncomingType::SystemMonitorRequest;
    monitorRequest.payload = "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_TEST\",\"intervalMs\":500,\"ttlSec\":30}";
    leasePublisher->incoming.push_back(monitorRequest);
    leaseService.runOnce(1770000003000LL);
    require(countTopic(*leasePublisher, leaseMqtt.systemMonitorPointTopic) == 1, "monitor subscribe should publish one point snapshot immediately");
    const auto fullPayload = lastPayloadForTopic(*leasePublisher, leaseMqtt.systemMonitorPointTopic);
    require(fullPayload.find("\"meterCode\":\"METER_1\"") != std::string::npos, "unfiltered monitor snapshot should include meter 1");
    require(fullPayload.find("\"meterCode\":\"METER_2\"") != std::string::npos, "unfiltered monitor snapshot should include meter 2");
    require(countTopic(*leasePublisher, leaseMqtt.systemMonitorTelemetryTopic) == 1, "monitor subscribe should still publish telemetry once");
    leaseService.runOnce(1770000003300LL);
    require(countTopic(*leasePublisher, leaseMqtt.systemMonitorPointTopic) == 1, "monitor point snapshot should respect 500ms interval after immediate publish");
    leaseService.runOnce(1770000003500LL);
    require(countTopic(*leasePublisher, leaseMqtt.systemMonitorPointTopic) == 2, "monitor point snapshot should publish after 500ms interval");

    auto filteredPublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService filteredService(leaseConfig, leaseMqtt, filteredPublisher, "GW_TEST", std::vector<std::string>{}, &router);
    MqttIncomingMessage filteredRequest;
    filteredRequest.type = MqttIncomingType::SystemMonitorRequest;
    filteredRequest.payload = "{\"machineCode\":\"GW_TEST\",\"sessionId\":\"REALTIME_FILTER\",\"meterCode\":\"METER_1\",\"intervalMs\":500,\"ttlSec\":30}";
    filteredPublisher->incoming.push_back(filteredRequest);
    filteredService.runOnce(1770000010000LL);
    const auto filteredPayload = lastPayloadForTopic(*filteredPublisher, leaseMqtt.systemMonitorPointTopic);
    require(filteredPayload.find("\"meterCode\":\"METER_1\"") != std::string::npos, "filtered monitor snapshot should include requested meter");
    require(filteredPayload.find("\"meterCode\":\"METER_2\"") == std::string::npos, "filtered monitor snapshot should exclude other meters");
    const auto leaseFile = readFile(leaseConfig.realtimeMeterLeaseFile);
    require(leaseFile.find("\"meterCodes\":[\"METER_1\"]") != std::string::npos, "filtered monitor lease should include requested meter only");
    require(leaseFile.find("\"METER_2\"") == std::string::npos, "filtered monitor lease should not include unrelated meters");
    require(leaseFile.find("\"expireAtMs\":1770000040000") != std::string::npos, "filtered monitor lease should include expiry");

    SystemMonitorConfig diagConfig;
    diagConfig.maxDiagOutputBytes = 256;
    diagConfig.allowedCommands = {"top_once"};
    auto diagPublisher = std::make_shared<CapturingPublisher>();
    MqttConfig diagMqtt;
    diagMqtt.diagReplyTopic = "diag/reply";
    diagMqtt.statusTopic = "status";
    SystemMonitorService diagService(diagConfig, diagMqtt, diagPublisher, "GW_TEST");
    MqttIncomingMessage diagRequest;
    diagRequest.type = MqttIncomingType::DiagRequest;
    diagRequest.payload = "{\"cmdId\":\"DIAG_1\",\"machineCode\":\"GW_TEST\",\"command\":\"top_once\"}";
    diagPublisher->incoming.push_back(diagRequest);
    diagService.runOnce(1770000002000LL);
    std::string diagReply;
    for (std::size_t i = 0; i < diagPublisher->topics.size(); ++i) {
        if (diagPublisher->topics[i] == diagMqtt.diagReplyTopic) {
            diagReply = diagPublisher->payloads[i];
            break;
        }
    }
    require(!diagReply.empty(), "expected diag reply");
    require(diagReply.find("[truncated]") != std::string::npos, "expected bounded diag output");
    require(diagReply.size() < 2048, "diag reply should stay bounded");

    removeFileIfExists(small);
    removeFileIfExists(medium);
    removeFileIfExists(large);
    for (const auto& path : chunkFiles) {
        removeFileIfExists(path);
    }
    removeFileIfExists(leaseConfig.realtimeMeterLeaseFile);
    std::cout << "system_monitor_service_test passed" << std::endl;
    return 0;
}
