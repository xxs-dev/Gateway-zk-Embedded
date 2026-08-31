#include "edge_gateway/point_store_router.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

namespace {

constexpr const char* kDefaultEmsVirtualParameterDirectory =
    "/opt/modbus-gateway/data/ems-virtual-parameters";

std::mutex& emsVirtualPersistenceMutex() {
    static std::mutex mutex;
    return mutex;
}

std::mutex& emsVirtualPersistenceErrorMutex() {
    static std::mutex mutex;
    return mutex;
}

void logEmsVirtualPersistenceErrorOnce(
    const std::string& operation,
    std::uint32_t index,
    const std::exception& ex
) {
    static std::unordered_set<std::string> reported;
    const auto key = operation + '\x1f' + ex.what();
    std::lock_guard<std::mutex> lock(emsVirtualPersistenceErrorMutex());
    if (!reported.insert(key).second) {
        return;
    }
    std::cerr << "point store router EMS virtual retention " << operation
              << " failed; live value remains available"
              << " firstIndex=" << index
              << " error=" << ex.what()
              << std::endl;
}

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool isPathSeparator(char ch) {
#ifdef _WIN32
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

void createDirectoryIfMissing(const std::string& path) {
    if (path.empty() || path == "/" || path == "\\" ||
        (path.size() == 2 && path[1] == ':')) {
        return;
    }
#ifdef _WIN32
    const int result = CreateDirectoryA(path.c_str(), nullptr) != 0 ? 0 : -1;
    const auto error = result == 0 ? ERROR_SUCCESS : GetLastError();
    if (result != 0 && error != ERROR_ALREADY_EXISTS) {
        throw std::runtime_error("failed to create EMS virtual parameter directory: " + path);
    }
#else
    if (::mkdir(path.c_str(), 0750) != 0 && errno != EEXIST) {
        throw std::runtime_error(
            "failed to create EMS virtual parameter directory " + path + ": " + std::strerror(errno)
        );
    }
#endif
}

void ensureDirectory(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("EMS virtual parameter directory is empty");
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (!isPathSeparator(path[i])) {
            continue;
        }
        auto partial = path.substr(0, i);
        while (!partial.empty() && isPathSeparator(partial.back())) {
            partial.pop_back();
        }
        createDirectoryIfMissing(partial);
    }
    auto finalPath = path;
    while (finalPath.size() > 1 && isPathSeparator(finalPath.back())) {
        finalPath.pop_back();
    }
    createDirectoryIfMissing(finalPath);
}

std::string joinPath(const std::string& directory, const std::string& name) {
    if (directory.empty() || isPathSeparator(directory.back())) {
        return directory + name;
    }
#ifdef _WIN32
    return directory + "\\" + name;
#else
    return directory + "/" + name;
#endif
}

std::string routeLocationKey(const std::string& sharedMemoryName, std::uint32_t index) {
    return sharedMemoryName + '\x1f' + std::to_string(index);
}

std::string parameterFilePath(const std::string& directory, std::uint32_t index) {
    return joinPath(directory, std::to_string(index) + ".value");
}

std::string temporaryParameterFilePath(const std::string& target) {
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto processId = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto processId = static_cast<std::uint64_t>(::getpid());
#endif
    return target + ".tmp." + std::to_string(processId) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

void flushFileOrThrow(std::FILE* file, const std::string& path) {
    if (std::fflush(file) != 0) {
        throw std::runtime_error("failed to flush EMS virtual parameter file " + path);
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        throw std::runtime_error("failed to sync EMS virtual parameter file " + path);
    }
#else
    if (::fsync(fileno(file)) != 0) {
        throw std::runtime_error("failed to sync EMS virtual parameter file " + path);
    }
#endif
}

void replaceFileAtomically(const std::string& temporary, const std::string& target) {
#ifdef _WIN32
    if (MoveFileExA(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) == 0) {
        throw std::runtime_error("failed to replace EMS virtual parameter file " + target);
    }
#else
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        throw std::runtime_error(
            "failed to replace EMS virtual parameter file " + target + ": " + std::strerror(errno)
        );
    }
#endif
}

void syncDirectoryBestEffort(const std::string& directory) {
#ifndef _WIN32
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    if (descriptor >= 0) {
        (void)::fsync(descriptor);
        (void)::close(descriptor);
    }
#else
    (void)directory;
#endif
}

void persistParameterAtomically(
    const std::string& directory,
    std::uint32_t index,
    double value,
    std::unordered_map<std::string, double>& persistedValues
) {
    std::lock_guard<std::mutex> lock(emsVirtualPersistenceMutex());
    ensureDirectory(directory);
    const auto target = parameterFilePath(directory, index);
    const auto cached = persistedValues.find(target);
    if (cached != persistedValues.end() && std::fabs(cached->second - value) <= 1e-12) {
        return;
    }
    const auto temporary = temporaryParameterFilePath(target);
    std::ostringstream content;
    content << std::setprecision(std::numeric_limits<double>::max_digits10) << value << '\n';
    const auto text = content.str();

    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        throw std::runtime_error("failed to open EMS virtual parameter file " + temporary);
    }
    try {
        if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) {
            throw std::runtime_error("failed to write EMS virtual parameter file " + temporary);
        }
        flushFileOrThrow(file, temporary);
        if (std::fclose(file) != 0) {
            file = nullptr;
            throw std::runtime_error("failed to close EMS virtual parameter file " + temporary);
        }
        file = nullptr;
        replaceFileAtomically(temporary, target);
        syncDirectoryBestEffort(directory);
        persistedValues[target] = value;
    } catch (...) {
        if (file != nullptr) {
            std::fclose(file);
        }
        std::remove(temporary.c_str());
        throw;
    }
}

