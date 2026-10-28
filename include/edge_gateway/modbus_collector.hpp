#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "edge_gateway/common/collector_base.hpp"
#include "edge_gateway/modbus_error.hpp"

namespace edge_gateway {

// Shared by the collectors of one Modbus line (one daemon): background points of a meter yield to the
// realtime points of every meter on the line, not only to its own.
class ModbusLineSchedule {
public:
    // Earliest due time of the meter's realtime points; INT64_MAX when it has none or is not
    // collecting (offline, backed off).
    void update(const void* collector, int slave, std::int64_t realtimeDueMs);
    // True when another meter has realtime work due at nowMs whose slave can take a request now
    // (a slave waiting for its frameIntervalMs leaves the line free).
    bool realtimeWaitingElsewhere(const void* collector, std::int64_t nowMs, const IModbusClient& client) const;
    // True when another meter has realtime work due before dueMs (and due at nowMs) whose slave can take a
    // request now: earliest deadline first across the meters of the line.
    bool earlierRealtimeElsewhere(const void* collector, std::int64_t dueMs, std::int64_t nowMs,
                                  const IModbusClient& client) const;

private:
    struct Entry {
        int slave = 0;
        std::int64_t realtimeDueMs = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<const void*, Entry> entries_;
};

// Historical class name kept so existing tests/tools that instantiate
// Collector continue to mean "Modbus collector".
class Collector : public CollectorBase {
public:
    Collector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr,
        std::function<std::int64_t()> monotonicClockMs = {},
        std::shared_ptr<ModbusLineSchedule> lineSchedule = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;
    bool deviceOnline() const override;

private:
    bool shouldSkipForBackoff(std::int64_t nowMs) const;
    void noteReadSuccess(std::int64_t nowMs);
    void noteReadFailure(std::int64_t nowMs);
    bool currentOnline() const;
    bool shouldSkipTaskForBackoff(const ReadTask& task, std::int64_t nowMs, bool realtimeFocused = false) const;
    void noteTaskReadSuccess(const ReadTask& task);
    void noteTaskReadFailure(
        const ReadTask& task,
        std::int64_t nowMs,
        bool realtimeFocused,
        const std::string& message,
        ModbusFailureKind failureKind
    );
    int taskFailureBackoffDurationMs(int consecutiveFailures, bool realtimeFocused) const;
    void clearOverlappingTaskFailures(const ReadTask& task);
    std::string taskBackoffKey(const ReadTask& task) const;
    std::string taskFailureMessage(const ReadTask& task) const;
    bool canAdaptiveSplitTask(const ReadTask& task) const;
    std::vector<ReadTask> splitReadTask(const ReadTask& task) const;
    std::vector<ReadTask> splitReadTaskByPoint(const ReadTask& task) const;
    std::vector<ReadTask> limitAdaptiveSplitLeafProbeTasks(
        const ReadTask& parentTask,
        const std::vector<ReadTask>& tasks,
        int& leafProbeBudgetRemaining,
        std::int64_t nowMs
    ) const;
    std::vector<ReadTask> expandBackedOffTasks(
        const std::vector<ReadTask>& tasks,
        std::int64_t nowMs,
        bool realtimeFocused
    ) const;
    std::vector<ReadTask> limitToOfflineProbeTasks(const std::vector<ReadTask>& tasks) const;
    std::vector<ReadTask> orderTasksByDeadline(
        std::vector<ReadTask> tasks,
        std::int64_t nowMs,
        bool realtimeFocused
    ) const;
    void coalesceTurnTasks(
        std::vector<ReadTask>& tasks,
        std::size_t turnSize,
        std::int64_t nowMs,
        bool realtimeFocused
    ) const;
    // Earliest due time of the realtime points that can be read now (points of backed-off tasks excluded).
    std::int64_t earliestRealtimeDueMs(const std::unordered_set<std::uint32_t>& heldPoints) const;
    // Earliest due time of the task's points (lastRead + interval); minimum for never-read points.
    std::int64_t taskDueMs(const ReadTask& task) const;
    std::int64_t taskOldestValueUpdateMs(const ReadTask& task) const;
    int pointPriority(const PointDefinition& point) const;
    int taskPriority(const ReadTask& task) const;
    std::vector<std::uint16_t> executeReadTask(const ReadTask& task) const;
    int taskFailureBackoffCycles(int consecutiveFailures, bool realtimeFocused) const;

    void onSchedulingClockStepped(std::int64_t deltaMs) override;

    std::shared_ptr<IModbusClient> modbusClient_;
    std::function<std::int64_t()> monotonicClockMs_;
    std::shared_ptr<ModbusLineSchedule> lineSchedule_;
    // Shortened to the fastest point interval; a single in-flight request may overrun.
    int timeSliceMs_ = 1000;
    std::vector<std::size_t> readablePointOrder_;
    std::unordered_map<std::uint32_t, int> pointFailureCounts_;
    int consecutiveSuccesses_ = 0;
    int consecutiveFailures_ = 0;
    bool online_ = false;
    std::int64_t offlineProbeNextAttemptMs_ = 0;
    std::int64_t collectCycle_ = 0;
    std::int64_t slaveNextAttemptCycle_ = 0;
    std::int64_t backoffUntilMs_ = 0;
    struct TaskFailureState {
        int consecutiveFailures = 0;
        std::int64_t backoffUntilMs = 0;
        std::int64_t offlineProbeUntilMs = 0;
        std::int64_t nextAttemptCycle = 0;
        std::int64_t lastFailureMs = 0;
        std::string lastMessage;
        ModbusFailureKind failureKind = ModbusFailureKind::ProtocolException;
    };
    std::unordered_map<std::string, TaskFailureState> taskFailureStates_;
    mutable std::unordered_map<std::string, std::size_t> adaptiveSplitProbeCursors_;
    mutable std::size_t offlineProbeCursor_ = 0;
    std::int64_t lastBackgroundRequestMs_ = -1;
};

}  // namespace edge_gateway
