#include "edge_gateway/dlt645_collector.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "edge_gateway/dlt645_client.hpp"
#include "edge_gateway/dlt645_codec.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

}  // namespace

Dlt645Collector::Dlt645Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<Dlt645Client> dlt645Client,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    dlt645Client_(std::move(dlt645Client)) {
    if (!dlt645Client_) {
        throw std::invalid_argument("dlt645Client is required");
    }
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
            continue;
        }
        if (std::find(dataIdOrder_.begin(), dataIdOrder_.end(), point.read.dlt645Di) == dataIdOrder_.end()) {
            dataIdOrder_.push_back(point.read.dlt645Di);
        }
    }
}

CollectCycleResult Dlt645Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    CollectCycleResult result;
    if (shouldSkipFailedCollectionCycle()) {
        return result;
    }
    const auto points = duePoints(nowMs);
    if (points.empty() || dataIdOrder_.empty()) {
        return result;
    }

    std::unordered_map<std::string, std::vector<PointDefinition>> dueByDataId;
    for (const auto& point : points) {
        dueByDataId[point.read.dlt645Di].push_back(point);
    }

    const auto requestBudget = static_cast<std::size_t>(std::max(
        1,
        realtimeFocused
            ? config_.collect.realtimeMaxTasksPerMeterPerCycle
            : config_.collect.maxTasksPerMeterPerCycle
    ));
    bool hasSuccessfulCommunication = false;
    bool attemptedRead = false;
    std::string firstFailureMessage;
    std::size_t requests = 0;
    while (!dueByDataId.empty() && requests < requestBudget) {
        std::size_t selectedIndex = dataIdOrder_.size();
        for (std::size_t offset = 0; offset < dataIdOrder_.size(); ++offset) {
            const auto candidate = (dataIdCursor_ + offset) % dataIdOrder_.size();
            if (dueByDataId.find(dataIdOrder_[candidate]) != dueByDataId.end()) {
                selectedIndex = candidate;
                break;
            }
        }
        if (selectedIndex >= dataIdOrder_.size()) {
            break;
        }

        const auto dataId = dataIdOrder_[selectedIndex];
        auto groupIt = dueByDataId.find(dataId);
        auto group = std::move(groupIt->second);
        dueByDataId.erase(groupIt);
        attemptedRead = true;
        ++requests;

        std::vector<std::uint8_t> response;
        std::string readError;
        try {
            if (config_.address.empty()) {
                throw std::runtime_error("DLT645 meter address is empty");
            }
            if (dataId.empty()) {
                throw std::runtime_error("DLT645 point missing read.dlt645.di: " + group.front().pointCode);
            }
            response = dlt645Client_->readData(config_.address, dataId);
            hasSuccessfulCommunication = true;
            failedRetryRoundsByDataId_.erase(dataId);
            dataIdCursor_ = (selectedIndex + 1) % dataIdOrder_.size();
        } catch (const std::exception& ex) {
            readError = ex.what();
            if (readError.find("DLT645 read failed") == std::string::npos) {
                readError = std::string("DLT645 read failed address=") + config_.address +
                    " di=" + dataId +
                    " pointCode=" + group.front().pointCode +
                    " error=" + ex.what();
            }
            if (firstFailureMessage.empty()) {
                firstFailureMessage = readError;
            }

            auto& failedRounds = failedRetryRoundsByDataId_[dataId];
            ++failedRounds;
            if (failedRounds > std::max(0, config_.protocol.transport.readRetryCount)) {
                failedRetryRoundsByDataId_.erase(dataId);
                dataIdCursor_ = (selectedIndex + 1) % dataIdOrder_.size();
            } else {
                dataIdCursor_ = selectedIndex;
            }
        }

        for (const auto& point : group) {
            PointValue value;
            if (!readError.empty()) {
                value = buildFailedPointValue(point, readError, currentTimeMs());
            } else {
                try {
                    const auto decoded = Dlt645Codec::decodeReadResponse(response, point);
                    value = buildPointValue(point, decoded, currentTimeMs());
                } catch (const std::exception& ex) {
                    auto message = std::string("DLT645 decode failed address=") + config_.address +
                        " di=" + dataId +
                        " pointCode=" + point.pointCode +
                        " error=" + ex.what();
                    if (firstFailureMessage.empty()) {
                        firstFailureMessage = message;
                    }
                    value = buildFailedPointValue(point, message, currentTimeMs());
                }
            }
            lastReadMs_[point.index] = nowMs;
            lastValueUpdateMs_[point.index] = nowMs;
            if (point.read.cachePolicy.storeLatest) {
                store_.putLatest(value);
            }
            result.values.push_back(std::move(value));
        }

        if (!readError.empty()) {
            break;
        }
    }
    if (!points.empty()) {
        publishDeviceOnlineStatus(hasSuccessfulCommunication, nowMs);
    }
    if (mqttPublisher_ && !result.values.empty()) {
        mqttPublisher_->publishTelemetry(config_.machineCode, result.values);
    }
    if (attemptedRead && !hasSuccessfulCommunication && !firstFailureMessage.empty()) {
        recordCollectionCycleFailure();
        throw std::runtime_error(firstFailureMessage);
    }
    if (hasSuccessfulCommunication) {
        recordCollectionCycleSuccess();
    }
    return result;
}

}  // namespace edge_gateway
