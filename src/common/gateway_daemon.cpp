#include "edge_gateway/gateway_daemon.hpp"
#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/control_dedup_store.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/common/persistent_flush_schedule.hpp"
#include "edge_gateway/writeback_service.hpp"

namespace edge_gateway {

namespace {

constexpr int kStoreHeartbeatIntervalMs = 5000;
constexpr int kStoreExpirySweepIntervalMs = 5000;
// Upper bound for one idle sleep of the collect loop, so lease changes and due work are noticed
// even when the configured interval is long.
constexpr int kMaxCollectSleepMs = 1000;

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const auto ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

// Wall clock: timestamps only.
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Monotonic clock: loop deadlines and sleeps, immune to NTP or manual clock steps.
std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const auto slice = std::min(100, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

std::string readSmallTextFile(const std::string& path, std::size_t maxBytes) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        return std::string();
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > maxBytes) {
        return std::string();
    }
    input.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

std::int64_t extractInt64Field(const std::string& text, const char* key, std::int64_t fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    pos = skipWhitespace(text, pos + needle.size());
    if (pos >= text.size() || text[pos] != ':') {
        return fallback;
    }
    pos = skipWhitespace(text, pos + 1);
    const auto begin = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
        ++pos;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (begin == pos) {
        return fallback;
    }
    try {
        return std::stoll(text.substr(begin, pos - begin));
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> extractStringArrayField(const std::string& text, const char* key) {
    std::vector<std::string> result;
    const std::string needle = std::string("\"") + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) {
        return result;
    }
    pos = skipWhitespace(text, pos + needle.size());
    if (pos >= text.size() || text[pos] != ':') {
        return result;
    }
    pos = skipWhitespace(text, pos + 1);
    if (pos >= text.size() || text[pos] != '[') {
        return result;
    }
    ++pos;
    while (pos < text.size()) {
        pos = skipWhitespace(text, pos);
        if (pos >= text.size() || text[pos] == ']') {
            break;
        }
        if (text[pos] != '"') {
            break;
        }
        ++pos;
        std::string value;
        while (pos < text.size()) {
            const char ch = text[pos++];
            if (ch == '"') {
                result.push_back(value);
                break;
            }
            if (ch == '\\' && pos < text.size()) {
                value.push_back(text[pos++]);
            } else {
                value.push_back(ch);
            }
        }
        pos = skipWhitespace(text, pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
        }
    }
    return result;
}

std::vector<DeviceConfig> expandRuntimeConfigs(const DeviceConfig& config) {
    if (config.meters.empty()) {
        return {config};
    }

    std::vector<PointDefinition> dlt645StandardPoints;
    if (config.protocol.type == "dlt645_2007" && !config.protocol.standardPointsFile.empty()) {
        dlt645StandardPoints = ConfigLoader::loadDlt645StandardPointsFromFile(config.protocol.standardPointsFile);
    }

    std::vector<DeviceConfig> expanded;
    expanded.reserve(config.meters.size());
    std::size_t meterIndex = 0;
    for (const auto& logicalDevice : config.meters) {
        if (!logicalDevice.enabled) {
            ++meterIndex;
            continue;
        }
        DeviceConfig item = config;
        item.meterCode = logicalDevice.meterCode;
        item.deviceName = logicalDevice.deviceName;
        item.address = logicalDevice.address;
        item.protocol.slave = logicalDevice.slave;
        item.points = logicalDevice.points.empty() ? dlt645StandardPoints : logicalDevice.points;
        if (config.protocol.type == "dlt645_2007" && logicalDevice.points.empty()) {
            const std::uint32_t indexBase = 200000U + static_cast<std::uint32_t>(meterIndex) * 10000U;
            for (std::size_t i = 0; i < item.points.size(); ++i) {
                item.points[i].index = indexBase + static_cast<std::uint32_t>(i);
            }
        }
        item.meters.clear();
        expanded.push_back(item);
        ++meterIndex;
    }
    return expanded;
}

}  // namespace

int resolveCollectLoopIntervalMs(const DeviceConfig& config) {
    int intervalMs = std::numeric_limits<int>::max();
    const auto inspectPoints = [&](const std::vector<PointDefinition>& points) {
        for (const auto& point : points) {
            if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
                continue;
            }
            const auto pointIntervalMs = point.read.intervalMs > 0
                ? point.read.intervalMs
                : config.collect.defaultIntervalMs;
            intervalMs = std::min(intervalMs, std::max(1, pointIntervalMs));
        }
    };
    inspectPoints(config.points);
    for (const auto& meter : config.meters) {
        if (meter.enabled) inspectPoints(meter.points);
    }
    return intervalMs == std::numeric_limits<int>::max()
        ? std::max(1, config.collect.defaultIntervalMs)
        : intervalMs;
}

GatewayDaemon::GatewayDaemon(
    DeviceConfig config,
    MemoryPointStore& store,
    CollectorFactory collectorFactory,
    CommandExecutorFactory commandExecutorFactory,
    ServiceStartStop auxiliaryService,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::string realtimeMeterLeaseFile
) : config_(std::move(config)),
    store_(store),
    realtimeMeterLeaseFile_(std::move(realtimeMeterLeaseFile)),
    priorityControlLease_(
        config_.mqttDriver.priorityControlLeaseFile,
        config_.protocol.type + ":" + config_.memoryStore.sharedMemoryName
    ),
    powerControlOwnership_(
        config_.mqttDriver.powerControlOwnershipFile,
        "mqtt-forwarder"
    ),
    sqliteWriter_(config_.memoryStore.sqlitePath, config_.memoryStore.sqliteLibraryPath,
                  config_.memoryStore.historyRetentionDays),
    mqttPublisher_(std::move(mqttPublisher)),
    collectorFactory_(std::move(collectorFactory)),
    commandExecutorFactory_(std::move(commandExecutorFactory)),
    auxiliaryService_(std::move(auxiliaryService)) {
    if (!collectorFactory_) {
        throw std::invalid_argument("collectorFactory is required");
    }
    if (!commandExecutorFactory_) {
        throw std::invalid_argument("commandExecutorFactory is required");
    }
    initializeRuntimeDevices();
}

GatewayDaemon::~GatewayDaemon() {
    stop();
}

void GatewayDaemon::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    publishStatusEvent(
        "started",
        nowMs(),
        std::string(R"("sharedMemory":")") + config_.memoryStore.sharedMemoryName +
            R"(","mode":")" + config_.protocol.type + R"(")"
    );

    if (auxiliaryService_) {
        auxiliaryService_(true);
    }
    collectThread_ = std::thread(&GatewayDaemon::collectLoop, this);
    persistThread_ = std::thread(&GatewayDaemon::persistLoop, this);
    writebackThread_ = std::thread(&GatewayDaemon::writebackLoop, this);
}

