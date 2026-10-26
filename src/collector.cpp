#include "edge_gateway/modbus_collector.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>
#include <stdexcept>

#include "edge_gateway/modbus_codec.hpp"
#include "edge_gateway/read_task_planner.hpp"

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

struct ParsedTaskBackoffKey {
    int function = 0;
    int start = 0;
    int count = 0;
    bool valid = false;
};

ParsedTaskBackoffKey parseTaskBackoffKey(const std::string& key) {
    ParsedTaskBackoffKey parsed;
    const auto first = key.find(':');
    if (first == std::string::npos) {
        return parsed;
    }
    const auto second = key.find(':', first + 1);
    if (second == std::string::npos) {
        return parsed;
    }
    try {
        parsed.function = std::stoi(key.substr(0, first));
        parsed.start = std::stoi(key.substr(first + 1, second - first - 1));
        parsed.count = std::stoi(key.substr(second + 1));
        parsed.valid = parsed.count > 0;
    } catch (...) {
        parsed.valid = false;
    }
    return parsed;
}

bool rangeCovers(int outerStart, int outerCount, int innerStart, int innerCount) {
    const auto outerEnd = outerStart + std::max(0, outerCount);
    const auto innerEnd = innerStart + std::max(0, innerCount);
    return outerStart <= innerStart && innerEnd <= outerEnd;
}

}  // namespace

Collector::Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::function<std::int64_t()> monotonicClockMs
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    modbusClient_(std::move(modbusClient)),
    monotonicClockMs_(std::move(monotonicClockMs)) {
    if (!modbusClient_) {
        throw std::invalid_argument("modbusClient is required");
    }
    if (!monotonicClockMs_) {
        monotonicClockMs_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
    }
    for (std::size_t i = 0; i < config_.points.size(); ++i) {
        const auto& point = config_.points[i];
        if (point.enabled && point.read.enable && point.read.dataType != "device_online") {
            timeSliceMs_ = std::min(timeSliceMs_, effectiveIntervalMs(point));
            readablePointOrder_.push_back(i);
        }
    }
    std::stable_sort(readablePointOrder_.begin(), readablePointOrder_.end(), [&](auto lhs, auto rhs) {
        const auto& a = config_.points[lhs];
        const auto& b = config_.points[rhs];
        return std::tie(a.read.function, a.address) < std::tie(b.read.function, b.address);
    });
}

