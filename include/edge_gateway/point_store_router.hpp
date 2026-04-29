#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"

namespace edge_gateway {

struct PointStoreRoute {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string interfaceCode;
    std::string interfaceType;
    std::string sharedMemoryName;
    std::string driverService;
    bool writable = false;
    bool reportOnChange = false;
    bool isStore = false;
    int persistIntervalSec = 60;
};

struct CommandSubmitResult {
    bool accepted = false;
    std::string message;
    PointStoreRoute route;
};

class PointStoreRouter {
public:
    void addStore(const std::string& sharedMemoryName, MemoryPointStore& store);
    void addRoutesFromDeviceConfigs(
        const std::vector<DeviceConfig>& deviceConfigs,
        const std::string& fallbackSharedMemoryName
    );
    void addRoute(const PointStoreRoute& route);

    Optional<PointStoreRoute> routeByIndex(std::uint32_t index) const;
    const std::unordered_map<std::uint32_t, PointStoreRoute>& routes() const;
    std::vector<std::uint32_t> allIndexes() const;

    Optional<StoredPointValue> getLatestByIndex(std::uint32_t index, std::int64_t nowMs) const;
    std::vector<StoredPointValue> getLatestByIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const;
    std::vector<StoredPointValue> getAllLatest(std::int64_t nowMs) const;
    std::vector<StoredPointValue> getLatestByInterface(
        const std::string& interfaceCode,
        std::int64_t nowMs
    ) const;
    std::vector<StoredPointValue> getLatestByMeter(
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) const;

    CommandSubmitResult submitWriteCommand(const PendingWriteCommand& command);
    CommandSubmitResult putLatestByIndex(PointValue value);
    std::vector<PendingWriteCommand> peekPendingWrites(std::size_t limit = 0) const;
    std::vector<MemoryStoreStats> getStoreStats() const;

private:
    MemoryPointStore* storeForRoute(const PointStoreRoute& route) const;
    StoredPointValue enrich(StoredPointValue value) const;

    std::unordered_map<std::string, MemoryPointStore*> stores_;
    std::unordered_map<std::uint32_t, PointStoreRoute> routes_;
};

}  // namespace edge_gateway