void GatewayDaemon::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    if (collectThread_.joinable()) {
        collectThread_.join();
    }
    if (persistThread_.joinable()) {
        persistThread_.join();
    }
    if (writebackThread_.joinable()) {
        writebackThread_.join();
    }
    if (auxiliaryService_) {
        auxiliaryService_(false);
    }
}

bool GatewayDaemon::isRunning() const {
    return running_.load();
}

void GatewayDaemon::collectOnce(std::int64_t nowMsValue) {
    collectionWorkDeferred_ = false;
    if (roundVisited_ == 0) {
        collectionNextDelayMs_ = 0;
    }
    if (runtimeDevices_.empty()) {
        return;
    }
    if (priorityControlBlocked(nowMsValue)) {
        return;
    }
    const auto batchSize = collectRuntimeMeterBatchSize();
    std::set<std::size_t> collectedIndexes;
    const auto collectDevice = [&](std::size_t index, bool realtimeFocused) {
        if (index >= runtimeDevices_.size()) {
            return;
        }
        if (!collectedIndexes.insert(index).second) {
            return;
        }
        auto& runtimeDevice = runtimeDevices_[index];
        try {
            const auto result = runtimeDevice.collector->collectOnce(nowMsValue, realtimeFocused);
            collectionWorkDeferred_ = collectionWorkDeferred_ || result.workDeferred;
            if (result.nextDelayMs > 0) {
                collectionNextDelayMs_ = collectionNextDelayMs_ > 0
                    ? std::min(collectionNextDelayMs_, result.nextDelayMs) : result.nextDelayMs;
            }
        } catch (const std::exception& ex) {
            // The collector applies its own failure thresholds; one failed cycle is not offline.
            runtimeDevice.collector->publishDeviceOnlineStatus(runtimeDevice.collector->deviceOnline(), nowMsValue);
            std::cerr << "collect failed"
                      << " device=" << runtimeDevice.config.meterCode
                      << " slave=" << runtimeDevice.config.protocol.slave
                      << " error=" << ex.what()
                      << std::endl;
            publishStatusEvent(
                "collect-failed",
                nowMsValue,
                std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                    R"(","slave":)" + std::to_string(runtimeDevice.config.protocol.slave) +
                    R"(,"message":")" + escapeJson(ex.what()) + R"(")"
            );
        }
        flushImmediatePersistentSamples();
    };

    const auto activeIndexes = activeRealtimeDeviceIndexes(nowMsValue);
    for (const auto index : activeIndexes) {
        collectDevice(index, true);
    }
    if (!activeIndexes.empty()) {
        roundVisited_ = 0;
        std::size_t attempts = 0;
        while (attempts < runtimeDevices_.size()) {
            const auto index = collectCursor_ % runtimeDevices_.size();
            collectCursor_ = (collectCursor_ + 1) % runtimeDevices_.size();
            ++attempts;
            if (collectedIndexes.find(index) != collectedIndexes.end()) {
                continue;
            }
            collectDevice(index, false);
            break;
        }
        return;
    }

    const auto targetCount = batchSize;
    std::size_t attempts = 0;
    while (attempts < runtimeDevices_.size() && collectedIndexes.size() < targetCount) {
        const auto index = collectCursor_ % runtimeDevices_.size();
        collectCursor_ = (collectCursor_ + 1) % runtimeDevices_.size();
        ++attempts;
        if (collectedIndexes.find(index) != collectedIndexes.end()) {
            continue;
        }
        collectDevice(index, false);
    }
    // A batch covers only part of the meters: finish the round before sleeping, otherwise meters
    // outside this batch would wait a whole loop interval although the bus is idle.
    roundVisited_ += collectedIndexes.size();
    if (roundVisited_ < runtimeDevices_.size()) {
        collectionWorkDeferred_ = true;
    } else {
        roundVisited_ = 0;
    }
}

