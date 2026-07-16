#include "edge_gateway/point_store_router.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
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

void logStoreReadFailure(const std::string& operation, const std::string& sharedMemoryName, const std::exception& ex) {
    std::cerr << "point store router skipped shared memory "
              << sharedMemoryName
              << " during "
              << operation
              << ": "
              << ex.what()
              << std::endl;
}

std::string trimAscii(const std::string& value) {
    const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    });
    const auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseStrictDouble(const std::string& value, double& result) {
    const auto trimmed = trimAscii(value);
    if (trimmed.empty()) {
        return false;
    }
    char* end = nullptr;
    const char* start = trimmed.c_str();
    result = std::strtod(start, &end);
    return end != start && end != nullptr && *end == '\0' && std::isfinite(result);
}

bool valueMatches(double actual, const std::string& expected) {
    double expectedNumber = 0.0;
    if (parseStrictDouble(expected, expectedNumber)) {
        return std::fabs(actual - expectedNumber) <= 1e-9;
    }
    const auto normalized = lowerAscii(trimAscii(expected));
    if (normalized == "true" || normalized == "on" || normalized == "yes") {
        return std::fabs(actual - 1.0) <= 1e-9;
    }
    if (normalized == "false" || normalized == "off" || normalized == "no") {
        return std::fabs(actual) <= 1e-9;
    }
    return false;
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
                    route.sourceIndex = point.index;
                    route.machineCode = config.machineCode;
                    route.meterCode = device.meterCode;
                    route.pointCode = point.pointCode;
                    route.interfaceCode = interfaceCode;
                    route.interfaceType = interfaceType;
                    route.sharedMemoryName = sharedMemoryName;
                    route.writable = point.write.enable;
                    route.commandMailbox = config.protocol.type == "agc_avc_virtual" && point.category == "command";
                    route.fullUpload = point.fullUpload;
                    route.reportOnChange = point.reportOnChange;
                    route.isStore = point.isStore;
                    route.persistIntervalSec = point.persistIntervalSec;
                    route.normalize = point.normalize;
                    addRoute(route);
                    addNormalizeRoute(route, point.normalize);
                }
                ++meterIndex;
            }
        }

        for (const auto& point : config.points) {
            PointStoreRoute route;
            route.index = point.index;
            route.sourceIndex = point.index;
            route.machineCode = config.machineCode;
            route.meterCode = config.meterCode;
            route.pointCode = point.pointCode;
            route.interfaceCode = interfaceCode;
            route.interfaceType = interfaceType;
            route.sharedMemoryName = sharedMemoryName;
            route.writable = point.write.enable;
            route.commandMailbox = config.protocol.type == "agc_avc_virtual" && point.category == "command";
            route.fullUpload = point.fullUpload;
            route.reportOnChange = point.reportOnChange;
            route.isStore = point.isStore;
            route.persistIntervalSec = point.persistIntervalSec;
            route.normalize = point.normalize;
            addRoute(route);
            addNormalizeRoute(route, point.normalize);
        }
    }
}

