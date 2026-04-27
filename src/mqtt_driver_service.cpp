#include "edge_gateway/mqtt_driver_service.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "edge_gateway/config_loader.hpp"

namespace edge_gateway {

namespace {

void sleepInterruptibly(const std::atomic<bool>& running, int intervalMs) {
    int remaining = std::max(0, intervalMs);
    while (running.load() && remaining > 0) {
        const int slice = std::min(remaining, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
}

class FlatJsonReader {
public:
    explicit FlatJsonReader(const std::string& text) : text_(text) {
    }

    bool tryGetString(const char* key, std::string* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        if (cursor >= text_.size() || text_[cursor] != '"') {
            return false;
        }
        ++cursor;
        std::string value;
        while (cursor < text_.size()) {
            const char ch = text_[cursor++];
            if (ch == '"') {
                *out = value;
                return true;
            }
            if (ch == '\\') {
                if (cursor >= text_.size()) {
                    return false;
                }
                const char esc = text_[cursor++];
                switch (esc) {
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/': value.push_back('/'); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: return false;
                }
            } else {
                value.push_back(ch);
            }
        }
        return false;
    }

    bool tryGetDouble(const char* key, double* out) const {
        const auto pos = findKey(key);
        if (pos == std::string::npos) {
            return false;
        }
        auto cursor = skipWhitespace(pos);
        const auto begin = cursor;
        if (cursor < text_.size() && (text_[cursor] == '-' || text_[cursor] == '+')) {
            ++cursor;
        }
        while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
            ++cursor;
        }
        if (cursor < text_.size() && text_[cursor] == '.') {
            ++cursor;
            while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
                ++cursor;
            }
        }
        if (cursor < text_.size() && (text_[cursor] == 'e' || text_[cursor] == 'E')) {
            ++cursor;
            if (cursor < text_.size() && (text_[cursor] == '-' || text_[cursor] == '+')) {
                ++cursor;
            }
            while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) != 0) {
                ++cursor;
            }
        }
        if (begin == cursor) {
            return false;
        }
        *out = std::strtod(text_.c_str() + begin, nullptr);
        return true;
    }

    bool tryGetUInt32(const char* key, std::uint32_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        if (value < 0.0) {
            return false;
        }
        *out = static_cast<std::uint32_t>(value);
        return true;
    }

    bool tryGetUInt64(const char* key, std::uint64_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        if (value < 0.0) {
            return false;
        }
        *out = static_cast<std::uint64_t>(value);
        return true;
    }

    bool tryGetInt64(const char* key, std::int64_t* out) const {
        double value = 0.0;
        if (!tryGetDouble(key, &value)) {
            return false;
        }
        *out = static_cast<std::int64_t>(value);
        return true;
    }

private:
    std::size_t findKey(const char* key) const {
        const std::string needle = std::string("\"") + key + "\"";
        auto keyPos = text_.find(needle);
        if (keyPos == std::string::npos) {
            return std::string::npos;
        }
        keyPos += needle.size();
        keyPos = skipWhitespace(keyPos);
        if (keyPos >= text_.size() || text_[keyPos] != ':') {
            return std::string::npos;
        }
        return keyPos + 1;
    }

    std::size_t skipWhitespace(std::size_t pos) const {
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos])) != 0) {
            ++pos;
        }
        return pos;
    }

    const std::string& text_;
};

MqttCommandRequest parseCommandRequest(const std::string& payload) {
    FlatJsonReader json(payload);
    MqttCommandRequest request;
    if (!json.tryGetString("cmdId", &request.cmdId) || request.cmdId.empty()) {
        throw std::invalid_argument("command cmdId is required");
    }
    json.tryGetString("machineCode", &request.machineCode);
    json.tryGetString("meterCode", &request.meterCode);
    json.tryGetString("pointCode", &request.pointCode);
    if (!json.tryGetUInt32("index", &request.index) || request.index == 0) {
        throw std::invalid_argument("command index is required");
    }
    if (!json.tryGetDouble("value", &request.value)) {
        throw std::invalid_argument("command value is required");
    }
    if (!json.tryGetString("source", &request.source) || request.source.empty()) {
        request.source = "mqtt";
    }
    json.tryGetInt64("ts", &request.ts);
    return request;
}