std::size_t GatewayDaemon::flushPersistentOnce() {
    std::lock_guard<std::mutex> lock(persistentFlushMutex_);
    const auto immediateGeneration = store_.immediatePersistentGeneration();
    // Bounded batches: an all-or-nothing flush of a full ring can exceed the storage
    // admission estimate forever, and a partial success must still be acknowledged.
    const auto batchSize = std::max<std::size_t>(1, config_.memoryStore.persistFlushBatchSize);
    std::size_t flushed = 0;
    for (std::size_t batch = 0; batch < 64; ++batch) {
        const auto samples = store_.peekPersistentSamples(batchSize);
        if (samples.empty()) {
            break;
        }
        sqliteWriter_.writeSamples(samples);
        store_.acknowledgePersistentSamples(samples.back().sequence);
        flushed += samples.size();
        if (samples.size() < batchSize) {
            break;
        }
    }
    if (flushed > 0) {
        publishStatusEvent(
            "persist-flushed",
            nowMs(),
            std::string(R"("count":)") + std::to_string(flushed)
        );
    }
    const auto dropped = store_.consumePersistentDropCount();
    if (dropped > 0) {
        std::cerr << "persistent samples dropped"
                  << " count=" << dropped
                  << " sharedMemory=" << config_.memoryStore.sharedMemoryName
                  << std::endl;
        publishStatusEvent(
            "persist-dropped",
            nowMs(),
            std::string(R"("count":)") + std::to_string(dropped)
        );
    }
    flushedImmediatePersistentGeneration_.store(immediateGeneration, std::memory_order_release);
    return flushed;
}

void GatewayDaemon::flushImmediatePersistentSamples() {
    const auto requested = store_.immediatePersistentGeneration();
    if (requested == flushedImmediatePersistentGeneration_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        flushPersistentOnce();
    } catch (const std::exception& ex) {
        std::cerr << "immediate persistent flush failed"
                  << " sharedMemory=" << config_.memoryStore.sharedMemoryName
                  << " error=" << ex.what()
                  << std::endl;
    }
}

