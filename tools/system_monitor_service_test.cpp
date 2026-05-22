#include "edge_gateway/system_monitor_service.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
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
    void publishFullSnapshot(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
    void publishAlarm(const std::string&, std::uint32_t, const edge_gateway::StoredPointValue&, const std::string&, bool) override {}
    void publishOnDemand(const std::string&, const std::vector<edge_gateway::StoredPointValue>&) override {}
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

void removeFileIfExists(const std::string& path) {
    unlink(path.c_str());
}

void ensureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("failed to create test dir: " + path);
    }
}

}  // namespace

int main() {
    using namespace edge_gateway;

    const std::string root = "/tmp/system-monitor-service-test";
    ensureDir(root);
    const std::string small = root + "/small.json";
    const std::string large = root + "/large.json";
    removeFileIfExists(small);
    removeFileIfExists(large);
    removeFileIfExists(root + "/missing.json");
    writeFile(small, "{\"ok\":true}\n");
    writeFile(large, std::string(512 * 1024 + 1, 'x'));

    MqttConfig mqtt;
    mqtt.systemMonitorReplyTopic = "reply";
    mqtt.configPullReplyTopic = "config/reply";
    auto publisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService service(
        SystemMonitorConfig{},
        mqtt,
        publisher,
        "GW_TEST",
        std::vector<std::string>{small, large, root + "/missing.json"}
    );

    MqttIncomingMessage request;
    request.type = MqttIncomingType::ConfigPullRequest;
    request.payload = "{\"requestId\":\"REQ_1\",\"machineCode\":\"GW_TEST\"}";
    publisher->incoming.push_back(request);
    service.runOnce(1770000000000LL);

    std::string reply;
    for (std::size_t i = 0; i < publisher->topics.size(); ++i) {
        if (publisher->topics[i] == mqtt.configPullReplyTopic) {
            reply = publisher->payloads[i];
            break;
        }
    }
    require(!reply.empty(), "expected config pull reply");
    require(reply.find("\"fileCount\":1") != std::string::npos, "expected one emitted file");
    require(reply.find("\"skippedFiles\":2") != std::string::npos, "expected two skipped files");
    require(reply.find("small.json") != std::string::npos, "small config should be included");
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

    removeFileIfExists(small);
    removeFileIfExists(large);
    for (const auto& path : chunkFiles) {
        removeFileIfExists(path);
    }
    std::cout << "system_monitor_service_test passed" << std::endl;
    return 0;
}