CollectCycleResult Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    CollectCycleResult result;
    const auto startedMs = monotonicClockMs_();
    const auto schedulingNowMs = [&] {
        return nowMs + std::max<std::int64_t>(0, monotonicClockMs_() - startedMs);
    };
    const auto realtimeBecameDue = [&](std::int64_t currentMs) {
        for (const auto& point : config_.points) {
            if (!point.enabled || !point.read.enable || point.read.dataType == "device_online" || pointPriority(point) != 0) continue;
            const auto last = lastReadMs_.find(point.index);
            if (last == lastReadMs_.end()) continue;
            const auto dueMs = last->second + effectiveIntervalMs(point);
            if (dueMs > nowMs && dueMs <= currentMs) return true;
        }
        return false;
    };
    ++collectCycle_;
    if (shouldSkipForBackoff(nowMs)) {
        return result;
    }
    std::vector<const PointDefinition*> realtimePoints;
    std::vector<const PointDefinition*> backgroundPoints;
    realtimePoints.reserve(readablePointOrder_.size());
    backgroundPoints.reserve(readablePointOrder_.size());
    for (const auto index : readablePointOrder_) {
        const auto& point = config_.points[index];
        const auto last = lastReadMs_.find(point.index);
        if (last != lastReadMs_.end() && nowMs - last->second < effectiveIntervalMs(point)) continue;
        if (pointPriority(point) == 0) {
            realtimePoints.push_back(&point);
        } else {
            backgroundPoints.push_back(&point);
        }
    }
    const auto buildPlannedTasks = [&](const std::vector<const PointDefinition*>& sourcePoints) {
        auto tasks = ReadTaskPlanner::buildSorted(
            sourcePoints,
            config_.collect.maxBatchRegisters,
            config_.collect.batchOptimize
        );
        if (taskFailureStates_.empty()) return tasks;
        return expandBackedOffTasksBeforeBudget(tasks, nowMs, realtimeFocused);
    };
    auto realtimeTasks = buildPlannedTasks(realtimePoints);
    auto backgroundTasks = buildPlannedTasks(backgroundPoints);

    bool hasSuccessfulRead = false;
    std::string firstFailureMessage;
    const auto recordFailure = [&](const PointDefinition& point, const std::string& message) {
        const auto completedMs = schedulingNowMs();
        if (firstFailureMessage.empty()) {
            firstFailureMessage = message;
        }
        auto value = buildFailedPointValue(point, message, completedMs);
        lastReadMs_[point.index] = completedMs;
        const auto failureCount = ++pointFailureCounts_[point.index];
        bool valueUpdated = !point.read.cachePolicy.storeLatest;
        if (point.read.cachePolicy.storeLatest) {
            const auto previous = store_.getLatestByIndex(point.index, completedMs);
            const auto lastUpdate = lastValueUpdateMs_.find(point.index);
            const auto previousUpdateMs = lastUpdate == lastValueUpdateMs_.end()
                ? previous ? previous->ts : 0
                : lastUpdate->second;
            const auto graceMs = std::max(0, config_.collect.failureGoodValueGraceMs);
            const auto badQualityThreshold = std::max(1, config_.collect.failureBadQualityThreshold);
            const bool keepGoodValue = previous && previous->quality == 1 && !previous->stale &&
                (failureCount < badQualityThreshold ||
                 (graceMs > 0 && completedMs - previousUpdateMs < graceMs));
            if (previous) {
                value.value = previous->value;
                value.expireAt = std::max(previous->expireAt, value.expireAt);
            }
            const bool waitForConfirmedFailure = !previous && failureCount < badQualityThreshold;
            if (!keepGoodValue && !waitForConfirmedFailure) {
                store_.putLatest(value);
                valueUpdated = true;
            }
        }
        if (valueUpdated) {
            lastValueUpdateMs_[point.index] = completedMs;
            result.values.push_back(std::move(value));
        }
    };

    std::vector<ReadTask> plannedTasks;
    bool backgroundSelected = false;
    const bool offlineProbeMode = config_.collect.offlineProbeOnly && !online_ && consecutiveFailures_ > 0 &&
        static_cast<int>(realtimeTasks.size() + backgroundTasks.size()) > config_.collect.offlineProbeTaskCount;
    if (offlineProbeMode) {
        auto candidateTasks = std::move(realtimeTasks);
        candidateTasks.insert(candidateTasks.end(), std::make_move_iterator(backgroundTasks.begin()),
                              std::make_move_iterator(backgroundTasks.end()));
        plannedTasks = limitToOfflineProbeTasks(candidateTasks);
        const auto realtimeTaskBudget = realtimeFocused
            ? config_.collect.realtimeMaxTasksPerMeterPerCycle
            : config_.collect.maxTasksPerMeterPerCycle;
        plannedTasks = limitToMeterTaskBudget(
            std::move(plannedTasks),
            nowMs,
            realtimeTaskBudget,
            realtimeFocused
        );
    } else {
        const auto realtimeTaskBudget = realtimeFocused
            ? config_.collect.realtimeMaxTasksPerMeterPerCycle
            : config_.collect.maxTasksPerMeterPerCycle;
        auto selectedRealtimeTasks = limitToMeterTaskBudget(
            std::move(realtimeTasks),
            nowMs,
            realtimeTaskBudget,
            realtimeFocused
        );
        plannedTasks = std::move(selectedRealtimeTasks);

        const auto backgroundBudget = std::max(
            0,
            realtimeFocused
                ? config_.collect.realtimeMaxBackgroundTasksPerMeterPerCycle
                : config_.collect.maxBackgroundTasksPerMeterPerCycle
        );
        const auto backgroundIntervalMs = std::max(0, config_.collect.backgroundTaskIntervalMs);
        bool backgroundDue = !backgroundTasks.empty() && backgroundBudget > 0;
        if (backgroundDue && backgroundIntervalMs > 0) {
            if (lastBackgroundTaskMs_ < 0) {
                lastBackgroundTaskMs_ = nowMs;
                backgroundDue = false;
            } else {
                backgroundDue = nowMs - lastBackgroundTaskMs_ >= backgroundIntervalMs;
            }
        }
        if (backgroundDue) {
            auto selectedBackgroundTasks = limitToMeterTaskBudget(
                std::move(backgroundTasks),
                nowMs,
                backgroundBudget,
                realtimeFocused
            );
            if (!selectedBackgroundTasks.empty()) {
                backgroundSelected = true;
                // Reserve one overdue background request after a slice deferred it.
                if (backgroundDeferred_) {
                    plannedTasks.insert(plannedTasks.begin(), std::move(selectedBackgroundTasks.front()));
                    selectedBackgroundTasks.erase(selectedBackgroundTasks.begin());
                }
                plannedTasks.insert(
                    plannedTasks.end(),
                    std::make_move_iterator(selectedBackgroundTasks.begin()),
                    std::make_move_iterator(selectedBackgroundTasks.end())
                );
            }
        }
    }

    // Coalesce only approved due tasks, keeping the earliest selected execution slot.
    // Retain original failure identities so batching cannot bypass their backoff.
    std::unordered_map<std::string, std::vector<ReadTask>> coalescedParts;
    if (!offlineProbeMode && config_.collect.batchOptimize && selectedTaskCoalescingHealthy_ &&
        taskFailureStates_.empty()) {
        for (std::size_t i = 0; i < plannedTasks.size(); ++i) {
            auto& task = plannedTasks[i];
            if (task.points.empty()) continue;
            std::vector<ReadTask> parts;
            bool extended;
            do {
                extended = false;
                for (std::size_t j = i + 1; j < plannedTasks.size(); ++j) {
                    auto& other = plannedTasks[j];
                    if (other.points.empty() || task.function != other.function) continue;
                    const auto start = std::min(task.start, other.start);
                    const auto end = std::max(task.start + task.count, other.start + other.count);
                    if (other.start > task.start + task.count || task.start > other.start + other.count ||
                        end - start > config_.collect.maxBatchRegisters) continue;
                    if (parts.empty()) parts.push_back(task);
                    parts.push_back(other);
                    task.points.insert(task.points.end(), std::make_move_iterator(other.points.begin()),
                                       std::make_move_iterator(other.points.end()));
                    other.points.clear();
                    task.start = start;
                    task.count = end - start;
                    for (auto& point : task.points) point.offset = point.definition.address - start;
                    extended = true;
                }
            } while (extended);
            if (!parts.empty()) coalescedParts.emplace(taskBackoffKey(task), std::move(parts));
        }
        plannedTasks.erase(std::remove_if(plannedTasks.begin(), plannedTasks.end(),
            [](const auto& task) { return task.points.empty(); }), plannedTasks.end());
    }

    int skippedTasks = 0;
    const auto logTaskFailure = [&](const ReadTask& task, const std::exception& ex, const char* action) {
        std::cerr << "collect task failed"
                  << " meter=" << config_.meterCode
                  << " slave=" << config_.protocol.slave
                  << " function=" << task.function
                  << " start=" << task.start
                  << " count=" << task.count
                  << " error=" << ex.what();
        if (action != nullptr && action[0] != '\0') {
            std::cerr << " action=" << action;
        }
        std::cerr << std::endl;
    };
    const auto decodeSuccessfulBlock = [&](const ReadTask& task, const std::vector<std::uint16_t>& block) {
        const auto taskNowMs = currentTimeMs();
        const auto completedMs = schedulingNowMs();
        for (const auto& taskPoint : task.points) {
            const auto& point = taskPoint.definition;
            const auto begin = block.begin() + taskPoint.offset;
            const auto end = begin + point.read.length;
            std::vector<std::uint16_t> slice(begin, end);

            try {
                const auto decoded = ModbusCodec::decodeReadValue(slice, point);
                auto value = buildPointValue(point, decoded, taskNowMs);
                lastReadMs_[point.index] = completedMs;
                lastValueUpdateMs_[point.index] = completedMs;
                if (point.read.cachePolicy.storeLatest) {
                    store_.putLatest(value);
                }
                pointFailureCounts_.erase(point.index);
                result.values.push_back(std::move(value));
                hasSuccessfulRead = true;
            } catch (const std::exception& ex) {
                std::cerr << "collect decode failed"
                          << " meter=" << config_.meterCode
                          << " slave=" << config_.protocol.slave
                          << " function=" << task.function
                          << " start=" << task.start
                          << " count=" << task.count
                          << " pointIndex=" << point.index
                          << " pointCode=" << point.pointCode
                          << " pointAddress=" << point.address
                          << " pointLength=" << point.read.length
                          << " error=" << ex.what()
                          << std::endl;
                recordFailure(point, ex.what());
            }
        }
    };
    const auto buildTaskFailureMessage = [](const ReadTask& task, const std::exception& ex) {
        return std::string("modbus read task failed function=") +
            std::to_string(task.function) +
            " start=" + std::to_string(task.start) +
            " count=" + std::to_string(task.count) +
            " error=" + ex.what();
    };
    int leafProbeBudgetRemaining = std::max(
        0,
        realtimeFocused
            ? config_.collect.realtimeAdaptiveSplitLeafProbeBudget
            : config_.collect.adaptiveSplitLeafProbeBudget
    );
    bool backgroundAttempted = false;
    std::function<bool(const ReadTask&, bool, int)> executeAdaptiveTask;
    executeAdaptiveTask = [&](const ReadTask& task, bool recordPointFailures, int depth) {
        if (shouldSkipTaskForBackoff(task, nowMs, realtimeFocused)) {
            if (canAdaptiveSplitTask(task) && depth < config_.collect.adaptiveSplitMaxDepth) {
                const auto state = taskFailureStates_.find(taskBackoffKey(task));
                const bool parentCanSplit = state != taskFailureStates_.end() &&
                    canAdaptiveSplitFailure(state->second.failureKind);
                if (parentCanSplit) {
                    auto splitTasks = splitReadTask(task);
                    if (splitTasks.size() == 1 && splitTasks.front().start == task.start &&
                        splitTasks.front().count == task.count &&
                        splitTasks.front().points.size() == task.points.size()) {
                        splitTasks = splitReadTaskByPoint(task);
                    }
                    if (splitTasks.size() > 1) {
                        splitTasks = limitAdaptiveSplitLeafProbeTasks(
                            task,
                            splitTasks,
                            leafProbeBudgetRemaining,
                            nowMs
                        );
                        for (const auto& splitTask : splitTasks) {
                            executeAdaptiveTask(splitTask, recordPointFailures, depth + 1);
                            if (result.workDeferred) break;
                        }
                        return true;
                    }
                }
            }
            ++skippedTasks;
            (void)recordPointFailures;
            return false;
        }
        // Yield only between eligible requests, including probes. Never interrupt I/O.
        const auto requestNowMs = schedulingNowMs();
        const bool backgroundPreempted = taskPriority(task) > 0 && backgroundAttempted && realtimeBecameDue(requestNowMs);
        if (!result.executedTasks.empty() &&
            (requestNowMs - nowMs >= timeSliceMs_ || backgroundPreempted)) {
            result.workDeferred = true;
            return false;
        }
        result.executedTasks.push_back(task);
        const bool hasBackground = std::any_of(task.points.begin(), task.points.end(),
            [this](const ReadTaskPoint& point) { return this->pointPriority(point.definition) > 0; });
        if (hasBackground) backgroundAttempted = true;
        std::vector<std::uint16_t> block;
        try {
            block = executeReadTask(task);
            if (hasBackground) lastBackgroundTaskMs_ = schedulingNowMs();
            if (block.size() < static_cast<std::size_t>(task.count)) {
                throw std::runtime_error("modbus block length is shorter than planned task");
            }
            noteTaskReadSuccess(task);
            decodeSuccessfulBlock(task, block);
        } catch (const std::exception& ex) {
            if (hasBackground) lastBackgroundTaskMs_ = schedulingNowMs();
            const auto messageText = std::string(ex.what());
            const auto failureKind = classifyModbusFailure(ex);
            const auto parts = coalescedParts.find(taskBackoffKey(task));
            if (parts != coalescedParts.end()) {
                // Fall back for this collector's lifetime, even if individual reads later recover.
                selectedTaskCoalescingHealthy_ = false;
                for (const auto& original : parts->second) {
                    noteTaskReadFailure(original, schedulingNowMs(), realtimeFocused, messageText, failureKind);
                }
                logTaskFailure(task, ex, "disable-selected-task-coalescing");
                if (recordPointFailures) {
                    const auto message = buildTaskFailureMessage(task, ex);
                    for (const auto& point : task.points) recordFailure(point.definition, message);
                }
                return true;
            }
            noteTaskReadFailure(task, schedulingNowMs(), realtimeFocused, messageText, failureKind);
            if (canAdaptiveSplitFailure(failureKind) && canAdaptiveSplitTask(task) &&
                depth < config_.collect.adaptiveSplitMaxDepth) {
                if (config_.collect.logAdaptiveSplitParentFailures) {
                    logTaskFailure(task, ex, "split");
                }
                auto splitTasks = splitReadTask(task);
                if (splitTasks.size() == 1 && splitTasks.front().start == task.start &&
                    splitTasks.front().count == task.count &&
                    splitTasks.front().points.size() == task.points.size()) {
                    splitTasks = splitReadTaskByPoint(task);
                }
                if (splitTasks.size() > 1) {
                    splitTasks = limitAdaptiveSplitLeafProbeTasks(
                        task,
                        splitTasks,
                        leafProbeBudgetRemaining,
                        nowMs
                    );
                    for (const auto& splitTask : splitTasks) {
                        executeAdaptiveTask(splitTask, recordPointFailures, depth + 1);
                        if (result.workDeferred) break;
                    }
                    return true;
                }
            }
            logTaskFailure(task, ex, "");
            if (recordPointFailures) {
                const auto message = buildTaskFailureMessage(task, ex);
                for (const auto& taskPoint : task.points) {
                    recordFailure(taskPoint.definition, message);
                }
            }
            return true;
        }
        return true;
    };

    for (const auto& task : plannedTasks) {
        executeAdaptiveTask(task, true, 0);
        if (result.workDeferred) break;
    }
    backgroundDeferred_ = backgroundSelected && !backgroundAttempted && result.workDeferred;

    // A completion-based interval can fall between the daemon's fixed ticks.
    const auto completedMs = schedulingNowMs();
    for (const auto& point : config_.points) {
        if (!point.enabled || !point.read.enable || point.read.dataType == "device_online") continue;
        const auto last = lastReadMs_.find(point.index);
        if (last == lastReadMs_.end()) continue;
        const auto delay = last->second + effectiveIntervalMs(point) - completedMs;
        if (delay > 0 && delay <= std::numeric_limits<int>::max()) {
            result.nextDelayMs = result.nextDelayMs > 0
                ? std::min(result.nextDelayMs, static_cast<int>(delay)) : static_cast<int>(delay);
        }
    }

    if (result.executedTasks.empty() && skippedTasks > 0) {
        return result;
    }

    if (!result.executedTasks.empty()) {
        if (hasSuccessfulRead) {
            noteReadSuccess(schedulingNowMs());
        } else {
            noteReadFailure(schedulingNowMs());
        }
        publishDeviceOnlineStatus(currentOnline(), schedulingNowMs());
    }

    publishTelemetryBestEffort(result.values);
    if (!result.workDeferred && !result.executedTasks.empty() && !hasSuccessfulRead && !firstFailureMessage.empty()) {
        throw std::runtime_error(firstFailureMessage);
    }
    return result;
}