std::size_t GatewayDaemon::processWritebackOnce(std::int64_t nowMsValue) {
    const auto entryTime = std::chrono::steady_clock::now();
    const auto commands = drainScheduledWriteCommands(
        store_,
        &priorityControlLease_,
        nowMsValue,
        config_.memoryStore.writebackBatchSize
    );
    std::size_t processed = 0;
    const auto maxAgeMs = config_.memoryStore.pendingWriteMaxAgeMs;
    const auto currentTime = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - entryTime).count();
        return nowMsValue > std::numeric_limits<std::int64_t>::max() - elapsed
            ? std::numeric_limits<std::int64_t>::max() : nowMsValue + elapsed;
    };
    for (const auto& command : commands) {
        const auto startedAt = nowMs();
        const auto commandNowMs = currentTime();
        auto writebackResult = beginWritebackResult(command, startedAt);
        if (isPendingWriteExpired(command, commandNowMs, maxAgeMs)) {
            // Never execute a stale setpoint late (e.g. queued while this daemon was down).
            const auto message = "pending write expired before execution (memoryStore.pendingWriteMaxAgeMs=" +
                std::to_string(maxAgeMs) + ")";
            const auto expire = [&] {
                auto receipt = beginWritebackResult(command, startedAt);
                completeWritebackResult(receipt, false, message, "expired", nowMs());
                return receipt;
            };
            const auto device = indexToRuntimeDevice_.find(command.index);
            if (command.durableControl && device != indexToRuntimeDevice_.end()) {
                try {
                    ControlDedupStore dedup(config_.memoryStore.controlDedupPath);
                    const auto& runtimeDevice = runtimeDevices_[device->second];
                    writebackResult = dedup.dispatch(runtimeDevice.config.machineCode,
                        runtimeDevice.config.meterCode, command, startedAt, expire);
                } catch (const std::exception& ex) {
                    completeWritebackResult(writebackResult, false, ex.what(), "expired", nowMs());
                }
            } else {
                writebackResult = expire();
            }
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(
                "writeback-expired",
                commandNowMs,
                std::string(R"("index":)") + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")"
            );
            if (command.highPriority) {
                priorityControlLease_.release(command.cmdId);
            }
            continue;
        }
        const auto authorization = powerControlOwnership_.authorize(
            command.index,
            command.source,
            command.controlGeneration,
            command.highPriority,
            commandNowMs
        );
        if (!authorization.allowed) {
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                false,
                authorization.message,
                "control-rejected",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(
                "control-rejected",
                commandNowMs,
                std::string(R"("index":)") + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) +
                    R"(","message":")" + escapeJson(authorization.message) + R"(")"
            );
            if (command.highPriority) {
                priorityControlLease_.release(command.cmdId);
            }
            continue;
        }
        const auto it = indexToRuntimeDevice_.find(command.index);
        if (it == indexToRuntimeDevice_.end()) {
            std::cerr << "writeback skipped index=" << command.index << " reason=unknown-index" << std::endl;
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                false,
                "unknown-index",
                "writeback-skipped",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(
                "writeback-skipped",
                commandNowMs,
                std::string(R"("index":)") + std::to_string(command.index) +
                    R"(,"reason":"unknown-index")"
            );
            if (command.highPriority) {
                priorityControlLease_.release(command.cmdId);
            }
            continue;
        }
        bool expiredBeforeSend = false;
        const auto executePending = [&] {
            BeforePhysicalWrite beforeWrite;
            const auto modePermit = EmsControlMode(config_.memoryStore.emsControlModeFile).dispatchPermit(command);
            const bool clusterGuard = clusterAuthorizationRequired(config_.emsCluster, command);
            if (clusterGuard || (maxAgeMs > 0 &&
                (config_.protocol.type == "modbus_rtu" || config_.protocol.type == "modbus_tcp"))) {
                auto guard = clusterGuard ? std::make_shared<ClusterWriteGuard>(config_.emsCluster) : nullptr;
                beforeWrite = [&, guard] {
                    if (guard) guard->check(command);
                    if (isPendingWriteExpired(command, currentTime(), maxAgeMs)) {
                        expiredBeforeSend = true;
                        throw std::runtime_error("pending write expired before physical send");
                    }
                };
            }
            return runtimeDevices_[it->second].executor->executePending(command, commandNowMs, beforeWrite);
        };
        if (command.durableControl) {
            const auto& device = runtimeDevices_[it->second];
            try {
                ControlDedupStore dedup(config_.memoryStore.controlDedupPath);
                writebackResult = dedup.dispatch(device.config.machineCode, device.config.meterCode,
                    command, startedAt, [&] {
                        const auto result = executePending();
                        auto receipt = beginWritebackResult(command, startedAt);
                        completeWritebackResult(receipt, result.success, result.message,
                            result.success ? "writeback-succeeded"
                                : result.outcomeUnknown ? "writeback-timeout" : "writeback-failed",
                            nowMs(), result.verifyAttempted, result.verifyPassed);
                        return receipt;
                    });
            } catch (const std::exception& ex) {
                completeWritebackResult(writebackResult, false, ex.what(), "writeback-failed", nowMs());
            }
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(writebackResult.stage, commandNowMs,
                std::string(R"("index":)") + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")");
            if (command.highPriority) priorityControlLease_.release(command.cmdId);
            if (writebackResult.success) ++processed;
            continue;
        }
        try {
            const auto result = executePending();
            const auto& runtimeDevice = runtimeDevices_[it->second];
            if (!result.success) {
                const auto completedAt = nowMs();
                // writeback-timeout: the request reached the device but no conclusive reply came back
                // (CommandResult.outcomeUnknown); the command must not be retried under a new cmdId.
                const auto stage = expiredBeforeSend ? "expired"
                    : result.outcomeUnknown ? "writeback-timeout" : "writeback-failed";
                completeWritebackResult(
                    writebackResult,
                    false,
                    result.message,
                    stage,
                    completedAt,
                    result.verifyAttempted,
                    result.verifyPassed
                );
                store_.recordWritebackResult(writebackResult);
                std::cerr << "writeback failed"
                          << " device=" << runtimeDevice.config.meterCode
                          << " slave=" << runtimeDevice.config.protocol.slave
                          << " index=" << command.index
                          << " error=" << result.message
                          << std::endl;
                publishStatusEvent(
                    stage,
                    commandNowMs,
                    std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                        R"(","index":)" + std::to_string(command.index) +
                        R"(,"message":")" + escapeJson(result.message) + R"(")"
                );
                if (command.highPriority) {
                    priorityControlLease_.release(command.cmdId);
                }
                continue;
            }
            ++processed;
            if (command.highPriority) {
                priorityControlLease_.release(command.cmdId);
            }
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                true,
                result.message.empty() ? "ok" : result.message,
                "writeback-succeeded",
                completedAt,
                result.verifyAttempted,
                result.verifyPassed
            );
            store_.recordWritebackResult(writebackResult);
            publishStatusEvent(
                "writeback-succeeded",
                commandNowMs,
                std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) +
                    R"(","queueDelayMs":)" + std::to_string(writebackResult.queueDelayMs) +
                    R"(,"deviceWriteMs":)" + std::to_string(writebackResult.deviceWriteMs) +
                    R"(,"totalElapsedMs":)" + std::to_string(writebackResult.totalElapsedMs)
            );
        } catch (const std::exception& ex) {
            const auto& runtimeDevice = runtimeDevices_[it->second];
            const auto completedAt = nowMs();
            completeWritebackResult(
                writebackResult,
                false,
                ex.what(),
                "writeback-failed",
                completedAt
            );
            store_.recordWritebackResult(writebackResult);
            std::cerr << "writeback failed"
                      << " device=" << runtimeDevice.config.meterCode
                      << " slave=" << runtimeDevice.config.protocol.slave
                      << " index=" << command.index
                      << " error=" << ex.what()
                      << std::endl;
            publishStatusEvent(
                "writeback-failed",
                commandNowMs,
                std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"message":")" + escapeJson(ex.what()) + R"(")"
            );
            if (command.highPriority) {
                priorityControlLease_.release(command.cmdId);
            }
        }
    }
    flushImmediatePersistentSamples();
    return processed;
}

