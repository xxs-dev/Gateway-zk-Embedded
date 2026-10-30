#include "edge_gateway/dlt645_collector.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
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
    for (std::size_t i = 0; i < config_.points.size(); ++i) {
        const auto& point = config_.points[i];
        if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") {
            continue;
        }
        if (point.read.dlt645Di.empty()) {
            std::cerr << "dlt645 point without read.dlt645.di is not collected"
                      << " meter=" << config_.meterCode << " pointCode=" << point.pointCode << std::endl;
            continue;
        }
        auto group = std::find_if(groups_.begin(), groups_.end(), [&point](const ReadGroup& candidate) {
            return candidate.dataId == point.read.dlt645Di;
        });
        if (group == groups_.end()) {
            groups_.push_back(ReadGroup());
            group = groups_.end() - 1;
            group->dataId = point.read.dlt645Di;
        }
        group->points.push_back(i);
        group->realtime = group->realtime || point.collectPriority == 0;
    }
}

void Dlt645Collector::onSchedulingClockStepped(std::int64_t deltaMs) {
    if (lastBackgroundReadMs_ >= 0) {
        lastBackgroundReadMs_ += deltaMs;
    }
}

std::int64_t Dlt645Collector::dueAtMs(const ReadGroup& group) const {
    auto due = std::numeric_limits<std::int64_t>::max();
    for (const auto position : group.points) {
        const auto& point = config_.points[position];
        const auto last = lastReadMs_.find(point.index);
        if (last == lastReadMs_.end()) {
            return std::numeric_limits<std::int64_t>::min();  // never read: due now, first
        }
        due = std::min(due, last->second + effectiveIntervalMs(point));
    }
    return due;
}

CollectCycleResult Dlt645Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    observeSchedulingClock(nowMs);
    CollectCycleResult result;
    storedThisCycle_.clear();
    if (groups_.empty() || shouldSkipFailedCollectionCycle()) {
        return result;
    }

    // Due DIs, most overdue first (ties keep configuration order).
    std::vector<std::pair<std::int64_t, std::size_t>> realtimeDue;
    std::vector<std::pair<std::int64_t, std::size_t>> backgroundDue;
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        const auto due = dueAtMs(groups_[i]);
        if (due <= nowMs) {
            (groups_[i].realtime ? realtimeDue : backgroundDue).emplace_back(due, i);
        }
    }
    std::sort(realtimeDue.begin(), realtimeDue.end());
    std::sort(backgroundDue.begin(), backgroundDue.end());

    const auto& collect = config_.collect;
    const auto budget = static_cast<std::size_t>(std::max(
        1, realtimeFocused ? collect.realtimeMaxTasksPerMeterPerCycle : collect.maxTasksPerMeterPerCycle
    ));
    std::vector<std::size_t> plan;
    for (std::size_t i = 0; i < realtimeDue.size() && plan.size() < budget; ++i) {
        plan.push_back(realtimeDue[i].second);
    }
    // Background DIs use the slots realtime leaves free; while realtime fills the budget they still
    // get one read per backgroundTaskIntervalMs (same share rule as the Modbus collector).
    auto backgroundSlots = budget - plan.size();
    if (lastBackgroundReadMs_ < 0) {
        lastBackgroundReadMs_ = nowMs;
    }
    if (backgroundSlots == 0 && nowMs - lastBackgroundReadMs_ >= std::max(0, collect.backgroundTaskIntervalMs)) {
        backgroundSlots = 1;
    }
    for (std::size_t i = 0; i < backgroundDue.size() && i < backgroundSlots; ++i) {
        plan.push_back(backgroundDue[i].second);
    }

    bool attempted = false;
    bool communicated = false;
    std::string firstFailure;
    for (const auto groupIndex : plan) {
        auto& group = groups_[groupIndex];
        const auto& firstPoint = config_.points[group.points.front()];
        attempted = true;
        if (!group.realtime) {
            lastBackgroundReadMs_ = nowMs;
        }
        try {
            if (config_.address.empty()) {
                throw std::runtime_error("DLT645 meter address is empty");
            }
            const auto data = dlt645Client_->readData(config_.address, group.dataId);
            communicated = true;
            group.consecutiveFailures = 0;
            group.retryRounds = 0;
            storeGroupData(group, data, nowMs, result);
        } catch (const Dlt645ErrorResponse& ex) {
            // The meter answered: the DI is not available (definite), the meter is alive.
            communicated = true;
            group.consecutiveFailures = 0;
            group.retryRounds = 0;
            storeGroupError(group, std::string("DLT645 read rejected address=") + config_.address +
                " di=" + group.dataId + " pointCode=" + firstPoint.pointCode + " error=" + ex.what(),
                nowMs, result);
        } catch (const std::exception& ex) {
            const auto message = std::string("DLT645 read failed address=") + config_.address +
                " di=" + group.dataId + " pointCode=" + firstPoint.pointCode + " error=" + ex.what();
            if (firstFailure.empty()) {
                firstFailure = message;
            }
            noteGroupFailure(group, message, nowMs, result);
            break;  // one timeout per cycle at most: a dead meter must not hold the bus
        }
    }

    const auto offlineThreshold = std::max(1, collect.offlineFailureThreshold);
    if (attempted) {
        if (communicated) {
            failedCycles_ = 0;
            ++successfulCycles_;
            if (successfulCycles_ >= std::max(1, collect.recoverySuccessThreshold)) {
                online_ = true;
                offlineMarked_ = false;
            }
            recordCollectionCycleSuccess();
        } else {
            successfulCycles_ = 0;
            ++failedCycles_;
            recordCollectionCycleFailure();
            if (failedCycles_ >= offlineThreshold) {
                online_ = false;
                if (!offlineMarked_) {
                    markMeterOffline(firstFailure, nowMs, result);
                    offlineMarked_ = true;
                }
            }
        }
        publishDeviceOnlineStatus(online_, nowMs);
    }

    auto nextDue = std::numeric_limits<std::int64_t>::max();
    for (const auto& group : groups_) {
        nextDue = std::min(nextDue, dueAtMs(group));
    }
    if (nextDue > nowMs && nextDue - nowMs < std::numeric_limits<int>::max()) {
        result.nextDelayMs = static_cast<int>(nextDue - nowMs);
    }

    publishTelemetryBestEffort(result.values);
    if (attempted && !communicated) {
        if (failedCycles_ >= offlineThreshold) {
            throw std::runtime_error(firstFailure);  // the daemon reports collect-failed / offline
        }
        std::cerr << "dlt645 collect failed meter=" << config_.meterCode
                  << " failedCycles=" << failedCycles_ << "/" << offlineThreshold
                  << " error=" << firstFailure << std::endl;
    }
    return result;
}

