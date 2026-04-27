#include "edge_gateway/read_task_planner.hpp"

#include <algorithm>
#include <stdexcept>

namespace edge_gateway {

namespace {

bool canMerge(const ReadTask& task, const PointDefinition& point, int maxBatchRegisters) {
    if (task.function != point.read.function) {
        return false;
    }

    const auto nextEnd = std::max(task.start + task.count, point.address + point.read.length);
    return (nextEnd - task.start) <= maxBatchRegisters;
}

}  // namespace

std::vector<ReadTask> ReadTaskPlanner::build(
    const std::vector<PointDefinition>& points,
    int maxBatchRegisters
) {
    if (maxBatchRegisters <= 0) {
        throw std::invalid_argument("maxBatchRegisters must be positive");
    }

    std::vector<PointDefinition> readable;
    readable.reserve(points.size());
    for (const auto& point : points) {
        if (point.enabled && point.read.enable && point.read.dataType != "device_online") {
            readable.push_back(point);
        }
    }

    std::sort(readable.begin(), readable.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.read.function != rhs.read.function) {
            return lhs.read.function < rhs.read.function;
        }
        return lhs.address < rhs.address;
    });

    std::vector<ReadTask> tasks;
    for (const auto& point : readable) {
        if (tasks.empty() || !canMerge(tasks.back(), point, maxBatchRegisters)) {
            ReadTask task;
            task.function = point.read.function;
            task.start = point.address;
            task.count = point.read.length;
            task.points.push_back(ReadTaskPoint{point, 0});
            tasks.push_back(task);
            continue;
        }

        auto& task = tasks.back();
        task.points.push_back(ReadTaskPoint{point, point.address - task.start});
        const auto taskEnd = std::max(task.start + task.count, point.address + point.read.length);
        task.count = taskEnd - task.start;
    }

    return tasks;
}

}  // namespace edge_gateway
