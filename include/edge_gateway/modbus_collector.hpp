#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/common/collector_base.hpp"

namespace edge_gateway {

// Historical class name kept so existing tests/tools that instantiate
// Collector continue to mean "Modbus collector".
class Collector : public CollectorBase {
public:
    Collector(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IModbusClient> modbusClient,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );

    CollectCycleResult collectOnce(std::int64_t nowMs, bool realtimeFocused = false) override;

private:
    bool shouldSkipForBackoff(std::int64_t nowMs) const;
    void noteReadSuccess(std::int64_t nowMs);
    void noteReadFailure(std::int64_t nowMs);
    bool currentOnline() const;
    bool shouldSkipTaskForBackoff(const ReadTask& task, std::int64_t nowMs) const;
    void noteTaskReadSuccess(const ReadTask& task);
    void noteTaskReadFailure(
        const ReadTask& task,
        std::int64_t nowMs,
        bool realtimeFocused,
        const std::string& message
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
    std::vector<ReadTask> expandBackedOffTasksBeforeBudget(
        const std::vector<ReadTask>& tasks,
        std::int64_t nowMs
    ) const;
    std::vector<ReadTask> splitMixedPriorityTasks(const std::vector<ReadTask>& tasks) const;
    std::vector<ReadTask> limitToOfflineProbeTasks(const std::vector<ReadTask>& tasks) const;
    std::vector<ReadTask> limitToMeterTaskBudget(
        const std::vector<ReadTask>& tasks,
        std::int64_t nowMs,
        int maxTasksPerCycle
    ) const;
    std::int64_t taskOldestValueUpdateMs(const ReadTask& task) const;
    int pointPriority(const PointDefinition& point) const;
    int taskPriority(const ReadTask& task) const;
    std::vector<std::uint16_t> executeReadTask(const ReadTask& task) const;

    std::shared_ptr<IModbusClient> modbusClient_;
    std::unordered_map<std::uint32_t, int> pointFailureCounts_;
    int consecutiveSuccesses_ = 0;
    int consecutiveFailures_ = 0;
    bool online_ = false;
    std::int64_t backoffUntilMs_ = 0;
    struct TaskFailureState {
        int consecutiveFailures = 0;
        std::int64_t backoffUntilMs = 0;
        std::string lastMessage;
        bool noResponse = false;
    };
    std::unordered_map<std::string, TaskFailureState> taskFailureStates_;
    mutable std::unordered_map<std::string, std::size_t> adaptiveSplitProbeCursors_;
    mutable std::size_t offlineProbeCursor_ = 0;
    std::int64_t lastBackgroundTaskMs_ = -1;
};

}  // namespace edge_gateway
