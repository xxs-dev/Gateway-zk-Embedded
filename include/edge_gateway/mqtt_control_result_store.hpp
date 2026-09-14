#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

struct MqttControlResultRecord {
    std::string id;
    std::string fingerprint;
    int type = 1;
    double targetKw = 0.0;
    std::uint32_t generation = 0;
    std::int64_t acceptedAtMs = 0;
    std::int64_t deadlineMs = 0;
    std::vector<PointStoreRoute> routes;
    bool submitted = false;
    std::string finalPayload;
    bool delivered = false;
};

enum class MqttControlReserveStatus {
    Inserted,
    Existing,
    FingerprintMismatch
};

class MqttControlResultStore {
public:
    explicit MqttControlResultStore(
        std::string dbPath,
        std::string libraryPath = {},
        int deliveredRetentionDays = 365,
        std::size_t maxDeliveredRecords = 10000
    );
    ~MqttControlResultStore();

    MqttControlResultStore(const MqttControlResultStore&) = delete;
    MqttControlResultStore& operator=(const MqttControlResultStore&) = delete;

    static std::string defaultPathForOwnershipFile(const std::string& ownershipFile);

    MqttControlReserveStatus reservePending(const MqttControlResultRecord& record);
    bool markSubmitted(const std::string& id, const std::string& fingerprint);
    Optional<MqttControlResultRecord> find(const std::string& id) const;
    std::vector<MqttControlResultRecord> loadPending() const;
    std::vector<MqttControlResultRecord> loadUndelivered(std::size_t limit) const;
    bool storeFinalPayload(
        const std::string& id,
        const std::string& fingerprint,
        const std::string& payload
    );
    bool markDelivered(
        const std::string& id,
        const std::string& fingerprint,
        std::int64_t deliveredAtMs
    );
    bool discardPending(const std::string& id, const std::string& fingerprint);
    void cleanupIfDue(std::int64_t nowMs);

private:
    void loadLibrary();
    void openDatabase();
    void ensureSchema();
    void closeDatabase();
    void unloadLibrary();
    Optional<MqttControlResultRecord> findInternal(const std::string& id) const;
    std::vector<PointStoreRoute> loadRoutes(const std::string& id) const;

    std::string dbPath_;
    std::string libraryPath_;
    int deliveredRetentionDays_ = 365;
    std::size_t maxDeliveredRecords_ = 10000;
    std::int64_t lastCleanupMs_ = 0;
    void* libraryHandle_ = nullptr;
    void* databaseHandle_ = nullptr;
};

}  // namespace edge_gateway