Optional<double> loadPersistedParameter(
    const std::string& directory,
    std::uint32_t index,
    std::unordered_map<std::string, double>& persistedValues
) {
    std::lock_guard<std::mutex> lock(emsVirtualPersistenceMutex());
    const auto path = parameterFilePath(directory, index);
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return NullOpt;
    }
    double value = 0.0;
    std::string trailing;
    if (!(input >> value) || !std::isfinite(value) || (input >> trailing)) {
        throw std::runtime_error("persisted EMS virtual parameter is invalid");
    }
    persistedValues[path] = value;
    return value;
}

bool isEmsVirtualPoint(const PointStoreRoute& route) {
    return !route.derived && route.protocolType == "ems_virtual";
}

bool isDirectEmsVirtualParameter(const PointStoreRoute& route) {
    return isEmsVirtualPoint(route) && route.writable && route.write.enable;
}

bool isRetainedEmsVirtualPoint(const PointStoreRoute& route) {
    return isEmsVirtualPoint(route) && route.retain;
}

std::string validateEmsVirtualParameter(const PointStoreRoute& route, double value) {
    if (!std::isfinite(value)) {
        return "value must be finite";
    }
    if (route.write.minValue && value < *route.write.minValue) {
        return "value below min";
    }
    if (route.write.maxValue && value > *route.write.maxValue) {
        return "value above max";
    }
    if (route.write.step > 0.0) {
        const auto base = route.write.minValue ? *route.write.minValue : 0.0;
        const auto ratio = (value - base) / route.write.step;
        if (std::fabs(ratio - std::round(ratio)) > 1e-7) {
            return "value does not match step";
        }
    }
    if (!route.write.allowedValues.empty()) {
        const auto match = std::find_if(
            route.write.allowedValues.begin(),
            route.write.allowedValues.end(),
            [value](double candidate) { return std::fabs(candidate - value) <= 1e-9; }
        );
        if (match == route.write.allowedValues.end()) {
            return "value is not in allowedValues";
        }
    }
    return {};
}

PointValue makeEmsVirtualValue(
    const PointStoreRoute& route,
    double value,
    std::int64_t timestamp,
    const std::string& qualityMessage
) {
    PointValue point;
    point.index = route.index;
    point.machineCode = route.machineCode;
    point.meterCode = route.meterCode;
    point.pointCode = route.pointCode;
    point.category = route.category.empty() ? std::string("setting") : route.category;
    point.value = value;
    point.quality = 1;
    point.qualityMsg = qualityMessage;
    point.ts = timestamp;
    // Persisted settings remain valid until explicitly changed. Treating them
    // like telemetry would make an unchanged EMS parameter stale after its TTL.
    point.expireAt = 0;
    point.stale = false;
    point.isStore = route.isStore;
    point.persistIntervalSec = route.persistIntervalSec;
    return point;
}

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

PointStoreRouter::PointStoreRouter()
    : emsVirtualParameterDirectory_(kDefaultEmsVirtualParameterDirectory) {
    const auto* configured = std::getenv("EDGE_GATEWAY_EMS_VIRTUAL_PARAMETER_DIR");
    if (configured != nullptr && *configured != '\0') {
        emsVirtualParameterDirectory_ = configured;
    }
}