OtaRequest parseOtaRequest(const std::string& payload) {
    FlatJsonReader json(payload);
    OtaRequest request;
    if (!json.tryGetString("jobId", &request.jobId) || request.jobId.empty()) {
        throw std::invalid_argument("ota jobId is required");
    }
    json.tryGetString("machineCode", &request.machineCode);
    if (!json.tryGetString("artifactUrl", &request.artifactUrl) || request.artifactUrl.empty()) {
        throw std::invalid_argument("ota artifactUrl is required");
    }
    json.tryGetString("version", &request.version);
    json.tryGetString("sha256", &request.sha256);
    json.tryGetUInt64("size", &request.size);
    json.tryGetString("upgradeMode", &request.upgradeMode);
    json.tryGetInt64("ts", &request.ts);
    return request;
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

std::vector<std::uint32_t> collectConfiguredFullUploadIndexes(const std::vector<DeviceConfig>& deviceConfigs) {
    std::vector<std::uint32_t> indexes;
    for (const auto& config : deviceConfigs) {
        if (!config.meters.empty()) {
            for (const auto& device : config.meters) {
                const auto points = effectiveMeterPoints(config, device);
                for (const auto& point : points) {
                    if (point.fullUpload) {
                        indexes.push_back(point.index);
                    }
                }
            }
            continue;
        }
        for (const auto& point : config.points) {
            if (point.fullUpload) {
                indexes.push_back(point.index);
            }
        }
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

}  // namespace

MqttDriverService::MqttDriverService(
    MqttConfig mqttConfig,
    MqttDriverConfig driverConfig,
    std::vector<DeviceConfig> deviceConfigs,
    MemoryPointStore& store,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::unique_ptr<OtaService> otaService
) : mqttConfig_(std::move(mqttConfig)),
    driverConfig_(std::move(driverConfig)),
    ownedRouter_(new PointStoreRouter()),
    router_(*ownedRouter_),
    publisher_(std::move(publisher)),
    eventOutbox_(std::move(eventOutbox)),
    otaService_(std::move(otaService)) {
    router_.addStore(driverConfig_.sharedMemoryName, store);
    router_.addRoutesFromDeviceConfigs(deviceConfigs, driverConfig_.sharedMemoryName);

    for (const auto& entry : router_.routes()) {
        PointRoute route;
        route.machineCode = entry.second.machineCode;
        route.meterCode = entry.second.meterCode;
        route.pointCode = entry.second.pointCode;
        route.writable = entry.second.writable;
        pointRoutes_.emplace(entry.first, route);
    }
    for (const auto& config : deviceConfigs) {
        machineCodes_.insert(config.machineCode);
    }
    const auto pointFullUploadIndexes = collectConfiguredFullUploadIndexes(deviceConfigs);
    if (!pointFullUploadIndexes.empty()) {
        driverConfig_.publishAllOnFull = false;
        driverConfig_.fullUploadIndexes = pointFullUploadIndexes;
    } else {
        std::sort(driverConfig_.fullUploadIndexes.begin(), driverConfig_.fullUploadIndexes.end());
        driverConfig_.fullUploadIndexes.erase(
            std::unique(driverConfig_.fullUploadIndexes.begin(), driverConfig_.fullUploadIndexes.end()),
            driverConfig_.fullUploadIndexes.end()
        );
    }

    if (!publisher_) {
        throw std::invalid_argument("mqtt driver publisher is required");
    }
}

MqttDriverService::MqttDriverService(
    MqttConfig mqttConfig,
    MqttDriverConfig driverConfig,
    std::vector<DeviceConfig> deviceConfigs,
    PointStoreRouter& router,
    std::shared_ptr<IMqttDriverPublisher> publisher,
    std::unique_ptr<MqttEventOutbox> eventOutbox,
    std::unique_ptr<OtaService> otaService
) : mqttConfig_(std::move(mqttConfig)),
    driverConfig_(std::move(driverConfig)),
    ownedRouter_(nullptr),
    router_(router),
    publisher_(std::move(publisher)),
    eventOutbox_(std::move(eventOutbox)),
    otaService_(std::move(otaService)) {
    if (!publisher_) {
        throw std::invalid_argument("mqtt driver publisher is required");
    }
    for (const auto& config : deviceConfigs) {
        machineCodes_.insert(config.machineCode);
    }
    const auto pointFullUploadIndexes = collectConfiguredFullUploadIndexes(deviceConfigs);
    if (!pointFullUploadIndexes.empty()) {
        driverConfig_.publishAllOnFull = false;
        driverConfig_.fullUploadIndexes = pointFullUploadIndexes;
    } else {
        std::sort(driverConfig_.fullUploadIndexes.begin(), driverConfig_.fullUploadIndexes.end());
        driverConfig_.fullUploadIndexes.erase(
            std::unique(driverConfig_.fullUploadIndexes.begin(), driverConfig_.fullUploadIndexes.end()),
            driverConfig_.fullUploadIndexes.end()
        );
    }
    for (const auto& entry : router_.routes()) {
        PointRoute route;
        route.machineCode = entry.second.machineCode;
        route.meterCode = entry.second.meterCode;
        route.pointCode = entry.second.pointCode;
        route.writable = entry.second.writable;
        pointRoutes_.emplace(entry.first, route);
    }
}

MqttDriverService::~MqttDriverService() {
    stop();
}

void MqttDriverService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    publishStatusEvent(
        "started",
        ts,
        std::string(R"("scanIntervalMs":)") + std::to_string(driverConfig_.scanIntervalMs) +
            R"(,"fullUploadIntervalMs":)" + std::to_string(driverConfig_.fullUploadIntervalMs)
    );
    replayPendingOtaStatuses();
    scanThread_ = std::thread(&MqttDriverService::scanLoop, this);
    if (eventOutbox_) {
        replayThread_ = std::thread(&MqttDriverService::replayLoop, this);
    }
}

void MqttDriverService::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    if (replayThread_.joinable()) {
        replayThread_.join();
    }
}