void PointStoreRouter::addRoutesFromCameraServiceConfig(
    const CameraServiceConfig& cameraConfig,
    const std::string& machineCode
) {
    if (!cameraConfig.enabled) {
        return;
    }
    const auto sharedMemoryName = cameraConfig.sharedMemoryName.empty()
        ? std::string("gateway_point_store")
        : cameraConfig.sharedMemoryName;
    const auto addCameraRoute = [&](const CameraConfig& camera, std::uint32_t index, const std::string& pointCode) {
        if (index == 0) {
            return;
        }
        PointStoreRoute route;
        route.index = index;
        route.sourceIndex = index;
        route.machineCode = machineCode;
        route.meterCode = camera.cameraCode;
        route.pointCode = pointCode;
        route.interfaceCode = "camera";
        route.interfaceType = "camera";
        route.sharedMemoryName = sharedMemoryName;
        route.writable = false;
        route.reportOnChange = pointCode == "camera_online";
        route.isStore = false;
        route.persistIntervalSec = 60;
        addRoute(route);
    };

    for (const auto& camera : cameraConfig.cameras) {
        if (!camera.enabled || camera.cameraCode.empty()) {
            continue;
        }
        addCameraRoute(camera, camera.statusPointIndexes.online, "camera_online");
        addCameraRoute(camera, camera.statusPointIndexes.fps, "camera_fps");
        addCameraRoute(camera, camera.statusPointIndexes.bitrateKbps, "camera_bitrate_kbps");
        addCameraRoute(camera, camera.statusPointIndexes.errorCode, "camera_error_code");
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
    if (route->derived) {
        return getDerivedLatestByRoute(*route, nowMs);
    }
    return getRawLatestByRoute(*route, nowMs);
}

std::vector<StoredPointValue> PointStoreRouter::getLatestByIndexes(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    std::unordered_map<std::string, std::vector<std::uint32_t>> grouped;
    std::vector<PointStoreRoute> derivedRoutes;
    for (const auto index : indexes) {
        const auto route = routeByIndex(index);
        if (route) {
            if (route->derived) {
                derivedRoutes.push_back(*route);
            } else {
                grouped[route->sharedMemoryName].push_back(index);
            }
        }
    }

    std::vector<StoredPointValue> result;
    for (const auto& entry : grouped) {
        const auto storeIt = stores_.find(entry.first);
        if (storeIt == stores_.end() || storeIt->second == nullptr) {
            continue;
        }
        std::vector<StoredPointValue> values;
        try {
            values = storeIt->second->getLatestByIndexes(entry.second, nowMs);
        } catch (const std::exception& ex) {
            logStoreReadFailure("getLatestByIndexes", entry.first, ex);
            continue;
        }
        for (auto& value : values) {
            result.push_back(enrich(std::move(value)));
        }
    }
    for (const auto& route : derivedRoutes) {
        auto value = getDerivedLatestByRoute(route, nowMs);
        if (value) {
            result.push_back(*value);
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
    const auto commandTime = command.ts > 0 ? command.ts : command.acceptedAt;
    if (!command.highPriority && powerControlOwnership_ &&
        powerControlOwnership_->isBlocked(command.index, command.source, commandTime)) {
        result.message = "power control target is owned by another controller";
        return result;
    }
    if (!route->writable) {
        result.message = "point write is disabled";
        return result;
    }
    auto* store = storeForRoute(*route);
    if (store == nullptr) {
        result.message = "target shared memory not found: " + route->sharedMemoryName;
        return result;
    }
    try {
        store->submitWriteCommand(command);
    } catch (const std::exception& ex) {
        result.message = ex.what();
        return result;
    }
    result.accepted = true;
    result.message = "write command routed";
    return result;
}

CommandSubmitResult PointStoreRouter::submitCommandMailbox(const PendingWriteCommand& command) {
    CommandSubmitResult result;
    const auto route = routeByIndex(command.index);
    if (!route) {
        result.message = "command mailbox index not found";
        return result;
    }
    result.route = *route;
    if (!route->commandMailbox) {
        result.message = "target is not an AGC/AVC command mailbox point";
        return result;
    }
    auto* store = storeForRoute(*route);
    if (store == nullptr) {
        result.message = "target shared memory not found: " + route->sharedMemoryName;
        return result;
    }
    PointValue value;
    value.index = route->index;
    value.machineCode = route->machineCode;
    value.meterCode = route->meterCode;
    value.pointCode = route->pointCode;
    value.category = "command";
    value.value = command.value;
    value.quality = 1;
    value.qualityMsg = command.source.empty() ? "command-mailbox" : command.source;
    value.ts = command.ts > 0 ? command.ts : command.acceptedAt;
    value.expireAt = value.ts + 600000;
    try {
        store->putLatest(value);
    } catch (const std::exception& ex) {
        result.message = ex.what();
        return result;
    }
    result.accepted = true;
    result.message = "command mailbox value committed";
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
    if (route->derived) {
        result.message = "derived point is read-only";
        return result;
    }
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
        if (entry.second == nullptr) {
            continue;
        }
        std::vector<PendingWriteCommand> items;
        try {
            items = entry.second->peekPendingWriteCommands(limit);
        } catch (const std::exception& ex) {
            logStoreReadFailure("peekPendingWrites", entry.first, ex);
            continue;
        }
        result.insert(result.end(), items.begin(), items.end());
        if (limit > 0 && result.size() >= limit) {
            result.resize(limit);
            break;
        }
    }
    return result;
}

Optional<WritebackResultRecord> PointStoreRouter::getWritebackResult(
    const PointStoreRoute& route,
    const std::string& cmdId
) const {
    auto* store = storeForRoute(route);
    if (store == nullptr) {
        return NullOpt;
    }
    try {
        return store->getWritebackResult(cmdId);
    } catch (const std::exception& ex) {
        logStoreReadFailure("getWritebackResult", route.sharedMemoryName, ex);
        return NullOpt;
    }
}

std::vector<MemoryStoreStats> PointStoreRouter::getStoreStats() const {
    std::vector<MemoryStoreStats> result;
    result.reserve(stores_.size());
    for (const auto& entry : stores_) {
        if (entry.second == nullptr) {
            continue;
        }
        try {
            result.push_back(entry.second->getStats());
        } catch (const std::exception& ex) {
            logStoreReadFailure("getStoreStats", entry.first, ex);
        }
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

void PointStoreRouter::setPowerControlOwnershipFile(const std::string& path, const std::string& owner) {
    powerControlOwnership_.reset(path.empty() ? nullptr : new PowerControlOwnership(path, owner));
}

MemoryPointStore* PointStoreRouter::storeForRoute(const PointStoreRoute& route) const {
    const auto it = stores_.find(route.sharedMemoryName);
    if (it == stores_.end()) {
        return nullptr;
    }
    return it->second;
}

Optional<PointStoreRoute> PointStoreRouter::routeByPointCode(
    const std::string& machineCode,
    const std::string& meterCode,
    const std::string& pointCode
) const {
    Optional<PointStoreRoute> fallback;
    for (const auto& entry : routes_) {
        const auto& route = entry.second;
        if (route.machineCode != machineCode || route.meterCode != meterCode || route.pointCode != pointCode) {
            continue;
        }
        if (!route.derived) {
            return route;
        }
        fallback = route;
    }
    return fallback;
}

Optional<StoredPointValue> PointStoreRouter::getRawLatestByRoute(
    const PointStoreRoute& route,
    std::int64_t nowMs
) const {
    auto* store = storeForRoute(route);
    if (store == nullptr) {
        return NullOpt;
    }
    Optional<StoredPointValue> value;
    try {
        value = store->getLatestByIndex(route.index, nowMs);
    } catch (const std::exception& ex) {
        logStoreReadFailure("getLatestByIndex", route.sharedMemoryName, ex);
        return NullOpt;
    }
    if (!value) {
        return NullOpt;
    }
    return enrich(*value);
}

Optional<StoredPointValue> PointStoreRouter::getDerivedLatestByRoute(
    const PointStoreRoute& route,
    std::int64_t nowMs
) const {
    const auto sourceRoute = routeByIndex(route.sourceIndex);
    if (!sourceRoute || sourceRoute->derived) {
        return NullOpt;
    }
    const auto sourceValue = getRawLatestByRoute(*sourceRoute, nowMs);
    if (!sourceValue) {
        return NullOpt;
    }

    StoredPointValue result = *sourceValue;
    result.index = route.index;
    result.machineCode = route.machineCode;
    result.meterCode = route.meterCode;
    result.pointCode = route.pointCode;

    if (sourceValue->quality == 1 && !sourceValue->stale) {
        for (const auto& rule : route.normalize.faultRules) {
            const auto faultRoute = routeByPointCode(route.machineCode, route.meterCode, rule.sourcePointCode);
            if (!faultRoute || faultRoute->index == route.index) {
                continue;
            }
            const auto faultValue = faultRoute->derived
                ? getDerivedLatestByRoute(*faultRoute, nowMs)
                : getRawLatestByRoute(*faultRoute, nowMs);
            if (!faultValue || faultValue->quality != 1 || faultValue->stale) {
                continue;
            }
            if (valueMatches(faultValue->value, rule.triggerValue)) {
                result.value = rule.standardValue;
                return result;
            }
        }
    }

    for (const auto& mapping : route.normalize.mappings) {
        if (valueMatches(sourceValue->value, mapping.rawValue)) {
            result.value = mapping.standardValue;
            return result;
        }
    }

    result.value = route.normalize.unknownValue;
    return result;
}

std::uint32_t PointStoreRouter::allocateDerivedIndex(std::uint32_t configuredIndex) {
    if (configuredIndex != 0 && routes_.find(configuredIndex) == routes_.end()) {
        return configuredIndex;
    }
    while (nextDerivedIndex_ != 0 && routes_.find(nextDerivedIndex_) != routes_.end()) {
        ++nextDerivedIndex_;
    }
    if (nextDerivedIndex_ == 0) {
        throw std::runtime_error("derived point index range exhausted");
    }
    return nextDerivedIndex_++;
}

void PointStoreRouter::addNormalizeRoute(
    const PointStoreRoute& sourceRoute,
    const ValueNormalizeConfig& normalize
) {
    if (!normalize.enabled) {
        return;
    }
    const auto type = lowerAscii(trimAscii(normalize.type));
    if (!type.empty() && type != "enum") {
        return;
    }
    const auto targetPointCode = normalize.targetPointCode.empty()
        ? normalize.targetSemanticRole
        : normalize.targetPointCode;
    if (targetPointCode.empty()) {
        return;
    }

    PointStoreRoute route = sourceRoute;
    route.index = allocateDerivedIndex(normalize.targetIndex);
    route.sourceIndex = sourceRoute.index;
    route.pointCode = targetPointCode;
    route.derived = true;
    route.writable = false;
    route.isStore = false;
    route.normalize = normalize;
    addRoute(route);
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