bool Collector::shouldSkipForBackoff(std::int64_t nowMs) const {
    if (config_.collect.cycleBackoffEnabled) {
        return slaveNextAttemptCycle_ > collectCycle_;
    }
    return backoffUntilMs_ > 0 && nowMs < backoffUntilMs_;
}

void Collector::noteReadSuccess(std::int64_t nowMs) {
    (void)nowMs;
    consecutiveFailures_ = 0;
    slaveNextAttemptCycle_ = 0;
    backoffUntilMs_ = 0;
    ++consecutiveSuccesses_;
    if (consecutiveSuccesses_ >= std::max(1, config_.collect.recoverySuccessThreshold)) {
        online_ = true;
    }
}

void Collector::noteReadFailure(std::int64_t nowMs) {
    consecutiveSuccesses_ = 0;
    ++consecutiveFailures_;
    if (consecutiveFailures_ >= std::max(1, config_.collect.offlineFailureThreshold)) {
        online_ = false;
    }
    if (consecutiveFailures_ >= std::max(1, config_.collect.slaveFailureBackoffThreshold)) {
        if (config_.collect.cycleBackoffEnabled) {
            const auto skipCycles = std::max(0, config_.collect.slaveFailureBackoffCycles);
            slaveNextAttemptCycle_ = collectCycle_ + static_cast<std::int64_t>(skipCycles) + 1;
        } else if (config_.collect.slaveFailureBackoffMs > 0) {
            backoffUntilMs_ = nowMs + config_.collect.slaveFailureBackoffMs;
        }
    }
}

