#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "edge_gateway/iec_client.hpp"

namespace edge_gateway {

struct Iec103RecordingJobStatus {
    std::string requestId;
    std::string action;
    std::string stage;
    bool retryable = false;
    int fan = -1;
    Iec103ComtradeFiles files;
    std::vector<Iec103DisturbanceRecord> records;
    std::string errorCode;
    std::string message;
    std::int64_t updatedAtMs = 0;

    bool terminal() const {
        return stage == "completed" || stage == "local_ready" || stage == "failed";
    }
};

std::string defaultIec103RecordingSocketPath(const std::string& configPath);

// Keeps all recording operations inside the live IecDriver process so the
// AM5SE reverse TCP listener is never opened by a competing process.
class Iec103RecordingCommandServer {
public:
    Iec103RecordingCommandServer(
        std::shared_ptr<IecClient> client,
        std::string socketPath,
        std::string recordingDirectory,
        int timeoutMs
    );
    ~Iec103RecordingCommandServer();

    Iec103RecordingCommandServer(const Iec103RecordingCommandServer&) = delete;
    Iec103RecordingCommandServer& operator=(const Iec103RecordingCommandServer&) = delete;

    void start();
    void stop();
    const std::string& socketPath() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Iec103RecordingCommandClient {
public:
    static Iec103RecordingJobStatus submitList(
        const std::string& socketPath,
        const std::string& requestId
    );
    static Iec103RecordingJobStatus submitPull(
        const std::string& socketPath,
        const std::string& requestId,
        int fan
    );
    static Iec103RecordingJobStatus status(
        const std::string& socketPath,
        const std::string& requestId
    );
};

}  // namespace edge_gateway
