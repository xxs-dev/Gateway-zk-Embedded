#include "edge_gateway/iec103_recording_transfer_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "edge_gateway/iec103_recording_command.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write test file: " + path);
    output << content;
}

class FakeIecClient final : public edge_gateway::IecClient {
public:
    explicit FakeIecClient(std::string outputDirectory)
        : outputDirectory_(std::move(outputDirectory)) {}

    std::vector<edge_gateway::IecDataValue> poll() override { return {}; }

    std::vector<edge_gateway::Iec103DisturbanceRecord> listDisturbanceRecords(int) override {
        edge_gateway::Iec103DisturbanceRecord record;
        record.fan = 15;
        record.state = 1;
        record.timestampMs = 1785315600000LL;
        record.rawTimeHex = "010201011A";
        return {record};
    }

    edge_gateway::Iec103ComtradeFiles pullComtradeRecording(
        int fan,
        const std::string&,
        int
    ) override {
        if (progress_) progress_("pulling_cfg");
        const auto cfg = outputDirectory_ + "/FAN00015.CFG";
        const auto dat = outputDirectory_ + "/FAN00015.DAT";
        writeFile(cfg, "CFG-CONTENT");
        if (progress_) progress_("pulling_dat");
        writeFile(dat, "DAT-CONTENT");
        edge_gateway::Iec103ComtradeFiles result;
        result.fan = fan;
        result.cfgPath = cfg;
        result.cfgBytes = 11;
        result.datPath = dat;
        result.datBytes = 11;
        return result;
    }

    void setRecordingProgressCallback(std::function<void(const std::string&)> callback) override {
        progress_ = std::move(callback);
    }

private:
    std::string outputDirectory_;
    std::function<void(const std::string&)> progress_;
};

class CapturingPublisher final : public edge_gateway::IMqttDriverPublisher {
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
    std::vector<edge_gateway::MqttIncomingMessage> pollIncoming(int) override { return {}; }

    std::vector<std::string> topics;
    std::vector<std::string> payloads;
};

