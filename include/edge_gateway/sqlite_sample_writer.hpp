#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class SqliteSampleWriter {
public:
    explicit SqliteSampleWriter(
        std::string dbPath,
        std::string libraryPath = "",
        int retentionDays = 30
    );
    ~SqliteSampleWriter();

    SqliteSampleWriter(const SqliteSampleWriter&) = delete;
    SqliteSampleWriter& operator=(const SqliteSampleWriter&) = delete;

    void writeSamples(const std::vector<PersistentPointSample>& samples);
    void writeSamples(
        const std::vector<PersistentPointSample>& samples,
        std::int64_t referenceTimeMs
    );

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void cleanupExpiredSamples(std::int64_t referenceTimeMs);
    void closeDatabase();
    void unloadLibrary();

    std::string dbPath_;
    std::string libraryPath_;
    int retentionDays_ = 30;
    std::int64_t lastCompletedCleanupMs_ = 0;
    bool enabled_ = false;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
