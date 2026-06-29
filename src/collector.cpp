#include "edge_gateway/modbus_collector.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
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

bool isNoResponseError(const std::string& message) {
    return message.find("response timeout") != std::string::npos ||
        message.find("no response") != std::string::npos;
}

}  // namespace

Collector::Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    modbusClient_(std::move(modbusClient)) {
    if (!modbusClient_) {
        throw std::invalid_argument("modbusClient is required");
    }
}

CollectCycleResult Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    CollectCycleResult result;
    ++collectCycle_;
    if (shouldSkipForBackoff(nowMs)) {
        publishDeviceOnlineStatus(currentOnline(), nowMs);
        return result;
    }
    const auto points = duePoints(nowMs);
    std::vector<PointDefinition> realtimePoints;
    std::vector<PointDefinition> backgroundPoints;
    realtimePoints.reserve(points.size());
    backgroundPoints.reserve(points.size());
    for (const auto& point : points) {
        if (pointPriority(point) == 0) {
            realtimePoints.push_back(point);
        } else {
            backgroundPoints.push_back(point);
        }
    }
    const auto buildPlannedTasks = [&](const std::vector<PointDefinition>& sourcePoints) {
        auto tasks = ReadTaskPlanner::build(
            sourcePoints,
            config_.collect.maxBatchRegisters,
            config_.collect.batchOptimize
        );
        tasks = splitMixedPriorityTasks(tasks);
        return expandBackedOffTasksBeforeBudget(tasks, nowMs, realtimeFocused);
    };
    auto realtimeTasks = buildPlannedTasks(realtimePoints);
    auto backgroundTasks = buildPlannedTasks(backgroundPoints);

    std::vector<ReadTask> candidateTasks;
    candidateTasks.reserve(realtimeTasks.size() + backgroundTasks.size());
    candidateTasks.insert(candidateTasks.end(), realtimeTasks.begin(), realtimeTasks.end());
    candidateTasks.insert(candidateTasks.end(), backgroundTasks.begin(), backgroundTasks.end());

    bool hasSuccessfulRead = false;
    std::string firstFailureMessage;
    const auto recordFailure = [&](const PointDefinition& point, const std::string& message) {
        if (firstFailureMessage.empty()) {
            firstFailureMessage = message;
        }
        auto value = buildFailedPointValue(point, message, nowMs);
        lastReadMs_[point.index] = nowMs;
        const auto failureCount = ++pointFailureCounts_[point.index];
        bool valueUpdated = !point.read.cachePolicy.storeLatest;
        if (point.read.cachePolicy.storeLatest) {
            const auto previous = store_.getLatestByIndex(point.index, nowMs);
            const auto lastUpdate = lastValueUpdateMs_.find(point.index);
            const auto previousUpdateMs = lastUpdate == lastValueUpdateMs_.end()
                ? previous ? previous->ts : 0
                : lastUpdate->second;
            const auto graceMs = std::max(0, config_.collect.failureGoodValueGraceMs);
            const auto badQualityThreshold = std::max(1, config_.collect.failureBadQualityThreshold);
            const bool keepGoodValue = previous && previous->quality == 1 && !previous->stale &&
                (failureCount < badQualityThreshold ||
                 (graceMs > 0 && nowMs - previousUpdateMs < graceMs));
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
            lastValueUpdateMs_[point.index] = nowMs;
            result.values.push_back(std::move(value));
        }
    };

    std::vector<ReadTask> plannedTasks;
    const bool offlineProbeMode = config_.collect.offlineProbeOnly && !online_ && consecutiveFailures_ > 0 &&
        static_cast<int>(candidateTasks.size()) > config_.collect.offlineProbeTaskCount;
    if (offlineProbeMode) {
        plannedTasks = limitToOfflineProbeTasks(candidateTasks);
        const auto realtimeTaskBudget = realtimeFocused
            ? config_.collect.realtimeMaxTasksPerMeterPerCycle
            : config_.collect.maxTasksPerMeterPerCycle;
        plannedTasks = limitToMeterTaskBudget(
            plannedTasks,
            nowMs,
            realtimeTaskBudget,
            realtimeFocused
        );
    } else {
        const auto realtimeTaskBudget = realtimeFocused
            ? config_.collect.realtimeMaxTasksPerMeterPerCycle
            : config_.collect.maxTasksPerMeterPerCycle;
        auto selectedRealtimeTasks = limitToMeterTaskBudget(
            realtimeTasks,
            nowMs,
            realtimeTaskBudget,
            realtimeFocused
        );
        plannedTasks.insert(
            plannedTasks.end(),
            selectedRealtimeTasks.begin(),
            selectedRealtimeTasks.end()
        );

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
                backgroundTasks,
                nowMs,
                backgroundBudget,
                realtimeFocused
            );
            if (!selectedBackgroundTasks.empty()) {
                lastBackgroundTaskMs_ = nowMs;
                plannedTasks.insert(
                    plannedTasks.end(),
                    selectedBackgroundTasks.begin(),
                    selectedBackgroundTasks.end()
                );
            }
        }
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
        for (const auto& taskPoint : task.points) {
            const auto& point = taskPoint.definition;
            const auto begin = block.begin() + taskPoint.offset;
            const auto end = begin + point.read.length;
            std::vector<std::uint16_t> slice(begin, end);

            try {
                const auto decoded = ModbusCodec::decodeReadValue(slice, point);
                auto value = buildPointValue(point, decoded, taskNowMs);
                lastReadMs_[point.index] = nowMs;
                lastValueUpdateMs_[point.index] = nowMs;
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
    std::function<bool(const ReadTask&, bool, int)> executeAdaptiveTask;
    executeAdaptiveTask = [&](const ReadTask& task, bool recordPointFailures, int depth) {
        if (shouldSkipTaskForBackoff(task, nowMs, realtimeFocused)) {
            if (config_.collect.cycleBackoffEnabled) {
                ++skippedTasks;
                (void)recordPointFailures;
                return false;
            }
            if (canAdaptiveSplitTask(task) && depth < config_.collect.adaptiveSplitMaxDepth) {
                const auto state = taskFailureStates_.find(taskBackoffKey(task));
                const bool parentNoResponse = state != taskFailureStates_.end() && state->second.noResponse;
                if (!parentNoResponse) {
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
                        }
                        return true;
                    }
                }
            }
            ++skippedTasks;
            (void)recordPointFailures;
            return false;
        }
        result.executedTasks.push_back(task);
        std::vector<std::uint16_t> block;
        try {
            block = executeReadTask(task);
            if (block.size() < static_cast<std::size_t>(task.count)) {
                throw std::runtime_error("modbus block length is shorter than planned task");
            }
            noteTaskReadSuccess(task);
            decodeSuccessfulBlock(task, block);
        } catch (const std::exception& ex) {
            const auto messageText = std::string(ex.what());
            noteTaskReadFailure(task, nowMs, realtimeFocused, messageText);
            if (!isNoResponseError(messageText) && canAdaptiveSplitTask(task) &&
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
    }

    if (result.executedTasks.empty() && skippedTasks > 0) {
        publishDeviceOnlineStatus(currentOnline(), nowMs);
        return result;
    }

    if (!result.executedTasks.empty()) {
        if (hasSuccessfulRead) {
            noteReadSuccess(nowMs);
        } else {
            noteReadFailure(nowMs);
        }
        publishDeviceOnlineStatus(currentOnline(), nowMs);
    }

    if (mqttPublisher_ && !result.values.empty()) {
        mqttPublisher_->publishTelemetry(config_.machineCode, result.values);
    }
    if (!result.executedTasks.empty() && !hasSuccessfulRead && !firstFailureMessage.empty()) {
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
    const std::string& message
) {
    auto& state = taskFailureStates_[taskBackoffKey(task)];
    ++state.consecutiveFailures;
    state.lastFailureMs = nowMs;
    state.noResponse = isNoResponseError(message);
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
        if (config_.collect.cycleBackoffEnabled) {
            expanded.push_back(task);
            continue;
        }

        const auto state = taskFailureStates_.find(taskBackoffKey(task));
        if (state != taskFailureStates_.end() && state->second.noResponse) {
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

std::vector<ReadTask> Collector::splitMixedPriorityTasks(const std::vector<ReadTask>& tasks) const {
    if (tasks.empty()) {
        return tasks;
    }

    std::vector<ReadTask> split;
    split.reserve(tasks.size());
    for (const auto& task : tasks) {
        bool hasHighPriority = false;
        bool hasLowPriority = false;
        for (const auto& taskPoint : task.points) {
            if (pointPriority(taskPoint.definition) == 0) {
                hasHighPriority = true;
            } else {
                hasLowPriority = true;
            }
        }

        if (!hasHighPriority || !hasLowPriority) {
            split.push_back(task);
            continue;
        }

        auto pointTasks = splitReadTaskByPoint(task);
        split.insert(split.end(), pointTasks.begin(), pointTasks.end());
    }
    return split;
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
    const std::vector<ReadTask>& tasks,
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
    if (budget >= tasks.size()) {
        return tasks;
    }

    std::vector<std::size_t> order(tasks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto lhsBackedOff = shouldSkipTaskForBackoff(tasks[lhs], nowMs, realtimeFocused);
        const auto rhsBackedOff = shouldSkipTaskForBackoff(tasks[rhs], nowMs, realtimeFocused);
        if (lhsBackedOff != rhsBackedOff) {
            return !lhsBackedOff && rhsBackedOff;
        }
        const auto lhsPriority = taskPriority(tasks[lhs]);
        const auto rhsPriority = taskPriority(tasks[rhs]);
        if (lhsPriority != rhsPriority) {
            return lhsPriority < rhsPriority;
        }
        const auto lhsOldest = taskOldestValueUpdateMs(tasks[lhs]);
        const auto rhsOldest = taskOldestValueUpdateMs(tasks[rhs]);
        if (lhsOldest != rhsOldest) {
            return lhsOldest < rhsOldest;
        }
        return lhs < rhs;
    });

    std::vector<ReadTask> limited;
    limited.reserve(budget);
    for (const auto index : order) {
        if (limited.size() >= budget) {
            break;
        }
        limited.push_back(tasks[index]);
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