bool Collector::currentOnline() const {
    return online_;
}

bool Collector::shouldSkipTaskForBackoff(const ReadTask& task, std::int64_t nowMs, bool realtimeFocused) const {
    if (taskFailureStates_.empty()) return false;
    const auto it = taskFailureStates_.find(taskBackoffKey(task));
    if (it == taskFailureStates_.end()) {
        return false;
    }
    if (config_.collect.cycleBackoffEnabled) {
        return it->second.nextAttemptCycle > collectCycle_;
    }
    if (!realtimeFocused) {
        return it->second.backoffUntilMs > nowMs;
    }
    const auto realtimeBackoffMs = std::max(0, config_.collect.realtimeTaskFailureBackoffMs);
    if (realtimeBackoffMs <= 0 || it->second.lastFailureMs <= 0) {
        return false;
    }
    return nowMs - it->second.lastFailureMs < realtimeBackoffMs;
}

void Collector::noteTaskReadSuccess(const ReadTask& task) {
    clearOverlappingTaskFailures(task);
}

void Collector::noteTaskReadFailure(
    const ReadTask& task,
    std::int64_t nowMs,
    bool realtimeFocused,
    const std::string& message,
    ModbusFailureKind failureKind
) {
    auto& state = taskFailureStates_[taskBackoffKey(task)];
    ++state.consecutiveFailures;
    state.lastFailureMs = nowMs;
    state.failureKind = failureKind;
    state.lastMessage = std::string("modbus read task is in backoff function=") +
        std::to_string(task.function) +
        " start=" + std::to_string(task.start) +
        " count=" + std::to_string(task.count);
    if (state.consecutiveFailures >= std::max(1, config_.collect.taskFailureBackoffThreshold)) {
        if (config_.collect.cycleBackoffEnabled) {
            const auto skipCycles = taskFailureBackoffCycles(state.consecutiveFailures, realtimeFocused);
            state.nextAttemptCycle = collectCycle_ + static_cast<std::int64_t>(skipCycles) + 1;
        } else {
            state.backoffUntilMs = nowMs + taskFailureBackoffDurationMs(state.consecutiveFailures, realtimeFocused);
        }
    }
}

