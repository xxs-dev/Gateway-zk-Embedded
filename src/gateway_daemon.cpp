#include "edge_gateway/gateway_daemon.hpp"

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

namespace edge_gateway {

namespace {

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

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
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

GatewayDaemon::GatewayDaemon(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::shared_ptr<IGpioPort> gpioPort,
    std::string realtimeMeterLeaseFile
) : config_(std::move(config)),
    store_(store),
    realtimeMeterLeaseFile_(std::move(realtimeMeterLeaseFile)),
    sqliteWriter_(config_.memoryStore.sqlitePath, config_.memoryStore.sqliteLibraryPath),
    mqttPublisher_(std::move(mqttPublisher)),
    gpioPort_(std::move(gpioPort)) {
    initializeRuntimeDevices(std::move(modbusClient), std::move(dlt645Client), mqttPublisher_, gpioPort_);
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
}

bool GatewayDaemon::isRunning() const {
    return running_.load();
}

void GatewayDaemon::collectOnce(std::int64_t nowMsValue) {
    if (runtimeDevices_.empty()) {
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
            runtimeDevice.collector->collectOnce(nowMsValue, realtimeFocused);
        } catch (const std::exception& ex) {
            runtimeDevice.collector->publishDeviceOnlineStatus(false, nowMsValue);
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
    };

    const auto activeIndexes = activeRealtimeDeviceIndexes(nowMsValue);
    std::set<std::size_t> activeIndexSet(activeIndexes.begin(), activeIndexes.end());
    for (const auto index : activeIndexes) {
        collectDevice(index, true);
    }
    if (!activeIndexes.empty()) {
        return;
    }

    const auto targetCount = std::max(batchSize, collectedIndexes.size());
    std::size_t attempts = 0;
    while (attempts < runtimeDevices_.size() && collectedIndexes.size() < targetCount) {
        const auto index = collectCursor_ % runtimeDevices_.size();
        collectCursor_ = (collectCursor_ + 1) % runtimeDevices_.size();
        ++attempts;
        if (collectedIndexes.find(index) != collectedIndexes.end()) {
            continue;
        }
        collectDevice(index, activeIndexSet.find(index) != activeIndexSet.end());
    }
}

std::size_t GatewayDaemon::flushPersistentOnce() {
    const auto samples = store_.drainPersistentSamples();
    sqliteWriter_.writeSamples(samples);
    if (!samples.empty()) {
        publishStatusEvent(
            "persist-flushed",
            nowMs(),
            std::string(R"("count":)") + std::to_string(samples.size())
        );
    }
    return samples.size();
}

std::size_t GatewayDaemon::processWritebackOnce(std::int64_t nowMsValue) {
    const auto commands = store_.drainPendingWriteCommands(config_.memoryStore.writebackBatchSize);
    std::size_t processed = 0;
    for (const auto& command : commands) {
        const auto it = indexToRuntimeDevice_.find(command.index);
        if (it == indexToRuntimeDevice_.end()) {
            std::cerr << "writeback skipped index=" << command.index << " reason=unknown-index" << std::endl;
            publishStatusEvent(
                "writeback-skipped",
                nowMsValue,
                std::string(R"("index":)") + std::to_string(command.index) +
                    R"(,"reason":"unknown-index")"
            );
            continue;
        }
        try {
            const auto result = runtimeDevices_[it->second].executor->executeByIndex(
                command.cmdId,
                command.index,
                command.value,
                nowMsValue
            );
            const auto& runtimeDevice = runtimeDevices_[it->second];
            if (!result.success) {
                std::cerr << "writeback failed"
                          << " device=" << runtimeDevice.config.meterCode
                          << " slave=" << runtimeDevice.config.protocol.slave
                          << " index=" << command.index
                          << " error=" << result.message
                          << std::endl;
                publishStatusEvent(
                    "writeback-failed",
                    nowMsValue,
                    std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                        R"(","index":)" + std::to_string(command.index) +
                        R"(,"message":")" + escapeJson(result.message) + R"(")"
                );
                continue;
            }
            ++processed;
            publishStatusEvent(
                "writeback-succeeded",
                nowMsValue,
                std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"cmdId":")" + escapeJson(command.cmdId) + R"(")"
            );
        } catch (const std::exception& ex) {
            const auto& runtimeDevice = runtimeDevices_[it->second];
            std::cerr << "writeback failed"
                      << " device=" << runtimeDevice.config.meterCode
                      << " slave=" << runtimeDevice.config.protocol.slave
                      << " index=" << command.index
                      << " error=" << ex.what()
                      << std::endl;
            publishStatusEvent(
                "writeback-failed",
                nowMsValue,
                std::string(R"("meterCode":")") + runtimeDevice.config.meterCode +
                    R"(","index":)" + std::to_string(command.index) +
                    R"(,"message":")" + escapeJson(ex.what()) + R"(")"
            );
        }
    }
    return processed;
}

void GatewayDaemon::collectLoop() {
    const auto intervalMs = collectLoopIntervalMs();
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            store_.heartbeatRegisteredPoints(ts);
            collectOnce(ts);
            store_.removeExpired(ts);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

int GatewayDaemon::collectLoopIntervalMs() const {
    int intervalMs = std::max(100, config_.collect.defaultIntervalMs);
    for (const auto& runtimeDevice : runtimeDevices_) {
        for (const auto& point : runtimeDevice.config.points) {
            if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
                continue;
            }
            const auto pointIntervalMs = point.read.intervalMs > 0
                ? point.read.intervalMs
                : config_.collect.defaultIntervalMs;
            intervalMs = std::min(intervalMs, std::max(100, pointIntervalMs));
        }
    }
    return intervalMs;
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
    const auto intervalMs = std::max(1000, config_.memoryStore.persistFlushIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            store_.heartbeatRegisteredPoints(ts);
            flushPersistentOnce();
            store_.removeExpired(ts);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

void GatewayDaemon::writebackLoop() {
    const auto intervalMs = std::max(100, config_.memoryStore.writebackIntervalMs);
    while (running_.load()) {
        try {
            const auto ts = nowMs();
            store_.heartbeatRegisteredPoints(ts);
            processWritebackOnce(ts);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
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
    mqttPublisher_->publishStatusMessage(config_.machineCode, payload.str());
}

void GatewayDaemon::initializeRuntimeDevices(
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::shared_ptr<IGpioPort> gpioPort
) {
    auto expandedConfigs = expandRuntimeConfigs(config_);
    runtimeDevices_.reserve(expandedConfigs.size());

    for (std::size_t i = 0; i < expandedConfigs.size(); ++i) {
        RuntimeDevice runtimeDevice;
        runtimeDevice.config = std::move(expandedConfigs[i]);
        runtimeDevice.collector.reset(new Collector(runtimeDevice.config, store_, modbusClient, dlt645Client, nullptr, gpioPort));
        runtimeDevice.executor.reset(new CommandExecutor(runtimeDevice.config, store_, modbusClient, mqttPublisher, gpioPort));

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