class FakeHttpClient final : public edge_gateway::IRecordingHttpClient {
public:
    edge_gateway::RecordingHttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& bodyFile
    ) override {
        calls.push_back(method + " " + url);
        allHeaders.push_back(headers);
        if (method == "GET" && url.find("/parts") != std::string::npos) {
            return {200, "{\"cfg\":[],\"dat\":[]}"};
        }
        if (method == "PUT") {
            std::ifstream input(bodyFile.c_str(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            uploadedBodies.push_back(body);
            return {200, "{\"success\":true}"};
        }
        if (method == "POST" && url.find("/complete") != std::string::npos) {
            ++completeCalls;
            if (onComplete) onComplete();
            return {completeStatusCode, "{\"status\":\"completed\",\"ackPublished\":true}"};
        }
        return {200, "{\"success\":true}"};
    }

    std::vector<std::string> calls;
    std::vector<std::vector<std::string>> allHeaders;
    std::vector<std::string> uploadedBodies;
    std::function<void()> onComplete;
    int completeStatusCode = 200;
    int completeCalls = 0;
};

class OneShotHttpServer {
public:
    OneShotHttpServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("cannot create recording HTTP test listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            close(listener_);
            throw std::runtime_error("cannot bind recording HTTP test listener");
        }
        socklen_t length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            close(listener_);
            throw std::runtime_error("cannot read recording HTTP test port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~OneShotHttpServer() {
        if (listener_ >= 0) {
            shutdown(listener_, SHUT_RDWR);
            close(listener_);
            listener_ = -1;
        }
        wait();
    }

    int port() const { return port_; }
    const std::string& requestText() const { return requestText_; }

    void wait() {
        if (thread_.joinable()) thread_.join();
    }

private:
    void serve() {
        const int client = accept(listener_, nullptr, nullptr);
        if (client < 0) return;
        char buffer[2048];
        while (requestText_.find("\r\n\r\n") == std::string::npos && requestText_.size() < 16384) {
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            requestText_.append(buffer, static_cast<std::size_t>(count));
        }
        const std::string body = "{\"ok\":true}";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::size_t offset = 0;
        while (offset < response.size()) {
            const auto count = send(client, response.data() + offset, response.size() - offset, 0);
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        close(client);
    }

    int listener_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::string requestText_;
};

#endif

}  // namespace

int main() {
#ifdef _WIN32
    std::cout << "IEC103 recording transfer test skipped on Windows" << std::endl;
    return EXIT_SUCCESS;
#else
    using namespace edge_gateway;
    const auto root = std::string("/tmp/iec103-recording-transfer-") + std::to_string(getpid());
    mkdir(root.c_str(), 0750);
    const auto socketPath = root + "/iec.sock";
    const auto configPath = root + "/device_am5se.json";
    const auto queuePath = root + "/queue.tsv";
    const auto workPath = root + "/work";
    mkdir(workPath.c_str(), 0750);
    setenv("NO_PROXY", "127.0.0.1", 1);
    setenv("no_proxy", "127.0.0.1", 1);
    {
        OneShotHttpServer server;
        CurlRecordingHttpClient client("curl", workPath, true, 10);
        const auto response = client.request(
            "GET",
            "http://127.0.0.1:" + std::to_string(server.port()) + "/recording-test",
            {"Authorization: RecordingUpload curl-test-token"},
            ""
        );
        server.wait();
        require(response.statusCode == 200 && response.body == "{\"ok\":true}",
            "curl recording client response");
        require(server.requestText().find("Authorization: RecordingUpload curl-test-token") != std::string::npos,
            "curl recording client must load the private header config");
    }
    writeFile(configPath,
        std::string("{\"schemaVersion\":\"1.0.0\",\"meterCode\":\"AM5SE_T_01\",") +
        "\"protocol\":{\"type\":\"iec103\",\"iec\":{\"transportMode\":\"am5se_passive_tcp\"," +
        "\"recordingCommandSocket\":\"" + socketPath + "\"}},\"meters\":[]}");

    auto iecClient = std::make_shared<FakeIecClient>(root);
    Iec103RecordingCommandServer commandServer(iecClient, socketPath, root, 1000);
    commandServer.start();
    auto publisher = std::make_shared<CapturingPublisher>();
    auto http = std::make_shared<FakeHttpClient>();
    SystemMonitorConfig::RecordingTransferConfig transferConfig;
    transferConfig.enabled = true;
    transferConfig.queueFile = queuePath;
    transferConfig.workDirectory = workPath;
    transferConfig.allowInsecureHttp = true;
    transferConfig.retryBaseSec = 1;
    transferConfig.retryMaxSec = 2;
    MqttConfig mqtt;
    mqtt.recordingReplyTopic = "recording/reply";
    mqtt.recordingStatusTopic = "recording/status";

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string request =
        std::string("{\"requestId\":\"REC_TEST_001\",\"action\":\"pull\",") +
        "\"machineCode\":\"COMM202600102\",\"deviceCode\":\"AM5SE_T_01\",\"fan\":15," +
        "\"upload\":{\"sessionId\":\"RS_TEST_001\",\"baseUrl\":\"http://platform/upload/RS_TEST_001\"," +
        "\"token\":\"secret-token\",\"expireAtMs\":" + std::to_string(now + 60000) +
        ",\"partSize\":65536}}";

    {
        Iec103RecordingTransferService service(
            transferConfig, mqtt, publisher, "COMM202600102", {configPath}, http);
        service.handleRequest(request, now);
        for (int i = 0; i < 100 && http->completeCalls == 0; ++i) {
            service.runPendingOnce(now + i * 300);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(http->completeCalls == 1, "recording upload complete request");
        require(http->uploadedBodies.size() == 2, "CFG and DAT must each upload one part");
        require(service.pendingTaskCount() == 1, "task must wait for platform MQTT ACK");
        require(std::ifstream(queuePath.c_str()).good(), "recording queue must be persisted");
        struct stat queueStat {};
        require(stat(queuePath.c_str(), &queueStat) == 0 && (queueStat.st_mode & 0777) == 0600,
            "recording queue must be private");
        require(std::ifstream((root + "/FAN00015.CFG").c_str()).good(), "CFG retained before ACK");
    }

    {
        Iec103RecordingTransferService resumed(
            transferConfig, mqtt, publisher, "COMM202600102", {configPath}, http);
        require(resumed.pendingTaskCount() == 1, "recording task must survive service restart");
        resumed.handleAck(
            "{\"requestId\":\"REC_TEST_001\",\"recordingId\":\"RID1\","
            "\"machineCode\":\"COMM202600102\",\"status\":\"completed\"}",
            now + 5000
        );
        require(resumed.pendingTaskCount() == 0, "ACK must remove completed queue task");
        require(!std::ifstream((root + "/FAN00015.CFG").c_str()).good(), "CFG removed only after ACK");
        require(!std::ifstream((root + "/FAN00015.DAT").c_str()).good(), "DAT removed only after ACK");
    }

    {
        Iec103RecordingTransferService duplicate(
            transferConfig, mqtt, publisher, "COMM202600102", {configPath}, http);
        duplicate.handleRequest(request, now + 120000);
        require(duplicate.pendingTaskCount() == 0, "completed duplicate must remain a tombstone, not a pending task");
        require(!duplicate.runPendingOnce(now + 120000), "completed duplicate must not pull or upload again");
        require(http->completeCalls == 1, "completed duplicate must not call the platform again");
    }

    {
        const std::string raceRequest =
            std::string("{\"requestId\":\"REC_TEST_002\",\"action\":\"pull\",") +
            "\"machineCode\":\"COMM202600102\",\"deviceCode\":\"AM5SE_T_01\",\"fan\":15," +
            "\"upload\":{\"sessionId\":\"RS_TEST_002\",\"baseUrl\":\"http://platform/upload/RS_TEST_002\"," +
            "\"token\":\"secret-token\",\"expireAtMs\":" + std::to_string(now + 60000) +
            ",\"partSize\":65536}}";
        Iec103RecordingTransferService service(
            transferConfig, mqtt, publisher, "COMM202600102", {configPath}, http);
        http->completeStatusCode = 500;
        http->onComplete = [&service, now] {
            service.handleAck(
                "{\"requestId\":\"REC_TEST_002\",\"recordingId\":\"RID2\","
                "\"machineCode\":\"COMM202600102\",\"status\":\"completed\"}",
                now + 6000
            );
        };
        service.handleRequest(raceRequest, now);
        for (int i = 0; i < 100 && http->completeCalls < 2; ++i) {
            service.runPendingOnce(now + i * 300);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        http->onComplete = {};
        http->completeStatusCode = 200;
        require(http->completeCalls == 2, "recording race upload complete request");
        require(service.pendingTaskCount() == 0, "ACK must win even when the HTTPS response reports failure");
        require(!std::ifstream((root + "/FAN00015.CFG").c_str()).good(), "early ACK removes CFG after verification");
        require(!std::ifstream((root + "/FAN00015.DAT").c_str()).good(), "early ACK removes DAT after verification");
    }

    require(std::find(publisher->topics.begin(), publisher->topics.end(), "recording/reply") != publisher->topics.end(),
        "recording acceptance reply must be published");
    require(std::find(publisher->topics.begin(), publisher->topics.end(), "recording/status") != publisher->topics.end(),
        "recording progress status must be published");
    bool tokenWasHeader = false;
    for (const auto& headers : http->allHeaders) {
        tokenWasHeader = tokenWasHeader || std::find(
            headers.begin(), headers.end(), "Authorization: RecordingUpload secret-token") != headers.end();
    }
    require(tokenWasHeader, "short-lived token must be sent as RecordingUpload authorization");

    commandServer.stop();
    std::remove(queuePath.c_str());
    std::remove(configPath.c_str());
    rmdir(workPath.c_str());
    rmdir(root.c_str());
    std::cout << "IEC103 recording transfer service test passed" << std::endl;
    return EXIT_SUCCESS;
#endif
}