void Dlt645Collector::storeValue(
    const PointDefinition& point,
    PointValue value,
    std::int64_t nowMs,
    CollectCycleResult& result
) {
    lastValueUpdateMs_[point.index] = nowMs;
    storedThisCycle_.insert(point.index);
    if (point.read.cachePolicy.storeLatest) {
        store_.putLatest(value);
    }
    result.values.push_back(std::move(value));
}

void Dlt645Collector::storeGroupData(
    const ReadGroup& group,
    const std::vector<std::uint8_t>& data,
    std::int64_t nowMs,
    CollectCycleResult& result
) {
    for (const auto position : group.points) {
        const auto& point = config_.points[position];
        PointValue value;
        try {
            value = buildPointValue(point, Dlt645Codec::decodePayload(data, point), currentTimeMs());
        } catch (const std::exception& ex) {
            value = buildFailedPointValue(point, std::string("DLT645 decode failed address=") + config_.address +
                " di=" + group.dataId + " pointCode=" + point.pointCode + " error=" + ex.what(), currentTimeMs());
        }
        lastReadMs_[point.index] = nowMs;
        storeValue(point, std::move(value), nowMs, result);
    }
}

void Dlt645Collector::storeGroupError(
    const ReadGroup& group,
    const std::string& message,
    std::int64_t nowMs,
    CollectCycleResult& result
) {
    for (const auto position : group.points) {
        const auto& point = config_.points[position];
        lastReadMs_[point.index] = nowMs;
        storeValue(point, buildFailedPointValue(point, message, currentTimeMs()), nowMs, result);
    }
}

void Dlt645Collector::noteGroupFailure(
    ReadGroup& group,
    const std::string& message,
    std::int64_t nowMs,
    CollectCycleResult& result
) {
    ++group.consecutiveFailures;
    // readRetryCount: the failed DI stays first in line for that many further cycles.
    const bool retry = group.retryRounds < std::max(0, config_.protocol.transport.readRetryCount);
    if (retry) {
        ++group.retryRounds;
    } else {
        group.retryRounds = 0;
    }
    const auto threshold = std::max(1, config_.collect.failureBadQualityThreshold);
    const auto graceMs = std::max(0, config_.collect.failureGoodValueGraceMs);
    const auto wallNowMs = currentTimeMs();
    for (const auto position : group.points) {
        const auto& point = config_.points[position];
        if (!retry) {
            lastReadMs_[point.index] = nowMs;
        }
        Optional<StoredPointValue> previous;
        if (point.read.cachePolicy.storeLatest) {
            previous = store_.getLatestByIndex(point.index, wallNowMs);
        }
        const auto lastUpdate = lastValueUpdateMs_.find(point.index);
        const bool withinGrace = graceMs > 0 && lastUpdate != lastValueUpdateMs_.end() &&
            nowMs - lastUpdate->second < graceMs;
        const bool keepGoodValue = previous && previous->quality == 1 && !previous->stale &&
            (group.consecutiveFailures < threshold || withinGrace);
        const bool awaitConfirmedFailure = !previous && group.consecutiveFailures < threshold;
        if (keepGoodValue || awaitConfirmedFailure) {
            continue;
        }
        auto value = buildFailedPointValue(point, message, wallNowMs);
        if (previous) {
            value.value = previous->value;
        }
        storeValue(point, std::move(value), nowMs, result);
    }
}

void Dlt645Collector::markMeterOffline(const std::string& message, std::int64_t nowMs, CollectCycleResult& result) {
    const auto wallNowMs = currentTimeMs();
    const auto text = "DLT645 meter offline: " + message;
    for (const auto& group : groups_) {
        for (const auto position : group.points) {
            const auto& point = config_.points[position];
            if (storedThisCycle_.count(point.index) != 0) {
                continue;
            }
            auto value = buildFailedPointValue(point, text, wallNowMs);
            if (point.read.cachePolicy.storeLatest) {
                const auto previous = store_.getLatestByIndex(point.index, wallNowMs);
                if (previous) {
                    value.value = previous->value;
                }
            }
            storeValue(point, std::move(value), nowMs, result);
        }
    }
}

}  // namespace edge_gateway