void GatewayDaemon::collectLoop() {
    const auto intervalMs = collectLoopIntervalMs();
    auto nextDeadline = steadyMs() + intervalMs;
    std::int64_t lastStoreHeartbeatMs = 0;
    std::int64_t lastExpirySweepMs = 0;
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            const auto tick = steadyMs();
            if (lastStoreHeartbeatMs <= 0 || tick - lastStoreHeartbeatMs >= kStoreHeartbeatIntervalMs) {
                store_.heartbeatRegisteredPoints(ts);
                lastStoreHeartbeatMs = tick;
            }
            if (!priorityControlBlocked(ts)) {
                collectOnce(ts);
                if (lastExpirySweepMs <= 0 || tick - lastExpirySweepMs >= kStoreExpirySweepIntervalMs) {
                    store_.removeExpired(ts);
                    lastExpirySweepMs = tick;
                }
            } else {
                collectionWorkDeferred_ = false;
            }
        } catch (...) {
            collectionWorkDeferred_ = false;
        }
        if (collectionWorkDeferred_) {
            sleepInterruptibly(running_, 1);
            nextDeadline = steadyMs() + intervalMs;
            continue;
        }
        const auto afterCollection = steadyMs();
        auto remaining = static_cast<int>(std::max<std::int64_t>(0, nextDeadline - afterCollection));
        if (collectionNextDelayMs_ > 0 && collectionNextDelayMs_ < remaining) {
            remaining = collectionNextDelayMs_;
        }
        sleepInterruptibly(running_, std::min(kMaxCollectSleepMs, std::max(1, remaining)));
        const auto now = steadyMs();
        if (now >= nextDeadline) {
            nextDeadline += intervalMs;
            if (nextDeadline < now) {
                nextDeadline = now + intervalMs;
            }
        }
    }
}