int Collector::taskFailureBackoffCycles(int consecutiveFailures, bool realtimeFocused) const {
    const auto baseCycles = std::max(
        0,
        realtimeFocused
            ? config_.collect.realtimeTaskFailureBackoffCycles
            : config_.collect.taskFailureBackoffCycles
    );
    if (baseCycles <= 0) {
        return 0;
    }
    const auto maxCycles = std::max(
        baseCycles,
        realtimeFocused
            ? config_.collect.realtimeTaskFailureBackoffMaxCycles
            : config_.collect.taskFailureBackoffMaxCycles
    );
    const auto threshold = std::max(1, config_.collect.taskFailureBackoffThreshold);
    long long cycles = baseCycles;
    const auto extraFailures = std::max(0, consecutiveFailures - threshold);
    for (int i = 0; i < extraFailures && cycles < maxCycles; ++i) {
        cycles = std::min<long long>(static_cast<long long>(maxCycles), cycles * 2LL);
    }
    return static_cast<int>(cycles);
}

int Collector::taskFailureBackoffDurationMs(int consecutiveFailures, bool realtimeFocused) const {
    const auto baseMs = std::max(
        0,
        realtimeFocused
            ? config_.collect.realtimeTaskFailureBackoffMs
            : config_.collect.taskFailureBackoffMs
    );
    const auto maxMs = std::max(
        baseMs,
        realtimeFocused
            ? config_.collect.realtimeTaskFailureBackoffMaxMs
            : config_.collect.taskFailureBackoffMaxMs
    );
    const auto threshold = std::max(1, config_.collect.taskFailureBackoffThreshold);
    long long duration = baseMs;
    const auto extraFailures = std::max(0, consecutiveFailures - threshold);
    for (int i = 0; i < extraFailures && duration < maxMs; ++i) {
        duration = std::min<long long>(static_cast<long long>(maxMs), duration * 2LL);
    }
    return static_cast<int>(duration);
}

