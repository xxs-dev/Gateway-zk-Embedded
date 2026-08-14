#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/power_control_ownership.hpp"

namespace edge_gateway {

struct PointStoreRoute {
    std::uint32_t index = 0;
    std::uint32_t sourceIndex = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string category;
    std::string protocolType;
    std::string interfaceCode;
    std::string interfaceType;
    std::string sharedMemoryName;
    std::string driverService;
    bool derived = false;
    bool writable = false;
    bool commandMailbox = false;
    bool fullUpload = false;
    bool reportOnChange = false;
    bool isStore = false;
    int persistIntervalSec = 60;
    int ttlMs = 600000;
    Optional<double> initialValue;
    bool retain = false;
    WriteSpec write;
    ValueNormalizeConfig normalize;
};

struct CommandSubmitResult {
    bool accepted = false;
    std::string message;
    PointStoreRoute route;
};

struct CommandGroupSubmitResult {
    bool accepted = false;
    std::string message;
    std::vector<PointStoreRoute> routes;
};

class PointStoreRouter {
public:
    PointStoreRouter();

    void addStore(const std::string& sharedMemoryName, MemoryPointStore& store);
    void addRoutesFromDeviceConfigs(
        const std::vector<DeviceConfig>& deviceConfigs,
        const std::string& fallbackSharedMemoryName
    );
    void addRoutesFromCameraServiceConfig(
        const CameraServiceConfig& cameraConfig,
        const std::string& machineCode
    );
    void addRoute(const PointStoreRoute& route);
    void setPowerControlOwnershipFile(const std::string& path, const std::string& owner);
    void setEmsVirtualParameterDirectory(const std::string& directory);

    Optional<PointStoreRoute> routeByIndex(std::uint32_t index) const;
    Optional<PointStoreRoute> routeByLocation(
        const std::string& sharedMemoryName,
        std::uint32_t index
    ) const;
    const std::unordered_map<std::uint32_t, PointStoreRoute>& routes() const;
    std::vector<std::uint32_t> allIndexes() const;

    Optional<StoredPointValue> getLatestByIndex(std::uint32_t index, std::int64_t nowMs) const;
    Optional<StoredPointValue> getLatestByLocation(
        const std::string& sharedMemoryName,
        std::uint32_t index,
        std::int64_t nowMs
    ) const;
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
    CommandSubmitResult submitWriteCommand(
        const PointStoreRoute& route,
        const PendingWriteCommand& command
    );
    CommandGroupSubmitResult submitWriteCommands(const std::vector<PendingWriteCommand>& commands);
    CommandSubmitResult submitCommandMailbox(const PendingWriteCommand& command);
    CommandSubmitResult putLatestByIndex(PointValue value);
    CommandSubmitResult putLatestByLocation(
        const std::string& sharedMemoryName,
        PointValue value
    );
    std::vector<PendingWriteCommand> peekPendingWrites(std::size_t limit = 0) const;
    Optional<WritebackResultRecord> getWritebackResult(const PointStoreRoute& route, const std::string& cmdId) const;
    Optional<WritebackResultRecord> getWritebackResult(
        const PointStoreRoute& route,
        const std::string& cmdId,
        std::uint32_t index
    ) const;
    std::vector<MemoryStoreStats> getStoreStats() const;

private:
    MemoryPointStore* storeForRoute(const PointStoreRoute& route) const;
    StoredPointValue enrich(StoredPointValue value) const;
    Optional<PointStoreRoute> routeByPointCode(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode
    ) const;
    Optional<StoredPointValue> getRawLatestByRoute(const PointStoreRoute& route, std::int64_t nowMs) const;
    Optional<StoredPointValue> getDerivedLatestByRoute(const PointStoreRoute& route, std::int64_t nowMs) const;
    std::uint32_t allocateDerivedIndex(std::uint32_t configuredIndex);
    void addNormalizeRoute(const PointStoreRoute& sourceRoute, const ValueNormalizeConfig& normalize);
    void restoreEmsVirtualParameter(const PointStoreRoute& route);

    std::unordered_map<std::string, MemoryPointStore*> stores_;
    std::unordered_map<std::uint32_t, PointStoreRoute> routes_;
    std::unordered_map<std::string, PointStoreRoute> routesByLocation_;
    std::uint32_t nextDerivedIndex_ = 900000000U;
    std::unique_ptr<PowerControlOwnership> powerControlOwnership_;
    std::string emsVirtualParameterDirectory_;
    std::unordered_map<std::string, double> emsVirtualPersistedValues_;
};

}  // namespace edge_gateway