int GatewayDaemon::collectLoopIntervalMs() const {
    return resolveCollectLoopIntervalMs(config_);
}

std::size_t GatewayDaemon::collectRuntimeMeterBatchSize() const {
    if (runtimeDevices_.empty()) {
        return 0;
    }
    return std::min<std::size_t>(
        runtimeDevices_.size(),
        static_cast<std::size_t>(std::max(1, config_.collect.runtimeMeterBatchSize))
    );
}

bool GatewayDaemon::priorityControlBlocked(std::int64_t nowMsValue) const {
    return priorityControlLease_.isBlocked(nowMsValue);
}

std::vector<std::string> GatewayDaemon::activeRealtimeMeterCodes(std::int64_t nowMsValue) {
    if (realtimeMeterLeaseFile_.empty()) {
        return {};
    }
    if (realtimeLeaseLastReadMs_ > 0 && nowMsValue - realtimeLeaseLastReadMs_ < 200) {
        return realtimeLeaseExpireAtMs_ > nowMsValue ? realtimeLeaseMeterCodes_ : std::vector<std::string>();
    }
    realtimeLeaseLastReadMs_ = nowMsValue;
    const auto text = readSmallTextFile(realtimeMeterLeaseFile_, 64 * 1024);
    if (text.empty()) {
        realtimeLeaseExpireAtMs_ = 0;
        realtimeLeaseMeterCodes_.clear();
        return {};
    }
    const auto expireAtMs = extractInt64Field(text, "expireAtMs", 0);
    auto meterCodes = extractStringArrayField(text, "meterCodes");
    std::sort(meterCodes.begin(), meterCodes.end());
    meterCodes.erase(std::unique(meterCodes.begin(), meterCodes.end()), meterCodes.end());
    if (expireAtMs <= nowMsValue || meterCodes.empty()) {
        realtimeLeaseExpireAtMs_ = expireAtMs;
        realtimeLeaseMeterCodes_.clear();
        return {};
    }
    realtimeLeaseExpireAtMs_ = expireAtMs;
    realtimeLeaseMeterCodes_ = std::move(meterCodes);
    return realtimeLeaseMeterCodes_;
}

std::vector<std::size_t> GatewayDaemon::activeRealtimeDeviceIndexes(std::int64_t nowMsValue) {
    std::vector<std::size_t> indexes;
    for (const auto& meterCode : activeRealtimeMeterCodes(nowMsValue)) {
        const auto it = meterCodeToRuntimeDevice_.find(meterCode);
        if (it == meterCodeToRuntimeDevice_.end()) {
            continue;
        }
        if (std::find(indexes.begin(), indexes.end(), it->second) == indexes.end()) {
            indexes.push_back(it->second);
        }
    }
    return indexes;
}