void Collector::clearOverlappingTaskFailures(const ReadTask& task) {
    for (auto it = taskFailureStates_.begin(); it != taskFailureStates_.end();) {
        const auto parsed = parseTaskBackoffKey(it->first);
        if (parsed.valid &&
            parsed.function == task.function &&
            rangeCovers(task.start, task.count, parsed.start, parsed.count)) {
            it = taskFailureStates_.erase(it);
            continue;
        }
        ++it;
    }
}

std::string Collector::taskBackoffKey(const ReadTask& task) const {
    return std::to_string(task.function) + ":" +
        std::to_string(task.start) + ":" +
        std::to_string(task.count);
}

std::string Collector::taskFailureMessage(const ReadTask& task) const {
    const auto it = taskFailureStates_.find(taskBackoffKey(task));
    if (it != taskFailureStates_.end() && !it->second.lastMessage.empty()) {
        return it->second.lastMessage;
    }
    return std::string("modbus read task is in backoff function=") +
        std::to_string(task.function) +
        " start=" + std::to_string(task.start) +
        " count=" + std::to_string(task.count);
}

bool Collector::canAdaptiveSplitTask(const ReadTask& task) const {
    return config_.collect.adaptiveSplitOnFailure && task.count > 1 && task.points.size() > 1;
}

