#include "edge_gateway/point_store_router.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

namespace {

std::vector<PointDefinition> effectiveMeterPoints(const DeviceConfig& config, const LogicalDeviceConfig& device) {
    if (!device.points.empty()) {
        return device.points;
    }
    if (config.protocol.type != "dlt645_2007" || config.protocol.standardPointsFile.empty()) {
        return {};
    }
    return ConfigLoader::loadDlt645StandardPointsFromFile(config.protocol.standardPointsFile);
}

std::string defaultInterfaceType(const DeviceConfig& config) {
    if (config.protocol.type == "modbus_tcp") {
        return "ethernet";
    }
    if (config.protocol.type == "dlt645_2007" || config.protocol.type == "modbus_rtu") {
        return "serial";
    }
    return config.protocol.type;
}

}  // namespace

void PointStoreRouter::addStore(const std::string& sharedMemoryName, MemoryPointStore& store) {
    if (sharedMemoryName.empty()) {
        throw std::invalid_argument("sharedMemoryName is required");
    }
    stores_[sharedMemoryName] = &store;
}

void PointStoreRouter::addRoutesFromDeviceConfigs(
    const std::vector<DeviceConfig>& deviceConfigs,
    const std::string& fallbackSharedMemoryName
) {
    for (const auto& config : deviceConfigs) {
        const auto sharedMemoryName = config.memoryStore.sharedMemoryName.empty()
            ? fallbackSharedMemoryName
            : config.memoryStore.sharedMemoryName;
        const auto interfaceType = defaultInterfaceType(config);
        const auto interfaceCode = sharedMemoryName;

        if (!config.meters.empty()) {
            std::size_t meterIndex = 0;
            for (const auto& device : config.meters) {
                auto points = effectiveMeterPoints(config, device);
                if (config.protocol.type == "dlt645_2007" && device.points.empty()) {
                    const std::uint32_t indexBase = 200000U + static_cast<std::uint32_t>(meterIndex) * 10000U;
                    for (std::size_t i = 0; i < points.size(); ++i) {
                        points[i].index = indexBase + static_cast<std::uint32_t>(i);
                    }
                }
                for (const auto& point : points) {
                    PointStoreRoute route;
                    route.index = point.index;
                    route.machineCode = config.machineCode;
                    route.meterCode = device.meterCode;
                    route.pointCode = point.pointCode;
                    route.interfaceCode = interfaceCode;
                    route.interfaceType = interfaceType;
                    route.sharedMemoryName = sharedMemoryName;
                    route.writable = point.write.enable;
                    route.reportOnChange = point.reportOnChange;
                    route.isStore = point.isStore;
                    route.persistIntervalSec = point.persistIntervalSec;
                    addRoute(route);
                }
                ++meterIndex;
            }
        }

        for (const auto& point : config.points) {
            PointStoreRoute route;
            route.index = point.index;
            route.machineCode = config.machineCode;
            route.meterCode = config.meterCode;
            route.pointCode = point.pointCode;
            route.interfaceCode = interfaceCode;
            route.interfaceType = interfaceType;
            route.sharedMemoryName = sharedMemoryName;
            route.writable = point.write.enable;
            route.reportOnChange = point.reportOnChange;
            route.isStore = point.isStore;
            route.persistIntervalSec = point.persistIntervalSec;
            addRoute(route);
        }
    }
}

Optional<PointStoreRoute> PointStoreRouter::routeByIndex(std::uint32_t index) const {
    const auto it = routes_.find(index);
    if (it == routes_.end()) {
        return NullOpt;
    }
    return it->second;
}

const std::unordered_map<std::uint32_t, PointStoreRoute>& PointStoreRouter::routes() const {
    return routes_;
}

