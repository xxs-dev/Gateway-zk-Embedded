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

    std::cout << "config_loader_test passed" << std::endl;
    return 0;
}