void PointStoreRouter::addStore(const std::string& sharedMemoryName, MemoryPointStore& store) {
    if (sharedMemoryName.empty()) {
        throw std::invalid_argument("sharedMemoryName is required");
    }
    stores_[sharedMemoryName] = &store;
    for (const auto& entry : routesByLocation_) {
        if (entry.second.sharedMemoryName == sharedMemoryName) {
            restoreEmsVirtualParameter(entry.second);
        }
    }
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
                    route.category = point.category;
                    route.protocolType = config.protocol.type;
                    route.interfaceCode = interfaceCode;
                    route.interfaceType = interfaceType;
                    route.sharedMemoryName = sharedMemoryName;
                    route.writable = point.write.enable;
                    route.commandMailbox = config.protocol.type == "agc_avc_virtual" && point.category == "command";
                    route.fullUpload = point.fullUpload;
                    route.reportOnChange = point.reportOnChange;
                    route.isStore = point.isStore;
                    route.persistIntervalSec = point.persistIntervalSec;
                    route.ttlMs = point.read.cachePolicy.ttlMs;
                    route.initialValue = point.initialValue;
                    route.retain = point.retain;
                    route.write = point.write;
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
            route.category = point.category;
            route.protocolType = config.protocol.type;
            route.interfaceCode = interfaceCode;
            route.interfaceType = interfaceType;
            route.sharedMemoryName = sharedMemoryName;
            route.writable = point.write.enable;
            route.commandMailbox = config.protocol.type == "agc_avc_virtual" && point.category == "command";
            route.fullUpload = point.fullUpload;
            route.reportOnChange = point.reportOnChange;
            route.isStore = point.isStore;
            route.persistIntervalSec = point.persistIntervalSec;
            route.ttlMs = point.read.cachePolicy.ttlMs;
            route.initialValue = point.initialValue;
            route.retain = point.retain;
            route.write = point.write;
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
        route.protocolType = "camera";
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

Optional<PointStoreRoute> PointStoreRouter::routeByLocation(
    const std::string& sharedMemoryName,
    std::uint32_t index
) const {
    if (sharedMemoryName.empty()) {
        return routeByIndex(index);
    }
    const auto it = routesByLocation_.find(routeLocationKey(sharedMemoryName, index));
    return it == routesByLocation_.end()
        ? Optional<PointStoreRoute>(NullOpt)
        : Optional<PointStoreRoute>(it->second);
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

Optional<StoredPointValue> PointStoreRouter::getLatestByLocation(
    const std::string& sharedMemoryName,
    std::uint32_t index,
    std::int64_t nowMs
) const {
    const auto route = routeByLocation(sharedMemoryName, index);
    if (!route) {
        return NullOpt;
    }
    return route->derived
        ? getDerivedLatestByRoute(*route, nowMs)
        : getRawLatestByRoute(*route, nowMs);
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
    const auto route = routeByIndex(command.index);
    if (!route) {
        CommandSubmitResult result;
        result.message = "command index not found";
        return result;
    }
    return submitWriteCommand(*route, command);
}

CommandSubmitResult PointStoreRouter::submitWriteCommand(
    const PointStoreRoute& route,
    const PendingWriteCommand& command
) {
    CommandSubmitResult result;
    result.route = route;
    const auto commandTime = command.ts > 0 ? command.ts : command.acceptedAt;
    if (!command.highPriority && powerControlOwnership_ &&
        powerControlOwnership_->isBlocked(command.index, command.source, commandTime)) {
        result.message = "power control target is owned by another controller";
        return result;
    }
    if (!route.writable) {
        result.message = "point write is disabled";
        return result;
    }
    auto* store = storeForRoute(route);
    if (store == nullptr) {
        result.message = "target shared memory not found: " + route.sharedMemoryName;
        return result;
    }
    try {
        if (isDirectEmsVirtualParameter(route)) {
            const auto validationError = validateEmsVirtualParameter(route, command.value);
            if (!validationError.empty()) {
                result.message = validationError;
                return result;
            }
            const auto startedAt = currentTimeMs();
            persistParameterAtomically(
                emsVirtualParameterDirectory_,
                route.index,
                command.value,
                emsVirtualPersistedValues_
            );
            store->putLatest(makeEmsVirtualValue(
                route,
                command.value,
                startedAt,
                "ems-virtual-parameter"
            ));
            const auto completedAt = currentTimeMs();

            WritebackResultRecord writeback;
            writeback.cmdId = command.cmdId;
            writeback.index = command.index;
            writeback.value = command.value;
            writeback.success = true;
            writeback.message = "EMS virtual parameter committed";
            writeback.stage = "local-parameter-committed";
            writeback.requestedAt = command.ts > 0 ? command.ts : startedAt;
            writeback.acceptedAt = command.acceptedAt > 0 ? command.acceptedAt : startedAt;
            writeback.startedAt = startedAt;
            writeback.completedAt = completedAt;
            writeback.queueDelayMs = std::max<std::int64_t>(0, startedAt - writeback.acceptedAt);
            writeback.deviceWriteMs = std::max<std::int64_t>(0, completedAt - startedAt);
            writeback.edgeElapsedMs = std::max<std::int64_t>(0, completedAt - writeback.acceptedAt);
            writeback.totalElapsedMs = writeback.edgeElapsedMs;
            writeback.highPriority = command.highPriority;
            store->recordWritebackResult(writeback);

            result.accepted = true;
            result.message = writeback.message;
            return result;
        }
        store->submitWriteCommand(command);
    } catch (const std::exception& ex) {
        result.message = ex.what();
        return result;
    }
    result.accepted = true;
    result.message = "write command routed";
    return result;
}

CommandGroupSubmitResult PointStoreRouter::submitWriteCommands(
    const std::vector<PendingWriteCommand>& commands
) {
    CommandGroupSubmitResult result;
    if (commands.empty()) {
        result.message = "write command group is empty";
        return result;
    }

    MemoryPointStore* targetStore = nullptr;
    std::string targetSharedMemoryName;
    std::unordered_set<std::uint32_t> indexes;
    result.routes.reserve(commands.size());
    for (const auto& command : commands) {
        if (!std::isfinite(command.value)) {
            result.message = "write command value must be finite";
            return result;
        }
        if (!indexes.insert(command.index).second) {
            result.message = "write command group contains duplicate index";
            return result;
        }
        const auto route = routeByIndex(command.index);
        if (!route) {
            result.message = "command index not found: " + std::to_string(command.index);
            return result;
        }
        const auto commandTime = command.ts > 0 ? command.ts : command.acceptedAt;
        if (!command.highPriority && powerControlOwnership_ &&
            powerControlOwnership_->isBlocked(command.index, command.source, commandTime)) {
            result.message = "power control target is owned by another controller";
            return result;
        }
        if (!route->writable) {
            result.message = "point write is disabled: " + std::to_string(command.index);
            return result;
        }
        if (isDirectEmsVirtualParameter(*route)) {
            result.message = "atomic device write group does not support EMS virtual parameters";
            return result;
        }
        auto* store = storeForRoute(*route);
        if (store == nullptr) {
            result.message = "target shared memory not found: " + route->sharedMemoryName;
            return result;
        }
        if (targetStore != nullptr &&
            (store != targetStore || route->sharedMemoryName != targetSharedMemoryName)) {
            result.message = "write command group must use one shared memory route";
            return result;
        }
        targetStore = store;
        targetSharedMemoryName = route->sharedMemoryName;
        result.routes.push_back(*route);
    }

    try {
        targetStore->submitWriteCommands(commands);
    } catch (const std::exception& ex) {
        result.message = ex.what();
        return result;
    }
    result.accepted = true;
    result.message = "write command group routed";
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
    if (isRetainedEmsVirtualPoint(*route)) {
        value.expireAt = 0;
        value.stale = false;
        try {
            persistParameterAtomically(
                emsVirtualParameterDirectory_,
                route->index,
                value.value,
                emsVirtualPersistedValues_
            );
        } catch (const std::exception& ex) {
            logEmsVirtualPersistenceErrorOnce("update", route->index, ex);
        }
    }
    store->putLatest(value);
    result.accepted = true;
    result.message = "latest value routed";
    return result;
}

CommandSubmitResult PointStoreRouter::putLatestByLocation(
    const std::string& sharedMemoryName,
    PointValue value
) {
    CommandSubmitResult result;
    const auto route = routeByLocation(sharedMemoryName, value.index);
    if (!route) {
        result.message = "latest point location not found";
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
    if (isRetainedEmsVirtualPoint(*route)) {
        value.expireAt = 0;
        value.stale = false;
        try {
            persistParameterAtomically(
                emsVirtualParameterDirectory_,
                route->index,
                value.value,
                emsVirtualPersistedValues_
            );
        } catch (const std::exception& ex) {
            logEmsVirtualPersistenceErrorOnce("update", route->index, ex);
        }
    }
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

Optional<WritebackResultRecord> PointStoreRouter::getWritebackResult(
    const PointStoreRoute& route,
    const std::string& cmdId,
    std::uint32_t index
) const {
    auto* store = storeForRoute(route);
    if (store == nullptr) {
        return NullOpt;
    }
    try {
        return store->getWritebackResult(cmdId, index);
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
    const auto location = routesByLocation_.emplace(
        routeLocationKey(route.sharedMemoryName, route.index),
        route
    );
    if (!location.second) {
        const auto& existing = location.first->second;
        throw std::invalid_argument(
            "duplicate point route location=" + route.sharedMemoryName +
            " index=" + std::to_string(route.index) +
            " existing=" + existing.machineCode + "/" + existing.meterCode + "/" + existing.pointCode +
            " incoming=" + route.machineCode + "/" + route.meterCode + "/" + route.pointCode
        );
    }
    routes_.emplace(route.index, route);
    restoreEmsVirtualParameter(location.first->second);
}

void PointStoreRouter::setPowerControlOwnershipFile(const std::string& path, const std::string& owner) {
    powerControlOwnership_.reset(path.empty() ? nullptr : new PowerControlOwnership(path, owner));
}

void PointStoreRouter::setEmsVirtualParameterDirectory(const std::string& directory) {
    emsVirtualParameterDirectory_ = directory.empty()
        ? std::string(kDefaultEmsVirtualParameterDirectory)
        : directory;
    emsVirtualPersistedValues_.clear();
    for (const auto& entry : routesByLocation_) {
        restoreEmsVirtualParameter(entry.second);
    }
}

void PointStoreRouter::restoreEmsVirtualParameter(const PointStoreRoute& route) {
    const auto directParameter = isDirectEmsVirtualParameter(route);
    const auto retainedPoint = isRetainedEmsVirtualPoint(route);
    if (!isEmsVirtualPoint(route) ||
        (!directParameter && !retainedPoint && !route.initialValue && !route.write.startupValue)) {
        return;
    }
    auto* store = storeForRoute(route);
    if (store == nullptr) {
        return;
    }
    Optional<double> persisted;
    if (directParameter || retainedPoint) {
        try {
            persisted = loadPersistedParameter(
                emsVirtualParameterDirectory_,
                route.index,
                emsVirtualPersistedValues_
            );
        } catch (const std::exception& ex) {
            logEmsVirtualPersistenceErrorOnce("load", route.index, ex);
        }
    }

    try {
        const auto initialValue = persisted
            ? persisted
            : (route.initialValue ? route.initialValue : route.write.startupValue);
        const auto timestamp = currentTimeMs();
        const auto current = store->getLatestByIndex(route.index, timestamp);
        if (current && current->quality == 1 && !current->stale) {
            if ((directParameter || retainedPoint) &&
                (!persisted || std::fabs(*persisted - current->value) > 1e-12)) {
                try {
                    persistParameterAtomically(
                        emsVirtualParameterDirectory_,
                        route.index,
                        current->value,
                        emsVirtualPersistedValues_
                    );
                } catch (const std::exception& ex) {
                    logEmsVirtualPersistenceErrorOnce("restore-current", route.index, ex);
                }
            }
            return;
        }
        if (!initialValue) {
            return;
        }
        const auto validationError = validateEmsVirtualParameter(route, *initialValue);
        if (!validationError.empty()) {
            std::cerr << "point store router ignored EMS virtual parameter initial value"
                      << " index=" << route.index
                      << " reason=" << validationError
                      << std::endl;
            return;
        }
        if ((directParameter || retainedPoint) && !persisted) {
            try {
                persistParameterAtomically(
                    emsVirtualParameterDirectory_,
                    route.index,
                    *initialValue,
                    emsVirtualPersistedValues_
                );
            } catch (const std::exception& ex) {
                logEmsVirtualPersistenceErrorOnce("initialize", route.index, ex);
            }
        }
        auto restored = makeEmsVirtualValue(
            route,
            *initialValue,
            timestamp,
            persisted ? "ems-virtual-retained" : "ems-virtual-initial"
        );
        restored.isStore = false;
        store->putLatest(restored);
    } catch (const std::exception& ex) {
        std::cerr << "point store router failed to restore EMS virtual parameter"
                  << " index=" << route.index
                  << " error=" << ex.what()
                  << std::endl;
    }
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
    for (const auto& entry : routesByLocation_) {
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
    if (value->machineCode.empty()) value->machineCode = route.machineCode;
    if (value->meterCode.empty()) value->meterCode = route.meterCode;
    if (value->pointCode.empty()) value->pointCode = route.pointCode;
    return *value;
}

Optional<StoredPointValue> PointStoreRouter::getDerivedLatestByRoute(
    const PointStoreRoute& route,
    std::int64_t nowMs
) const {
    const auto sourceRoute = routeByLocation(route.sharedMemoryName, route.sourceIndex);
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