void GatewayDaemon::persistLoop() {
    PersistentFlushSchedule schedule(config_.memoryStore.persistFlushIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            if (!priorityControlBlocked(ts)) {
                const auto stats = store_.getStats();
                if (schedule.shouldFlush(stats.persistentCount, stats.persistentConfiguredLimit)) {
                    flushPersistentOnce();
                    schedule.succeeded();
                }
            }
        } catch (...) {
            schedule.failed();
        }
        try {
            std::unique_lock<std::mutex> lock(persistentFlushMutex_, std::try_to_lock);
            if (lock.owns_lock() && running_.load() && !priorityControlBlocked(nowMs())) {
                sqliteWriter_.cleanupExpiredSamples(nowMs());
            }
        } catch (const std::exception& ex) {
            std::cerr << "point history cleanup failed: " << ex.what() << std::endl;
        }
        sleepInterruptibly(running_, PersistentFlushSchedule::kCheckIntervalMs);
    }
}

void GatewayDaemon::writebackLoop() {
    const auto intervalMs = std::max(100, config_.memoryStore.writebackIntervalMs);
    auto nextDeadline = steadyMs() + intervalMs;
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            processWritebackOnce(ts);
        } catch (...) {
        }
        const auto remaining = static_cast<int>(std::max<std::int64_t>(0, nextDeadline - steadyMs()));
        sleepInterruptibly(running_, std::min(intervalMs, std::max(1, remaining)));
        nextDeadline += intervalMs;
        const auto now = steadyMs();
        if (nextDeadline < now) {
            nextDeadline = now + intervalMs;
        }
    }
}

void GatewayDaemon::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson
) const {
    if (!mqttPublisher_) {
        return;
    }
    const auto serviceName = config_.protocol.type == "dlt645_2007"
        ? "dlt645-daemon"
        : config_.protocol.type == "local_dio"
            ? "dio-daemon"
        : "modbus-daemon";
    std::ostringstream payload;
    payload << "{\"service\":\"" << serviceName << "\",\"event\":\""
            << event
            << "\",\"ts\":"
            << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    try {
        mqttPublisher_->publishStatusMessage(config_.machineCode, payload.str());
    } catch (const std::exception& ex) {
        std::cerr << "gateway status publish failed"
                  << " event=" << event
                  << " error=" << ex.what()
                  << std::endl;
    } catch (...) {
        std::cerr << "gateway status publish failed"
                  << " event=" << event
                  << " error=unknown"
                  << std::endl;
    }
}

void GatewayDaemon::initializeRuntimeDevices() {
    auto expandedConfigs = expandRuntimeConfigs(config_);
    store_.registerDevicePoints(expandedConfigs);
    runtimeDevices_.reserve(expandedConfigs.size());

    for (std::size_t i = 0; i < expandedConfigs.size(); ++i) {
        RuntimeDevice runtimeDevice;
        runtimeDevice.config = std::move(expandedConfigs[i]);
        runtimeDevice.collector = collectorFactory_(runtimeDevice.config, store_);
        runtimeDevice.executor = commandExecutorFactory_(runtimeDevice.config, store_);
        if (!runtimeDevice.collector) {
            throw std::invalid_argument("collectorFactory returned null for meter: " + runtimeDevice.config.meterCode);
        }
        if (!runtimeDevice.executor) {
            throw std::invalid_argument("commandExecutorFactory returned null for meter: " + runtimeDevice.config.meterCode);
        }

        for (const auto& point : runtimeDevice.config.points) {
            const auto inserted = indexToRuntimeDevice_.emplace(point.index, runtimeDevices_.size());
            if (!inserted.second) {
                throw std::invalid_argument("duplicate point.index across logical meters: " + std::to_string(point.index));
            }
        }

        meterCodeToRuntimeDevice_.emplace(runtimeDevice.config.meterCode, runtimeDevices_.size());
        runtimeDevices_.push_back(std::move(runtimeDevice));
    }

    if (runtimeDevices_.empty()) {
        throw std::invalid_argument("no enabled runtime meters configured");
    }
}

}  // namespace edge_gateway
