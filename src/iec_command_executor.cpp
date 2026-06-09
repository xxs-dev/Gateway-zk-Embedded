#include "edge_gateway/iec_command_executor.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

IecCommandExecutor::IecCommandExecutor(
    DeviceConfig config,
    MemoryPointStore& store,
    std::shared_ptr<IecClient> client,
    std::shared_ptr<IMqttPublisher> mqttPublisher
) : config_(std::move(config)),
    store_(store),
    client_(std::move(client)),
    mqttPublisher_(std::move(mqttPublisher)) {
    if (!client_) {
        throw std::invalid_argument("iecClient is required");
    }
    store_.registerPoints(config_.machineCode, config_.meterCode, config_.points);
}

CommandResult IecCommandExecutor::executeByIndex(
    const std::string& cmdId,
    std::uint32_t index,
    double value,
    std::int64_t nowMs
) const {
    const auto& point = findPointByIndex(index);
    auto result = client_->writeByPoint(point, value, cmdId, config_.machineCode, config_.meterCode, nowMs);
    if (mqttPublisher_) {
        mqttPublisher_->publishCommandResult(result);
    }
    return result;
}

const PointDefinition& IecCommandExecutor::findPointByIndex(std::uint32_t index) const {
    const auto it = std::find_if(config_.points.begin(), config_.points.end(), [index](const PointDefinition& point) {
        return point.index == index;
    });
    if (it == config_.points.end()) {
        throw std::invalid_argument("point index not found: " + std::to_string(index));
    }
    return *it;
}

}  // namespace edge_gateway