std::vector<ReadTask> Collector::splitReadTask(const ReadTask& task) const {
    std::vector<ReadTask> splitTasks;
    splitTasks.reserve(task.points.size());
    const auto maxRegisters = std::max(1, config_.collect.adaptiveSplitMaxRegisters);
    for (const auto& taskPoint : task.points) {
        const auto& point = taskPoint.definition;
        if (!splitTasks.empty()) {
            auto& current = splitTasks.back();
            const auto nextEnd = std::max(
                current.start + current.count,
                point.address + point.read.length
            );
            if (current.function == task.function &&
                point.address >= current.start &&
                nextEnd - current.start <= maxRegisters) {
                current.points.push_back(ReadTaskPoint{point, point.address - current.start});
                current.count = nextEnd - current.start;
                continue;
            }
        }

        ReadTask splitTask;
        splitTask.function = task.function;
        splitTask.start = point.address;
        splitTask.count = point.read.length;
        splitTask.points.push_back(ReadTaskPoint{point, 0});
        splitTasks.push_back(std::move(splitTask));
    }
    return splitTasks;
}

std::vector<ReadTask> Collector::splitReadTaskByPoint(const ReadTask& task) const {
    std::vector<ReadTask> splitTasks;
    splitTasks.reserve(task.points.size());
    for (const auto& taskPoint : task.points) {
        const auto& point = taskPoint.definition;
        ReadTask splitTask;
        splitTask.function = task.function;
        splitTask.start = point.address;
        splitTask.count = point.read.length;
        splitTask.points.push_back(ReadTaskPoint{point, 0});
        splitTasks.push_back(std::move(splitTask));
    }
    return splitTasks;
}

std::vector<ReadTask> Collector::limitAdaptiveSplitLeafProbeTasks(
    const ReadTask& parentTask,
    const std::vector<ReadTask>& tasks,
    int& leafProbeBudgetRemaining,
    std::int64_t nowMs
) const {
    if (tasks.empty() || leafProbeBudgetRemaining <= 0) {
        return {};
    }
    const bool allLeafTasks = std::all_of(tasks.begin(), tasks.end(), [](const ReadTask& task) {
        return task.points.size() == 1;
    });
    if (!allLeafTasks) {
        return tasks;
    }

    const auto probeCount = std::min<std::size_t>(
        static_cast<std::size_t>(leafProbeBudgetRemaining),
        tasks.size()
    );
    leafProbeBudgetRemaining -= static_cast<int>(probeCount);
    if (probeCount >= tasks.size()) {
        return tasks;
    }

    std::vector<std::size_t> order(tasks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto lhsBackedOff = shouldSkipTaskForBackoff(tasks[lhs], nowMs);
        const auto rhsBackedOff = shouldSkipTaskForBackoff(tasks[rhs], nowMs);
        if (lhsBackedOff != rhsBackedOff) {
            return !lhsBackedOff && rhsBackedOff;
        }
        const auto lhsOldest = taskOldestValueUpdateMs(tasks[lhs]);
        const auto rhsOldest = taskOldestValueUpdateMs(tasks[rhs]);
        if (lhsOldest != rhsOldest) {
            return lhsOldest < rhsOldest;
        }
        return lhs < rhs;
    });

    std::vector<ReadTask> limited;
    limited.reserve(probeCount);
    for (std::size_t i = 0; i < probeCount; ++i) {
        limited.push_back(tasks[order[i]]);
    }
    (void)parentTask;
    return limited;
}

