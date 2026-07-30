#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct RecordingHttpResponse {
    int statusCode = 0;
    std::string body;
};

class IRecordingHttpClient {
public:
    virtual ~IRecordingHttpClient() = default;

    virtual RecordingHttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& bodyFile
    ) = 0;
};

class CurlRecordingHttpClient final : public IRecordingHttpClient {
public:
    CurlRecordingHttpClient(
        std::string executable,
        std::string workDirectory,
        bool allowInsecureHttp,
        int timeoutSec
    );

    RecordingHttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers,
        const std::string& bodyFile
    ) override;

private:
    std::string executable_;
    std::string workDirectory_;
    bool allowInsecureHttp_ = false;
    int timeoutSec_ = 180;
};

// Owns the durable MQTT-control/HTTPS-data workflow. Device communication is
// delegated to the live IecDriver through its Unix recording command socket.
class Iec103RecordingTransferService {
public:
    Iec103RecordingTransferService(
        SystemMonitorConfig::RecordingTransferConfig config,
        MqttConfig mqttConfig,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::string machineCode,
        std::vector<std::string> configFiles,
        std::shared_ptr<IRecordingHttpClient> httpClient = nullptr
    );
    ~Iec103RecordingTransferService();

    Iec103RecordingTransferService(const Iec103RecordingTransferService&) = delete;
    Iec103RecordingTransferService& operator=(const Iec103RecordingTransferService&) = delete;

    void start();
    void stop();
    void handleRequest(const std::string& payload, std::int64_t nowMs);
    void handleAck(const std::string& payload, std::int64_t nowMs);

    // Exposed for deterministic edge tests; production uses the worker thread.
    bool runPendingOnce(std::int64_t nowMs);
    std::size_t pendingTaskCount() const;

private:
    struct Task {
        std::string requestId;
        std::string action;
        std::string deviceCode;
        int fan = -1;
        std::string sessionId;
        std::string baseUrl;
        std::string token;
        std::int64_t expireAtMs = 0;
        int partSize = 4 * 1024 * 1024;
        std::string socketPath;
        std::string stage = "accepted";
        std::string cfgPath;
        std::uint64_t cfgBytes = 0;
        std::string cfgSha256;
        std::string datPath;
        std::uint64_t datBytes = 0;
        std::string datSha256;
        int retryCount = 0;
        std::int64_t nextRetryAtMs = 0;
        std::string lastError;
    };

    void loop();
    void loadQueue();
    void saveQueueLocked() const;
    void pruneCompletedLocked();
    void processTask(Task task, std::int64_t nowMs);
    void processLocalCommand(Task& task, std::int64_t nowMs);
    void processUpload(Task& task, std::int64_t nowMs);
    void uploadFileParts(Task& task, const std::string& kind, const std::vector<int>& received);
    bool applyTask(const Task& task);
    void completeListTask(const Task& task, const std::string& replyPayload);
    void scheduleRetry(
        const Task& task,
        const std::string& error,
        std::int64_t nowMs,
        bool retryable = true
    );
    void publishReply(const Task& task, const std::string& stage, const std::string& message) const;
    void publishStatus(
        const Task& task,
        const std::string& stage,
        const std::string& message,
        bool retryable = false,
        const std::string& errorCode = std::string()
    ) const;

    SystemMonitorConfig::RecordingTransferConfig config_;
    MqttConfig mqttConfig_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::string machineCode_;
    std::vector<std::string> configFiles_;
    std::shared_ptr<IRecordingHttpClient> httpClient_;
    mutable std::mutex mutex_;
    std::map<std::string, Task> tasks_;
    std::atomic<bool> running_{false};
    std::thread workerThread_;
};

}  // namespace edge_gateway