bool MqttDriverService::isRunning() const {
    return running_.load();
}

void MqttDriverService::runScanOnce(std::int64_t nowMs) {
    if (nowMs - lastOtaReplayAttemptMs_ >= 5000) {
        replayPendingOtaStatuses();
        lastOtaReplayAttemptMs_ = nowMs;
    }

    processIncomingMessages(nowMs);

    if (driverConfig_.publishFullOnStart && lastFullUploadMs_ == 0) {
        publishFullSnapshotNow(nowMs);
    } else if (driverConfig_.fullUploadIntervalMs > 0 &&
               (lastFullUploadMs_ == 0 || nowMs - lastFullUploadMs_ >= driverConfig_.fullUploadIntervalMs)) {
        if (shouldDeferSnapshotForEventBacklog(nowMs)) {
            return;
        }
        publishFullSnapshotNow(nowMs);
    }
}

void MqttDriverService::replayEventOutboxIfNeeded(std::int64_t nowMs) {
    if (!eventOutbox_) {
        return;
    }
    const auto intervalMs = std::max(100, driverConfig_.scanIntervalMs);
    if (lastEventOutboxReplayMs_ > 0 && nowMs - lastEventOutboxReplayMs_ < intervalMs) {
        return;
    }
    lastEventOutboxReplayMs_ = nowMs;
    try {
        const auto stats = eventOutbox_->replayWithStats(driverConfig_.eventReplayMaxBytes, [this](const std::string& topic, const std::string& payload) {
            publisher_->publishJsonMessage(topic, payload);
        });
        eventOutbox_->cleanupIfDue(nowMs);
        if (stats.count > 0) {
            publishStatusEvent(
                "event-outbox-replayed",
                nowMs,
                std::string(R"("count":)") + std::to_string(stats.count) +
                    R"(,"bytes":)" + std::to_string(stats.bytes) +
                    R"(,"alarmCount":)" + std::to_string(stats.alarmCount) +
                    R"(,"changeCount":)" + std::to_string(stats.changeCount) +
                    R"(,"otherCount":)" + std::to_string(stats.otherCount) +
                    R"(,"maxBytes":)" + std::to_string(driverConfig_.eventReplayMaxBytes)
            );
        }
    } catch (const std::exception& ex) {
        publishStatusEvent(
            "event-outbox-replay-failed",
            nowMs,
            std::string(R"("message":")") + ex.what() + R"(")"
        );
    }
}

bool MqttDriverService::shouldDeferSnapshotForEventBacklog(std::int64_t nowMs) {
    if (!eventOutbox_ ||
        driverConfig_.snapshotBacklogThreshold == 0 ||
        driverConfig_.snapshotBackoffIntervalMs <= 0 ||
        lastFullUploadMs_ <= 0) {
        return false;
    }

    std::size_t pending = 0;
    try {
        pending = eventOutbox_->pendingCount();
    } catch (const std::exception& ex) {
        publishStatusEvent(
            "event-outbox-pending-count-failed",
            nowMs,
            std::string(R"("message":")") + ex.what() + R"(")"
        );
        return false;
    }

    if (pending <= driverConfig_.snapshotBacklogThreshold) {
        return false;
    }
    if (nowMs - lastFullUploadMs_ >= driverConfig_.snapshotBackoffIntervalMs) {
        return false;
    }
    if (lastSnapshotDeferredMs_ == 0 || nowMs - lastSnapshotDeferredMs_ >= 1000) {
        lastSnapshotDeferredMs_ = nowMs;
        publishStatusEvent(
            "full-snapshot-deferred",
            nowMs,
            std::string(R"("pendingEvents":)") + std::to_string(pending) +
                R"(,"threshold":)" + std::to_string(driverConfig_.snapshotBacklogThreshold) +
                R"(,"backoffMs":)" + std::to_string(driverConfig_.snapshotBackoffIntervalMs)
        );
    }
    return true;
}

void MqttDriverService::processIncomingMessages(std::int64_t nowMs) {
    std::vector<MqttIncomingMessage> messages;
    try {
        messages = publisher_->pollIncoming(std::max(10, std::min(100, driverConfig_.scanIntervalMs)));
    } catch (const std::exception& ex) {
        publishStatusEvent(
            "mqtt-subscribe-unavailable",
            nowMs,
            std::string(R"("message":")") + ex.what() + R"(")"
        );
        return;
    }
    for (const auto& message : messages) {
        try {
            if (message.type == MqttIncomingType::CommandRequest) {
                handleCommandRequest(message.payload, nowMs);
            } else if (message.type == MqttIncomingType::OtaRequest) {
                handleOtaRequest(message.payload, nowMs);
            }
        } catch (const std::exception& ex) {
            (void)ex;
        }
    }
}

void MqttDriverService::publishFullSnapshotNow(std::int64_t nowMs) {
    std::vector<StoredPointValue> values;
    if (driverConfig_.publishAllOnFull || driverConfig_.fullUploadIndexes.empty()) {
        values = enrichValues(router_.getAllLatest(nowMs));
    } else {
        values = filterValues(driverConfig_.fullUploadIndexes, nowMs);
    }
    publisher_->publishFullSnapshot(mqttConfig_.telemetryTopic, values);
    publishStatusEvent(
        "full-snapshot",
        nowMs,
        std::string(R"("valueCount":)") + std::to_string(values.size())
    );
    lastFullUploadMs_ = nowMs;
}

void MqttDriverService::publishOnDemandNow(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs) {
    const auto values = indexes.empty() ? enrichValues(router_.getAllLatest(nowMs)) : filterValues(indexes, nowMs);
    publisher_->publishOnDemand(mqttConfig_.telemetryTopic, values);
    publishStatusEvent(
        "on-demand",
        nowMs,
        std::string(R"("requestedCount":)") + std::to_string(indexes.size()) +
            R"(,"publishedCount":)" + std::to_string(values.size())
    );
}

void MqttDriverService::scanLoop() {
    const auto intervalMs = std::max(100, driverConfig_.scanIntervalMs);
    while (running_.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        try {
            runScanOnce(nowMs);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

void MqttDriverService::replayLoop() {
    const auto intervalMs = std::max(50, std::min(200, driverConfig_.scanIntervalMs));
    while (running_.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        try {
            replayEventOutboxIfNeeded(nowMs);
        } catch (...) {
        }
        sleepInterruptibly(running_, intervalMs);
    }
}

void MqttDriverService::handleCommandRequest(const std::string& payload, std::int64_t nowMs) {
    MqttCommandReply reply;
    reply.ts = nowMs;

    try {
        const auto request = parseCommandRequest(payload);
        reply.cmdId = request.cmdId;
        reply.machineCode = request.machineCode;
        reply.meterCode = request.meterCode;
        reply.pointCode = request.pointCode;
        reply.index = request.index;
        if (request.machineCode.empty()) {
            throw std::invalid_argument("machineCode is required");
        }

        const auto routeIt = pointRoutes_.find(request.index);
        if (routeIt == pointRoutes_.end()) {
            throw std::invalid_argument("command index not found");
        }

        const auto& route = routeIt->second;
        if (!request.machineCode.empty() && request.machineCode != route.machineCode) {
            throw std::invalid_argument("machineCode mismatch");
        }
        if (!request.meterCode.empty() && request.meterCode != route.meterCode) {
            throw std::invalid_argument("meterCode mismatch");
        }
        if (!request.pointCode.empty() && request.pointCode != route.pointCode) {
            throw std::invalid_argument("pointCode mismatch");
        }
        if (!route.writable) {
            throw std::invalid_argument("point write is disabled");
        }

        PendingWriteCommand command;
        command.cmdId = request.cmdId;
        command.index = request.index;
        command.value = request.value;
        command.source = request.source;
        command.ts = request.ts > 0 ? request.ts : nowMs;
        const auto submitResult = router_.submitWriteCommand(command);
        if (!submitResult.accepted) {
            throw std::invalid_argument(submitResult.message);
        }

        reply.success = true;
        reply.message = "accepted";
        reply.machineCode = route.machineCode;
        reply.meterCode = route.meterCode;
        reply.pointCode = route.pointCode;
        std::cout << "mqtt command accepted"
                  << " cmdId=" << reply.cmdId
                  << " index=" << reply.index
                  << " device=" << reply.meterCode
                  << std::endl;
        publishStatusEvent(
            "command-accepted",
            nowMs,
            std::string(R"("cmdId":")") + reply.cmdId +
                R"(","index":)" + std::to_string(reply.index) +
                R"(,"meterCode":")" + reply.meterCode + R"(")"
        );
    } catch (const std::exception& ex) {
        reply.success = false;
        reply.message = ex.what();
        std::cerr << "mqtt command rejected"
                  << " cmdId=" << reply.cmdId
                  << " index=" << reply.index
                  << " error=" << reply.message
                  << std::endl;
        publishStatusEvent(
            "command-rejected",
            nowMs,
            std::string(R"("cmdId":")") + reply.cmdId +
                R"(","index":)" + std::to_string(reply.index) +
                R"(,"message":")" + reply.message + R"(")"
        );
    }

    publisher_->publishCommandReply(mqttConfig_.commandReplyTopic, reply);
}

void MqttDriverService::handleOtaRequest(const std::string& payload, std::int64_t nowMs) {
    OtaReply reply;
    OtaStatus status;
    reply.ts = nowMs;
    status.ts = nowMs;

    try {
        const auto request = parseOtaRequest(payload);
        if (request.machineCode.empty()) {
            throw std::invalid_argument("machineCode is required");
        }
        const std::string machineCode = request.machineCode;
        reply.jobId = request.jobId;
        reply.machineCode = machineCode;
        status.jobId = request.jobId;
        status.machineCode = machineCode;

        if (!request.machineCode.empty() && machineCodes_.find(request.machineCode) == machineCodes_.end()) {
            throw std::invalid_argument("machineCode mismatch");
        }
        if (!otaService_ || !otaService_->enabled()) {
            throw std::runtime_error("ota is disabled");
        }

        reply = otaService_->createAcceptedReply(request, machineCode, nowMs);
        publisher_->publishOtaReply(mqttConfig_.otaReplyTopic, reply);

        otaService_->execute(
            request,
            machineCode,
            nowMs,
            &reply,
            &status,
            [this](const OtaStatus& stageStatus) {
                publisher_->publishOtaStatus(mqttConfig_.otaStatusTopic, stageStatus);
            }
        );
        std::cout << "mqtt ota request handled"
                  << " jobId=" << reply.jobId
                  << " stage=" << status.stage
                  << " message=" << status.message
                  << std::endl;
        publishStatusEvent(
            "ota-handled",
            nowMs,
            std::string(R"("jobId":")") + reply.jobId +
                R"(","stage":")" + status.stage +
                R"(","message":")" + status.message + R"(")"
        );
    } catch (const std::exception& ex) {
        reply.accepted = false;
        if (reply.message.empty()) {
            reply.message = ex.what();
        }
        status.stage = "failed";
        status.progress = 0;
        status.message = ex.what();
        std::cerr << "mqtt ota request rejected"
                  << " jobId=" << reply.jobId
                  << " error=" << ex.what()
                  << std::endl;
        publishStatusEvent(
            "ota-rejected",
            nowMs,
            std::string(R"("jobId":")") + reply.jobId +
                R"(","message":")" + ex.what() + R"(")"
        );
        publisher_->publishOtaReply(mqttConfig_.otaReplyTopic, reply);
    }

    publisher_->publishOtaStatus(mqttConfig_.otaStatusTopic, status);
}

void MqttDriverService::publishStatusEvent(
    const std::string& event,
    std::int64_t ts,
    const std::string& detailsJson
) const {
    if (mqttConfig_.statusTopic.empty()) {
        return;
    }
    std::ostringstream payload;
    payload << "{\"service\":\"mqtt-driver\",\"event\":\""
            << event
            << "\",\"ts\":"
            << ts;
    if (!detailsJson.empty()) {
        payload << "," << detailsJson;
    }
    payload << "}";
    publisher_->publishJsonMessage(mqttConfig_.statusTopic, payload.str());
}

std::vector<StoredPointValue> MqttDriverService::filterValues(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const {
    return enrichValues(router_.getLatestByIndexes(indexes, nowMs));
}

void MqttDriverService::enrichValue(StoredPointValue& value) const {
    const auto routeIt = pointRoutes_.find(value.index);
    if (routeIt == pointRoutes_.end()) {
        return;
    }
    const auto& route = routeIt->second;
    if (value.machineCode.empty()) {
        value.machineCode = route.machineCode;
    }
    if (value.meterCode.empty()) {
        value.meterCode = route.meterCode;
    }
    if (value.pointCode.empty()) {
        value.pointCode = route.pointCode;
    }
}

void MqttDriverService::replayPendingOtaStatuses() {
    if (!otaService_) {
        return;
    }
    lastOtaReplayAttemptMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    const auto statuses = otaService_->loadPendingStatuses();
    if (statuses.empty()) {
        return;
    }
    std::size_t published = 0;
    for (const auto& status : statuses) {
        try {
            publisher_->publishOtaStatus(mqttConfig_.otaStatusTopic, status);
            ++published;
        } catch (...) {
            break;
        }
    }
    if (published == statuses.size()) {
        otaService_->clearPendingStatuses();
    }
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    publishStatusEvent(
        "ota-status-replayed",
        ts,
        std::string(R"("count":)") + std::to_string(published)
    );
}

std::vector<StoredPointValue> MqttDriverService::enrichValues(std::vector<StoredPointValue> values) const {
    for (auto& value : values) {
        enrichValue(value);
    }
    return values;
}

}  // namespace edge_gateway