std::vector<ReadTask> Collector::expandBackedOffTasksBeforeBudget(
    const std::vector<ReadTask>& tasks,
    std::int64_t nowMs,
    bool realtimeFocused
) const {
    if (!config_.collect.adaptiveSplitOnFailure || tasks.empty()) {
        return tasks;
    }

    std::vector<ReadTask> expanded;
    expanded.reserve(tasks.size());
    for (const auto& task : tasks) {
        if (!shouldSkipTaskForBackoff(task, nowMs, realtimeFocused) || !canAdaptiveSplitTask(task)) {
            expanded.push_back(task);
            continue;
        }
        const auto state = taskFailureStates_.find(taskBackoffKey(task));
        if (state == taskFailureStates_.end() ||
            !canAdaptiveSplitFailure(state->second.failureKind)) {
            expanded.push_back(task);
            continue;
        }

        auto splitTasks = splitReadTask(task);
        if (splitTasks.size() == 1 && splitTasks.front().start == task.start &&
            splitTasks.front().count == task.count &&
            splitTasks.front().points.size() == task.points.size()) {
            splitTasks = splitReadTaskByPoint(task);
        }
        if (splitTasks.size() <= 1) {
            expanded.push_back(task);
            continue;
        }
        expanded.insert(expanded.end(), splitTasks.begin(), splitTasks.end());
    }
    return expanded;
}

std::vector<ReadTask> Collector::limitToOfflineProbeTasks(const std::vector<ReadTask>& tasks) const {
    if (!config_.collect.offlineProbeOnly || online_ || consecutiveFailures_ == 0 ||
        static_cast<int>(tasks.size()) <= config_.collect.offlineProbeTaskCount) {
        return tasks;
    }
    std::vector<ReadTask> probes;
    const auto probeCount = std::min<std::size_t>(
        static_cast<std::size_t>(config_.collect.offlineProbeTaskCount),
        tasks.size()
    );
    probes.reserve(probeCount);
    for (std::size_t i = 0; i < probeCount; ++i) {
        probes.push_back(tasks[(offlineProbeCursor_ + i) % tasks.size()]);
    }
    offlineProbeCursor_ = (offlineProbeCursor_ + probeCount) % tasks.size();
    return probes;
}

std::vector<ReadTask> Collector::limitToMeterTaskBudget(
    std::vector<ReadTask> tasks,
    std::int64_t nowMs,
    int maxTasksPerCycle,
    bool realtimeFocused
) const {
    if (tasks.empty() || maxTasksPerCycle <= 0) {
        return {};
    }
    const auto budget = std::min<std::size_t>(
        static_cast<std::size_t>(maxTasksPerCycle),
        tasks.size()
    );
    using Rank = std::tuple<bool, int, std::int64_t, std::size_t>;
    std::vector<Rank> order;
    order.reserve(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        order.emplace_back(shouldSkipTaskForBackoff(tasks[i], nowMs, realtimeFocused),
                           taskPriority(tasks[i]), taskOldestValueUpdateMs(tasks[i]), i);
    }
    std::partial_sort(order.begin(), order.begin() + budget, order.end());

    std::vector<ReadTask> limited;
    limited.reserve(budget);
    for (std::size_t i = 0; i < budget; ++i) {
        limited.push_back(std::move(tasks[std::get<3>(order[i])]));
    }
    return limited;
}

std::int64_t Collector::taskOldestValueUpdateMs(const ReadTask& task) const {
    std::int64_t oldest = std::numeric_limits<std::int64_t>::max();
    for (const auto& taskPoint : task.points) {
        const auto it = lastValueUpdateMs_.find(taskPoint.definition.index);
        if (it == lastValueUpdateMs_.end()) {
            return std::numeric_limits<std::int64_t>::min();
        }
        oldest = std::min(oldest, it->second);
    }
    return oldest == std::numeric_limits<std::int64_t>::max()
        ? std::numeric_limits<std::int64_t>::min()
        : oldest;
}

int Collector::pointPriority(const PointDefinition& point) const {
    if (point.read.dataType == "device_online") {
        return 0;
    }
    return std::max(0, point.collectPriority);
}

int Collector::taskPriority(const ReadTask& task) const {
    int priority = std::numeric_limits<int>::max();
    for (const auto& taskPoint : task.points) {
        priority = std::min(priority, pointPriority(taskPoint.definition));
    }
    return priority == std::numeric_limits<int>::max() ? 0 : priority;
}

std::vector<std::uint16_t> Collector::executeReadTask(const ReadTask& task) const {
    switch (task.function) {
        case 1:
            return modbusClient_->readCoils(config_.protocol.slave, task.start, task.count);
        case 2:
            return modbusClient_->readDiscreteInputs(config_.protocol.slave, task.start, task.count);
        case 3:
            return modbusClient_->readHoldingRegisters(config_.protocol.slave, task.start, task.count);
        case 4:
            return modbusClient_->readInputRegisters(config_.protocol.slave, task.start, task.count);
        default:
            throw std::invalid_argument("unsupported read function: " + std::to_string(task.function));
    }
}

}  // namespace edge_gateway
