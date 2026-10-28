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
#include <unordered_set>
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

void ModbusLineSchedule::update(const void* collector, int slave, std::int64_t realtimeDueMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = entries_[collector];
    entry.slave = slave;
    entry.realtimeDueMs = realtimeDueMs;
}

bool ModbusLineSchedule::realtimeWaitingElsewhere(
    const void* collector, std::int64_t nowMs, const IModbusClient& client) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.first != collector && entry.second.realtimeDueMs <= nowMs &&
            client.requestReadyInMs(entry.second.slave) == 0) {
            return true;
        }
    }
    return false;
}

bool ModbusLineSchedule::earlierRealtimeElsewhere(
    const void* collector, std::int64_t dueMs, std::int64_t nowMs, const IModbusClient& client) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.first != collector && entry.second.realtimeDueMs < dueMs && entry.second.realtimeDueMs <= nowMs &&
            client.requestReadyInMs(entry.second.slave) == 0) {
            return true;
        }
    }
    return false;
}

Collector::Collector(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IModbusClient> modbusClient,
    std::shared_ptr<IMqttPublisher> mqttPublisher,
    std::function<std::int64_t()> monotonicClockMs,
    std::shared_ptr<ModbusLineSchedule> lineSchedule
) : CollectorBase(std::move(config), store, std::move(mqttPublisher)),
    modbusClient_(std::move(modbusClient)),
    monotonicClockMs_(std::move(monotonicClockMs)),
    lineSchedule_(std::move(lineSchedule)) {
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

void Collector::onSchedulingClockStepped(std::int64_t deltaMs) {
    const auto shift = [deltaMs](std::int64_t& value) { if (value > 0) value += deltaMs; };
    shift(offlineProbeNextAttemptMs_);
    shift(backoffUntilMs_);
    shift(lastBackgroundRequestMs_);
    for (auto& entry : taskFailureStates_) {
        shift(entry.second.backoffUntilMs);
        shift(entry.second.offlineProbeUntilMs);
        shift(entry.second.lastFailureMs);
    }
}

bool Collector::deviceOnline() const {
    return online_;
}

CollectCycleResult Collector::collectOnce(std::int64_t nowMs, bool realtimeFocused) {
    observeSchedulingClock(nowMs);
    CollectCycleResult result;
    const auto slave = config_.protocol.slave;
    // Another meter on this bus can use the wait for this slave's frame interval.
    const auto initialReadyInMs = modbusClient_->requestReadyInMs(slave);
    if (initialReadyInMs > 0) {
        result.nextDelayMs = initialReadyInMs;
        return result;
    }
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
        if (lineSchedule_) lineSchedule_->update(this, slave, std::numeric_limits<std::int64_t>::max());
        return result;
    }
    // Every point is due on its own intervalMs, counted from the completion of its last read.
    // Realtime (collectPriority 0) and background points are planned into separate requests: on a
    // busy line, background registers must not widen every realtime read.
    std::vector<const PointDefinition*> realtimePoints;
    std::vector<const PointDefinition*> backgroundPoints;
    for (const auto index : readablePointOrder_) {
        const auto& point = config_.points[index];
        const auto last = lastReadMs_.find(point.index);
        if (last != lastReadMs_.end() && nowMs - last->second < effectiveIntervalMs(point)) continue;
        (pointPriority(point) == 0 ? realtimePoints : backgroundPoints).push_back(&point);
    }

    auto tasks = ReadTaskPlanner::buildSorted(
        realtimePoints,
        config_.collect.maxBatchRegisters,
        config_.collect.batchOptimize,
        config_.collect.maxGapRegisters
    );
    auto backgroundTasks = ReadTaskPlanner::buildSorted(
        backgroundPoints,
        config_.collect.maxBatchRegisters,
        config_.collect.batchOptimize,
        config_.collect.maxGapRegisters
    );
    tasks.insert(tasks.end(), std::make_move_iterator(backgroundTasks.begin()),
                 std::make_move_iterator(backgroundTasks.end()));
    if (!taskFailureStates_.empty()) {
        tasks = expandBackedOffTasks(tasks, nowMs, realtimeFocused);
    }

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

    const bool offlineProbeMode = config_.collect.offlineProbeOnly && !online_ && consecutiveFailures_ > 0 &&
        static_cast<int>(tasks.size()) > config_.collect.offlineProbeTaskCount;
    auto plannedTasks = orderTasksByDeadline(
        offlineProbeMode ? limitToOfflineProbeTasks(tasks) : std::move(tasks),
        nowMs,
        realtimeFocused
    );
    // A turn ends after this many requests so the next meter on the bus is served; due work that is
    // left over continues on the following turn without waiting for the daemon's interval.
    auto turnQuantum = static_cast<std::size_t>(std::max(1,
        realtimeFocused ? config_.collect.realtimeMaxTasksPerMeterPerCycle
                        : config_.collect.maxTasksPerMeterPerCycle));
    // Background points (collectPriority > 0) use idle line time: they run after this meter's realtime
    // reads only while no other meter on the line has realtime work. While realtime work is pending
    // they still get one read per backgroundTaskIntervalMs (the share) so they are never starved; the
    // share opens the turn because a slave with frameIntervalMs serves one request per turn.
    if (lastBackgroundRequestMs_ < 0) {
        lastBackgroundRequestMs_ = nowMs;
    }
    const bool realtimeFirst = !plannedTasks.empty() && taskPriority(plannedTasks.front()) == 0;
    const bool lineBusy = lineSchedule_ && lineSchedule_->realtimeWaitingElsewhere(this, nowMs, *modbusClient_);
    if (realtimeFirst || lineBusy) {
        const auto background = std::find_if(plannedTasks.begin(), plannedTasks.end(), [&](const ReadTask& task) {
            return taskPriority(task) > 0 && !shouldSkipTaskForBackoff(task, nowMs, realtimeFocused);
        });
        const bool share = background != plannedTasks.end() &&
            nowMs - lastBackgroundRequestMs_ >= std::max(0, config_.collect.backgroundTaskIntervalMs);
        if (share) {
            std::rotate(plannedTasks.begin(), background, background + 1);
            ++turnQuantum;
        }
        if (lineBusy) {
            plannedTasks.erase(std::remove_if(plannedTasks.begin() + (share ? 1 : 0), plannedTasks.end(),
                [this](const ReadTask& task) { return taskPriority(task) > 0; }), plannedTasks.end());
        }
    }
    coalesceTurnTasks(plannedTasks, std::min(plannedTasks.size(), turnQuantum), nowMs, realtimeFocused);

    int skippedTasks = 0;
    // Set when the turn stopped in the middle of its work (time slice, realtime preemption, busy
    // slave): its failures say nothing about the meter as a whole.
    bool turnInterrupted = false;
    bool slaveBusy = false;
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
                            if (result.workDeferred || slaveBusy) break;
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
        if (!result.executedTasks.empty() && (requestNowMs - nowMs >= timeSliceMs_ || backgroundPreempted)) {
            result.workDeferred = true;
            turnInterrupted = true;
            return false;
        }
        const auto readyInMs = modbusClient_->requestReadyInMs(slave);
        if (readyInMs > 0) {
            slaveBusy = true;
            result.nextDelayMs = result.nextDelayMs > 0 ? std::min(result.nextDelayMs, readyInMs) : readyInMs;
            return false;
        }
        result.executedTasks.push_back(task);
        const bool hasBackground = std::any_of(task.points.begin(), task.points.end(),
            [this](const ReadTaskPoint& point) { return this->pointPriority(point.definition) > 0; });
        if (hasBackground) {
            backgroundAttempted = true;
            lastBackgroundRequestMs_ = requestNowMs;
        }
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
            const auto failureKind = classifyModbusFailure(ex);
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
                        if (result.workDeferred || slaveBusy) break;
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

    // Points of backed-off tasks cannot be read now; they must not make other meters wait.
    std::unordered_set<std::uint32_t> heldPoints;
    for (const auto& task : plannedTasks) {
        if (shouldSkipTaskForBackoff(task, nowMs, realtimeFocused)) {
            for (const auto& point : task.points) heldPoints.insert(point.definition.index);
        }
    }
    std::size_t turnsUsed = 0;
    for (const auto& task : plannedTasks) {
        if (turnsUsed >= turnQuantum) {
            // Backed-off tasks are ordered last; only runnable work justifies an immediate next turn.
            if (!shouldSkipTaskForBackoff(task, nowMs, realtimeFocused)) {
                result.workDeferred = true;
            }
            break;
        }
        // Earliest deadline first across the line: give way to a meter whose realtime work is more
        // overdue (a meter watched through the realtime lease keeps its turn).
        if (lineSchedule_ && !realtimeFocused && taskPriority(task) == 0 &&
            !shouldSkipTaskForBackoff(task, nowMs, realtimeFocused) &&
            lineSchedule_->earlierRealtimeElsewhere(this, taskDueMs(task), schedulingNowMs(), *modbusClient_)) {
            result.workDeferred = true;
            turnInterrupted = true;
            break;
        }
        const auto executedBefore = result.executedTasks.size();
        executeAdaptiveTask(task, true, 0);
        if (result.executedTasks.size() > executedBefore) {
            ++turnsUsed;
        }
        if (result.workDeferred || slaveBusy) break;
    }

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

    if (lineSchedule_) {
        // A meter that is not online is only probed; it must not hold back the line's background reads.
        lineSchedule_->update(this, slave,
            online_ ? earliestRealtimeDueMs(heldPoints) : std::numeric_limits<std::int64_t>::max());
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
    // A turn cut short mid-work is not a verdict on the whole meter; a full turn (quantum used) whose
    // every request failed is reported.
    if (!turnInterrupted && !slaveBusy && !result.executedTasks.empty() && !hasSuccessfulRead &&
        !firstFailureMessage.empty()) {
        throw std::runtime_error(firstFailureMessage);
    }
    return result;
}

bool Collector::shouldSkipForBackoff(std::int64_t nowMs) const {
    if (config_.collect.cycleBackoffEnabled) {
        if (slaveNextAttemptCycle_ > collectCycle_) {
            return true;
        }
    } else if (backoffUntilMs_ > 0 && nowMs < backoffUntilMs_) {
        return true;
    }
    return config_.collect.offlineProbeOnly && !online_ && consecutiveFailures_ > 0 &&
        offlineProbeNextAttemptMs_ > 0 && nowMs < offlineProbeNextAttemptMs_;
}

void Collector::noteReadSuccess(std::int64_t nowMs) {
    (void)nowMs;
    consecutiveFailures_ = 0;
    slaveNextAttemptCycle_ = 0;
    backoffUntilMs_ = 0;
    offlineProbeNextAttemptMs_ = 0;
    ++consecutiveSuccesses_;
    if (consecutiveSuccesses_ >= std::max(1, config_.collect.recoverySuccessThreshold)) {
        online_ = true;
    }
}

void Collector::noteReadFailure(std::int64_t nowMs) {
    consecutiveSuccesses_ = 0;
    ++consecutiveFailures_;
    const auto offlineFailureThreshold = std::max(1, config_.collect.offlineFailureThreshold);
    if (consecutiveFailures_ >= offlineFailureThreshold) {
        online_ = false;
        if (config_.collect.offlineProbeOnly && config_.collect.offlineProbeIntervalMs > 0) {
            offlineProbeNextAttemptMs_ = nowMs + config_.collect.offlineProbeIntervalMs;
        }
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
    if (config_.collect.offlineProbeOnly &&
        it->second.offlineProbeUntilMs > 0 && nowMs < it->second.offlineProbeUntilMs) {
        return true;
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
    const auto offlineProbeThreshold = std::max(
        std::max(1, config_.collect.offlineFailureThreshold),
        std::max(1, config_.collect.taskFailureBackoffThreshold)
    );
    if (config_.collect.offlineProbeOnly && config_.collect.offlineProbeIntervalMs > 0 &&
        state.consecutiveFailures >= offlineProbeThreshold) {
        state.offlineProbeUntilMs = nowMs + config_.collect.offlineProbeIntervalMs;
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

std::vector<ReadTask> Collector::expandBackedOffTasks(
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

std::vector<ReadTask> Collector::orderTasksByDeadline(
    std::vector<ReadTask> tasks,
    std::int64_t nowMs,
    bool realtimeFocused
) const {
    // Backed-off tasks last (they are skipped or split), then collect priority, then earliest due.
    using Rank = std::tuple<bool, int, std::int64_t, std::size_t>;
    std::vector<Rank> order;
    order.reserve(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        order.emplace_back(shouldSkipTaskForBackoff(tasks[i], nowMs, realtimeFocused),
                           taskPriority(tasks[i]), taskDueMs(tasks[i]), i);
    }
    std::sort(order.begin(), order.end());
    std::vector<ReadTask> ordered;
    ordered.reserve(tasks.size());
    for (const auto& rank : order) {
        ordered.push_back(std::move(tasks[std::get<3>(rank)]));
    }
    return ordered;
}

void Collector::coalesceTurnTasks(
    std::vector<ReadTask>& tasks,
    std::size_t turnSize,
    std::int64_t nowMs,
    bool realtimeFocused
) const {
    // Requests that run in this turn anyway (e.g. the background share next to a realtime block) are
    // read as one request when adjacent: one round trip, and with frameIntervalMs one turn serves both.
    // While a range is failing (backoff / adaptive split) keep requests as planned: merging split
    // leaves would recreate the rejected range.
    if (!config_.collect.batchOptimize || !taskFailureStates_.empty()) {
        return;
    }
    const auto gap = std::max(0, config_.collect.maxGapRegisters);
    const auto maxRegisters = std::max(1, config_.collect.maxBatchRegisters);
    for (std::size_t i = 0; i < turnSize && i < tasks.size(); ++i) {
        if (shouldSkipTaskForBackoff(tasks[i], nowMs, realtimeFocused)) continue;
        for (std::size_t j = i + 1; j < turnSize && j < tasks.size();) {
            auto& base = tasks[i];
            const auto& other = tasks[j];
            const auto start = std::min(base.start, other.start);
            const auto end = std::max(base.start + base.count, other.start + other.count);
            const bool mergeable = base.function == other.function &&
                !shouldSkipTaskForBackoff(other, nowMs, realtimeFocused) &&
                other.start <= base.start + base.count + gap && base.start <= other.start + other.count + gap &&
                end - start <= maxRegisters;
            if (!mergeable) {
                ++j;
                continue;
            }
            base.points.insert(base.points.end(), other.points.begin(), other.points.end());
            base.start = start;
            base.count = end - start;
            for (auto& point : base.points) point.offset = point.definition.address - start;
            tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(j));
            --turnSize;
            j = i + 1;
        }
    }
}

std::int64_t Collector::earliestRealtimeDueMs(const std::unordered_set<std::uint32_t>& heldPoints) const {
    std::int64_t due = std::numeric_limits<std::int64_t>::max();
    for (const auto index : readablePointOrder_) {
        const auto& point = config_.points[index];
        if (pointPriority(point) != 0 || heldPoints.count(point.index) != 0) continue;
        const auto last = lastReadMs_.find(point.index);
        if (last == lastReadMs_.end()) return std::numeric_limits<std::int64_t>::min();
        due = std::min(due, last->second + effectiveIntervalMs(point));
    }
    return due;
}

std::int64_t Collector::taskDueMs(const ReadTask& task) const {
    std::int64_t due = std::numeric_limits<std::int64_t>::max();
    for (const auto& taskPoint : task.points) {
        const auto it = lastReadMs_.find(taskPoint.definition.index);
        if (it == lastReadMs_.end()) {
            return std::numeric_limits<std::int64_t>::min();
        }
        due = std::min(due, it->second + effectiveIntervalMs(taskPoint.definition));
    }
    return due;
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
