#pragma once

#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

class SqliteSampleWriter {
public:
    explicit SqliteSampleWriter(std::string dbPath, std::string libraryPath = "");
    ~SqliteSampleWriter();

    SqliteSampleWriter(const SqliteSampleWriter&) = delete;
    SqliteSampleWriter& operator=(const SqliteSampleWriter&) = delete;

    void writeSamples(const std::vector<PersistentPointSample>& samples);

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void closeDatabase();
    void unloadLibrary();

    std::string dbPath_;
    std::string libraryPath_;
    bool enabled_ = false;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