std::vector<std::uint32_t> PointStoreRouter::allIndexes() const {
    std::vector<std::uint32_t> indexes;
    indexes.reserve(routes_.size());
    for (const auto& entry : routes_) {
        indexes.push_back(entry.first);
    }
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

Optional<StoredPointValue> PointStoreRouter::getLatestByIndex(std::uint32_t index, std::int64_t nowMs) const {
    const auto route = routeByIndex(index);
    if (!route) {
        return NullOpt;
    }
    auto* store = storeForRoute(*route);
    if (store == nullptr) {
        return NullOpt;
    }
    auto value = store->getLatestByIndex(index, nowMs);
    if (!value) {
        return NullOpt;
    }
    return enrich(*value);
}

std::vector<StoredPointValue> PointStoreRouter::getLatestByIndexes(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    std::unordered_map<std::string, std::vector<std::uint32_t>> grouped;
    for (const auto index : indexes) {
        const auto route = routeByIndex(index);
        if (route) {
            grouped[route->sharedMemoryName].push_back(index);
        }
    }

    std::vector<StoredPointValue> result;
    for (const auto& entry : grouped) {
        const auto storeIt = stores_.find(entry.first);
        if (storeIt == stores_.end() || storeIt->second == nullptr) {
            continue;
        }
        auto values = storeIt->second->getLatestByIndexes(entry.second, nowMs);
        for (auto& value : values) {
            result.push_back(enrich(std::move(value)));
        }
    }
    std::sort(result.begin(), result.end(), [](const StoredPointValue& lhs, const StoredPointValue& rhs) {
        return lhs.index < rhs.index;
    });
    return result;
}

std::vector<StoredPointValue> PointStoreRouter::getAllLatest(std::int64_t nowMs) const {
    return getLatestByIndexes(allIndexes(), nowMs);
}

std::vector<StoredPointValue> PointStoreRouter::getLatestByInterface(
    const std::string& interfaceCode,
    std::int64_t nowMs
) const {
    std::vector<std::uint32_t> indexes;
    for (const auto& entry : routes_) {
        if (entry.second.interfaceCode == interfaceCode) {
            indexes.push_back(entry.first);
        }
    }
    return getLatestByIndexes(indexes, nowMs);
}

std::vector<StoredPointValue> PointStoreRouter::getLatestByMeter(
    const std::string& machineCode,
    const std::string& meterCode,
    std::int64_t nowMs
) const {
    std::vector<std::uint32_t> indexes;
    for (const auto& entry : routes_) {
        if (entry.second.machineCode == machineCode && entry.second.meterCode == meterCode) {
            indexes.push_back(entry.first);
        }
    }
    return getLatestByIndexes(indexes, nowMs);
}

CommandSubmitResult PointStoreRouter::submitWriteCommand(const PendingWriteCommand& command) {
    CommandSubmitResult result;
    const auto route = routeByIndex(command.index);
    if (!route) {
        result.message = "command index not found";
        return result;
    }
    result.route = *route;
    if (!route->writable) {
        result.message = "point write is disabled";
        return result;
    }
    auto* store = storeForRoute(*route);
    if (store == nullptr) {
        result.message = "target shared memory not found: " + route->sharedMemoryName;
        return result;
    }
    store->submitWriteCommand(command);
    result.accepted = true;
    result.message = "write command routed";
    return result;
}

CommandSubmitResult PointStoreRouter::putLatestByIndex(PointValue value) {
    CommandSubmitResult result;
    const auto route = routeByIndex(value.index);
    if (!route) {
        result.message = "latest index not found";
        return result;
    }
    result.route = *route;
    auto* store = storeForRoute(*route);
    if (store == nullptr) {
        result.message = "target shared memory not found: " + route->sharedMemoryName;
        return result;
    }
    value.machineCode = route->machineCode;
    value.meterCode = route->meterCode;
    value.pointCode = route->pointCode;
    value.isStore = route->isStore;
    value.persistIntervalSec = route->persistIntervalSec;
    store->putLatest(value);
    result.accepted = true;
    result.message = "latest value routed";
    return result;
}

std::vector<PendingWriteCommand> PointStoreRouter::peekPendingWrites(std::size_t limit) const {
    std::vector<PendingWriteCommand> result;
    for (const auto& entry : stores_) {
        auto items = entry.second->peekPendingWriteCommands(limit);
        result.insert(result.end(), items.begin(), items.end());
        if (limit > 0 && result.size() >= limit) {
            result.resize(limit);
            break;
        }
    }
    return result;
}

std::vector<MemoryStoreStats> PointStoreRouter::getStoreStats() const {
    std::vector<MemoryStoreStats> result;
    result.reserve(stores_.size());
    for (const auto& entry : stores_) {
        result.push_back(entry.second->getStats());
    }
    std::sort(result.begin(), result.end(), [](const MemoryStoreStats& lhs, const MemoryStoreStats& rhs) {
        return lhs.sharedMemoryName < rhs.sharedMemoryName;
    });
    return result;
}

void PointStoreRouter::addRoute(const PointStoreRoute& route) {
    if (route.index == 0) {
        throw std::invalid_argument("route index must be non-zero");
    }
    const auto inserted = routes_.emplace(route.index, route);
    if (!inserted.second) {
        const auto& existing = inserted.first->second;
        throw std::invalid_argument(
            "duplicate point route index=" + std::to_string(route.index) +
            " existing=" + existing.machineCode + "/" + existing.meterCode + "/" + existing.pointCode +
            " incoming=" + route.machineCode + "/" + route.meterCode + "/" + route.pointCode
        );
    }
}

MemoryPointStore* PointStoreRouter::storeForRoute(const PointStoreRoute& route) const {
    const auto it = stores_.find(route.sharedMemoryName);
    if (it == stores_.end()) {
        return nullptr;
    }
    return it->second;
}

StoredPointValue PointStoreRouter::enrich(StoredPointValue value) const {
    const auto route = routeByIndex(value.index);
    if (!route) {
        return value;
    }
    if (value.machineCode.empty()) {
        value.machineCode = route->machineCode;
    }
    if (value.meterCode.empty()) {
        value.meterCode = route->meterCode;
    }
    if (value.pointCode.empty()) {
        value.pointCode = route->pointCode;
    }
    return value;
}

}  // namespace edge_gateway
