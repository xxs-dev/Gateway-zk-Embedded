#include "edge_gateway/system_monitor_service.hpp"
#include "edge_gateway/system_monitor_points.hpp"

#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
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

struct TestSharedStoreHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    pthread_mutex_t mutex{};
};

class LockedSharedStore {
public:
    explicit LockedSharedStore(const std::string& storeName) {
        auto normalized = storeName;
        if (!normalized.empty() && normalized.front() != '/') {
            normalized.insert(normalized.begin(), '/');
        }
        fd_ = shm_open(normalized.c_str(), O_RDWR, 0600);
        if (fd_ < 0) {
            throw std::runtime_error("failed to open test shared memory");
        }
        view_ = mmap(nullptr, sizeof(TestSharedStoreHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (view_ == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("failed to map test shared memory");
        }
        auto* header = static_cast<TestSharedStoreHeader*>(view_);
        if (pthread_mutex_lock(&header->mutex) != 0) {
            munmap(view_, sizeof(TestSharedStoreHeader));
            close(fd_);
            view_ = nullptr;
            fd_ = -1;
            throw std::runtime_error("failed to lock test shared memory");
        }
        mutex_ = &header->mutex;
    }

    ~LockedSharedStore() {
        if (mutex_ != nullptr) {
            pthread_mutex_unlock(mutex_);
            mutex_ = nullptr;
        }
        if (view_ != nullptr) {
            munmap(view_, sizeof(TestSharedStoreHeader));
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    LockedSharedStore(const LockedSharedStore&) = delete;
    LockedSharedStore& operator=(const LockedSharedStore&) = delete;

private:
    int fd_ = -1;
    void* view_ = nullptr;
    pthread_mutex_t* mutex_ = nullptr;
};

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

    SustainedThresholdAlert cpuAlert;
    require(
        cpuAlert.update(95.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None,
        "single CPU spike should not trigger"
    );
    require(
        cpuAlert.update(96.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None,
        "two CPU spikes should not trigger"
    );
    require(
        cpuAlert.update(97.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::Triggered,
        "third consecutive CPU spike should trigger"
    );
    require(cpuAlert.active(), "CPU alert should stay active after triggering");
    require(
        cpuAlert.update(85.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None,
        "CPU alert should not recover inside the hysteresis band"
    );
    require(
        cpuAlert.update(75.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        cpuAlert.update(79.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        cpuAlert.update(70.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::Recovered,
        "three healthy CPU samples should recover"
    );
    require(!cpuAlert.active(), "CPU alert should be inactive after recovery");
    require(
        cpuAlert.update(91.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        cpuAlert.update(92.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        cpuAlert.update(93.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::Triggered,
        "CPU alert should trigger again after recovery"
    );

    SustainedThresholdAlert initialCpuState;
    require(
        initialCpuState.update(20.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        initialCpuState.update(21.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::None &&
        initialCpuState.update(22.0, 90.0, 80.0, 3, 3) == SustainedThresholdTransition::Recovered,
        "startup health confirmation should clear a stale platform alert"
    );

    const std::string root = "/tmp/system-monitor-service-test";
    ensureDir(root);
    const std::string small = root + "/small.json";
    const std::string medium = root + "/medium.json";
    const std::string large = root + "/large.json";
    removeFileIfExists(small);
    removeFileIfExists(medium);
    removeFileIfExists(large);
    removeFileIfExists(root + "/missing.json");
    writeFile(
        small,
        std::string("{\"ok\":true,\"localDisplay\":{\"qtRuntime\":{\"home\":\"") +
            root + "/ky-ems-empty\"}}}\n"
    );
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

    const std::string runtimeRoot = root + "/config/runtime";
    const std::string runtimeApps = runtimeRoot + "/apps";
    const std::string runtimeLogic = runtimeRoot + "/logic";
    ensureDir(root + "/config");
    ensureDir(root + "/config/runtime");
    ensureDir(runtimeApps);
    ensureDir(runtimeLogic);
    const std::string runtimeAppConfig = runtimeApps + "/monitor-service.json";
    const std::string runtimeGraph = runtimeLogic + "/shuntong_ems_graph.json";
    writeFile(runtimeAppConfig, "{\"computeEngine\":{\"enabled\":false,\"rules\":[]}}\n");
    writeFile(runtimeGraph, "{\"graphCode\":\"test-graph\",\"nodes\":[{\"id\":\"pcs_writeback\",\"type\":\"pcsWriteback\",\"enabled\":true}]}\n");
    auto logicPublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService logicService(SystemMonitorConfig{}, mqtt, logicPublisher, "GW_TEST", std::vector<std::string>{runtimeAppConfig});
    request.payload = "{\"requestId\":\"REQ_LOGIC\",\"machineCode\":\"GW_TEST\"}";
    logicPublisher->incoming.push_back(request);
    logicService.runOnce(1770000000500LL);
    const std::string logicReply = configPullReplyPayload(*logicPublisher, mqtt.configPullReplyTopic);
    require(logicReply.find(runtimeGraph) != std::string::npos, "runtime logic graph should be included in config pull");
    require(logicReply.find("test-graph") != std::string::npos, "runtime logic graph content should be included");

    const std::string templateRoot = root + "/config/templates";
    const std::string deviceRoot = runtimeRoot + "/devices";
    ensureDir(templateRoot);
    ensureDir(deviceRoot);
    const std::string dlt645Template = templateRoot + "/dlt645_breaker_points.json";
    const std::string dlt645Device = deviceRoot + "/device_dlt645_breakers.json";
    writeFile(
        dlt645Template,
        "{\"protocol\":\"dlt645_2007\",\"points\":[{\"pointCode\":\"breaker_remote_close\",\"name\":\"remote close\",\"access\":\"write\",\"write\":{\"enable\":true}}]}\n"
    );
    writeFile(
        dlt645Device,
        std::string("{\"machineCode\":\"GW_TEST\",\"protocol\":{\"type\":\"dlt645_2007\",\"standardPointsFile\":\"") +
            dlt645Template +
            "\"},\"dlt645\":{\"write\":{\"enabled\":false,\"password\":\"\",\"operatorCode\":\"00000000\"}},\"meters\":[{\"meterCode\":\"BREAKER_1\",\"address\":\"000000000020\",\"points\":[]}]}\n"
    );
    auto dlt645Publisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService dlt645Service(
        SystemMonitorConfig{},
        mqtt,
        dlt645Publisher,
        "GW_TEST",
        std::vector<std::string>{dlt645Device}
    );
    request.payload = "{\"requestId\":\"REQ_DLT645_TEMPLATE\",\"machineCode\":\"GW_TEST\"}";
    dlt645Publisher->incoming.push_back(request);
    dlt645Service.runOnce(1770000000550LL);
    const std::string dlt645Reply = configPullReplyPayload(*dlt645Publisher, mqtt.configPullReplyTopic);
    require(dlt645Reply.find(dlt645Template) != std::string::npos, "referenced DLT645 standard point file should be included in config pull");
    require(dlt645Reply.find("breaker_remote_close") != std::string::npos, "referenced DLT645 point content should be included in config pull");

    const std::string scadaRoot = root + "/scada/current";
    const std::string scadaScreens = scadaRoot + "/screens";
    const std::string scadaAssets = scadaRoot + "/assets";
    ensureDir(root + "/scada");
    ensureDir(scadaRoot);
    ensureDir(scadaScreens);
    ensureDir(scadaAssets);
    writeFile(scadaRoot + "/manifest.json", "{\"projectId\":\"SCADA_TEST\",\"packageVersion\":\"1.0.0\"}\n");
    writeFile(scadaRoot + "/topology.json", "{\"mode\":\"integrated\"}\n");
    writeFile(scadaRoot + "/nodes.json", "[]\n");
    writeFile(scadaRoot + "/tags.json", "[]\n");
    writeFile(scadaRoot + "/runtime-map.json", "[]\n");
    writeFile(scadaRoot + "/checksums.json", "{}\n");
    writeFile(scadaScreens + "/overview.json", "{\"screenId\":\"overview\",\"nodes\":[]}\n");
    writeFile(scadaAssets + "/background.png", std::string("\x01\x02\x03\x04", 4));
    writeFile(scadaRoot + "/not-part-of-package.txt", "ignored\n");
    SystemMonitorConfig scadaConfig;
    scadaConfig.scadaUpperComputerSafety.projectDirectory = scadaRoot;
    auto scadaPublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService scadaService(
        scadaConfig,
        mqtt,
        scadaPublisher,
        "GW_TEST",
        std::vector<std::string>{small}
    );
    request.payload = "{\"requestId\":\"REQ_SCADA\",\"machineCode\":\"GW_TEST\",\"scope\":\"scada\"}";
    scadaPublisher->incoming.push_back(request);
    scadaService.runOnce(1770000000575LL);
    const std::string scadaReply = configPullReplyPayload(*scadaPublisher, mqtt.configPullReplyTopic);
    require(scadaReply.find("\"scope\":\"scada\"") != std::string::npos, "SCADA reply should preserve request scope");
    require(scadaReply.find(scadaRoot + "/manifest.json") != std::string::npos, "SCADA manifest should be included");
    require(scadaReply.find(scadaRoot + "/topology.json") != std::string::npos, "SCADA topology should be included");
    require(scadaReply.find(scadaRoot + "/nodes.json") != std::string::npos, "SCADA nodes should be included");
    require(scadaReply.find(scadaScreens + "/overview.json") != std::string::npos, "SCADA screen should be included");
    require(scadaReply.find(scadaAssets + "/background.png") != std::string::npos, "SCADA asset should be included");
    require(scadaReply.find("\"content\":\"AQIDBA==\"") != std::string::npos, "SCADA binary asset should be base64 encoded");
    require(scadaReply.find("not-part-of-package.txt") == std::string::npos, "unrecognized SCADA root file should be excluded");
    require(scadaReply.find(small) == std::string::npos, "SCADA scope should not include regular config files");

    MqttIncomingMessage logicApplyRequest;
    logicApplyRequest.type = MqttIncomingType::ConfigApplyRequest;
    logicApplyRequest.payload =
        std::string("{\"requestId\":\"REQ_LOGIC_APPLY\",\"machineCode\":\"GW_TEST\",\"dryRun\":false,\"files\":[{\"path\":\"") +
        runtimeGraph +
        "\",\"encoding\":\"utf8\",\"content\":\"{\\\"graphCode\\\":\\\"updated-graph\\\",\\\"nodes\\\":[]}\\n\"}]}";
    logicPublisher->incoming.push_back(logicApplyRequest);
    logicService.runOnce(1770000000600LL);
    require(readFile(runtimeGraph).find("updated-graph") != std::string::npos, "runtime logic graph should be writable through config apply");

    std::vector<std::string> chunkFiles;
    for (int i = 0; i < 4; ++i) {
        const std::string path = root + "/chunk_" + std::to_string(i) + ".json";
        removeFileIfExists(path);
        writeFile(path, std::string(256 * 1024, static_cast<char>('a' + i)));
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

    const std::string kyEmsRoot = root + "/ky-ems";
    const std::string kyEmsPictures = kyEmsRoot + "/Pictures";
    ensureDir(kyEmsRoot);
    ensureDir(kyEmsPictures);
    const std::string kyEmsAppConfig = root + "/ky_ems_app.json";
    const std::string kyEmsImage = kyEmsPictures + "/main.png";
    removeFileIfExists(kyEmsAppConfig);
    removeFileIfExists(kyEmsImage);
    writeFile(
        kyEmsAppConfig,
        std::string("{\"localDisplay\":{\"qtRuntime\":{\"home\":\"") + kyEmsRoot + "\"}}}"
    );
    writeFile(kyEmsImage, std::string("\x01\x02\x03\x04", 4));
    MqttConfig kyEmsMqtt;
    kyEmsMqtt.configPullReplyTopic = "kyems/config/reply";
    kyEmsMqtt.configApplyReplyTopic = "kyems/config/apply/reply";
    auto kyEmsPublisher = std::make_shared<CapturingPublisher>();
    SystemMonitorService kyEmsService(
        SystemMonitorConfig{},
        kyEmsMqtt,
        kyEmsPublisher,
        "GW_TEST",
        std::vector<std::string>{kyEmsAppConfig}
    );
    request.payload = "{\"requestId\":\"REQ_KYEMS\",\"machineCode\":\"GW_TEST\"}";
    kyEmsPublisher->incoming.push_back(request);
    kyEmsService.runOnce(1770000001500LL);
    const std::string kyEmsReply = configPullReplyPayload(*kyEmsPublisher, kyEmsMqtt.configPullReplyTopic);
    require(kyEmsReply.find(kyEmsImage) != std::string::npos, "KY-EMS image should be included");
    require(kyEmsReply.find("\"encoding\":\"base64\"") != std::string::npos, "KY-EMS image should be base64 encoded");
    require(kyEmsReply.find("\"content\":\"AQIDBA==\"") != std::string::npos, "KY-EMS image bytes should be encoded");

    MqttIncomingMessage applyRequest;
    applyRequest.type = MqttIncomingType::ConfigApplyRequest;
    applyRequest.payload =
        std::string("{\"requestId\":\"REQ_KYEMS_APPLY\",\"machineCode\":\"GW_TEST\",\"dryRun\":false,\"files\":[{\"path\":\"") +
        kyEmsImage +
        "\",\"encoding\":\"base64\",\"content\":\"BQYHCA==\"}]}";
    kyEmsPublisher->incoming.push_back(applyRequest);
    kyEmsService.runOnce(1770000001600LL);
    require(readFile(kyEmsImage) == std::string("\x05\x06\x07\x08", 4), "KY-EMS image should be written as binary");
    const std::string applyReply = lastPayloadForTopic(*kyEmsPublisher, kyEmsMqtt.configApplyReplyTopic);
    require(applyReply.find("\"success\":true") != std::string::npos, "KY-EMS image apply should succeed");

    const std::string storeName = "system_monitor_service_test_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(storeName);
    MemoryPointStore store(storeName);
    PointStoreRouter router;
    router.addStore(storeName, store);
    const std::string systemMonitorStoreName = "system_monitor_points_test_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(systemMonitorStoreName);
    MemoryPointStore systemMonitorStore(systemMonitorStoreName);
    system_monitor_points::registerStorePoints(systemMonitorStore, "GW_TEST");
    router.addStore(systemMonitorStoreName, systemMonitorStore);
    system_monitor_points::addRoutes(router, "GW_TEST", systemMonitorStoreName);
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
    leaseConfig.cellular.enabled = false;
    leaseConfig.cellular.routeFailoverStateFile = root + "/network-failover-state";
    writeFile(
        leaseConfig.cellular.routeFailoverStateFile,
        "mode=cellular\n"
        "last_result=healthy\n"
        "last_checked=2026-07-30T06:00:00Z\n"
        "selected_wired_interface=''\n"
        "failover_enabled=true\n"
        "prefer_cellular=true\n"
        "cellular_interface=usb0\n"
    );
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
    const auto routeTelemetry = lastPayloadForTopic(*leasePublisher, leaseMqtt.systemMonitorTelemetryTopic);
    require(routeTelemetry.find("\"failoverEnabled\":true") != std::string::npos, "telemetry should publish failover enablement");
    require(routeTelemetry.find("\"preferCellular\":true") != std::string::npos, "telemetry should publish 4G priority policy");
    require(routeTelemetry.find("\"usingCellular\":true") != std::string::npos, "telemetry should publish the active 4G route");
    require(routeTelemetry.find("\"activeInterface\":\"usb0\"") != std::string::npos, "telemetry should publish the active route interface");
    const auto cellularEnabled = router.getLatestByLocation(
        systemMonitorStoreName,
        system_monitor_points::kCellularEnabled,
        1770000003000LL
    );
    require(cellularEnabled && cellularEnabled->quality == 1 && cellularEnabled->value == 0.0,
            "disabled cellular monitoring should publish an explicit disabled point");
    const auto cellularRoute = router.getLatestByLocation(
        systemMonitorStoreName,
        system_monitor_points::kCellularUsingRoute,
        1770000003000LL
    );
    require(cellularRoute && cellularRoute->quality == 1 && cellularRoute->value == 1.0,
            "cellular route state should be published to shared memory");
    const auto cellularSignal = router.getLatestByLocation(
        systemMonitorStoreName,
        system_monitor_points::kCellularSignalPercent,
        1770000003000LL
    );
    require(cellularSignal && cellularSignal->quality == 0,
            "unavailable cellular signal must not be exposed as a real zero-percent value");
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

    const std::string lockedStoreName = "system_monitor_service_locked_" + std::to_string(getpid());
    MemoryPointStore::cleanupOrphanedSegment(lockedStoreName);
    MemoryPointStore lockedStore(lockedStoreName);
    PointStoreRouter tolerantRouter;
    tolerantRouter.addStore(storeName, store);
    tolerantRouter.addStore(lockedStoreName, lockedStore);
    tolerantRouter.addRoute(route);
    PointStoreRoute lockedRoute;
    lockedRoute.index = 2001;
    lockedRoute.machineCode = "GW_TEST";
    lockedRoute.meterCode = "LOCKED_METER";
    lockedRoute.pointCode = "LOCKED_POINT";
    lockedRoute.sharedMemoryName = lockedStoreName;
    tolerantRouter.addRoute(lockedRoute);
    {
        setenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC", "1", 1);
        LockedSharedStore lockedSegment(lockedStoreName);
        const auto values = tolerantRouter.getAllLatest(1770000011000LL);
        require(values.size() == 1, "locked store should be skipped without failing the whole snapshot");
        require(values.front().index == 1001, "snapshot should retain values from healthy stores");
        require(tolerantRouter.getLatestByIndex(2001, 1770000011000LL) == NullOpt, "single locked index should return empty");
        require(tolerantRouter.peekPendingWrites().empty(), "locked pending writes should be skipped");
        require(tolerantRouter.getStoreStats().size() == 1, "locked stats store should be skipped");
        unsetenv("GATEWAY_SHARED_MUTEX_LOCK_TIMEOUT_SEC");
    }
    MemoryPointStore::cleanupOrphanedSegment(lockedStoreName);

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
    removeFileIfExists(dlt645Device);
    removeFileIfExists(dlt645Template);
    for (const auto& path : chunkFiles) {
        removeFileIfExists(path);
    }
    removeFileIfExists(leaseConfig.realtimeMeterLeaseFile);
    std::cout << "system_monitor_service_test passed" << std::endl;
    return 0;
}
