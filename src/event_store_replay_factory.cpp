#include "edge_gateway/event_store_replay_factory.hpp"

#include <algorithm>
#include <set>

namespace edge_gateway {
namespace {
class EventStoreReplay final : public IMqttEventReplay {
public:
    explicit EventStoreReplay(EventStoreSenderOptions options) : sender_(std::move(options)) {}
    MqttEventReplayProgress runOnce(bool drainOnly) override {
        if (finished_) throw std::logic_error("IPC replay instance already finished its batch");
        const auto progress = sender_.runOnceDetailed(drainOnly);
        finished_ = !progress.pending;
        MqttEventReplayProgress result;
        result.pending = progress.pending;
        result.healthy = progress.healthy;
        result.attemptedBytes = progress.attemptedBytes;
        result.ackedCount = progress.ackedCount;
        result.alarmCount = progress.alarmCount;
        result.changeCount = progress.changeCount;
        result.otherCount = progress.otherCount;
        result.error = progress.error;
        return result;
    }
private:
    EventStoreSender sender_;
    bool finished_ = false;
};
} // namespace

MqttEventReplayFactory makeEventStoreReplayFactory(EventStoreReplayFactoryOptions options) {
    const std::set<std::string> types(options.eventTypes.begin(), options.eventTypes.end());
    if ((!options.client.expectedSenderTargetId.empty() && options.client.expectedSenderTargetId != options.targetId) ||
        (!options.client.expectedSenderEventTypes.empty() &&
         std::set<std::string>(options.client.expectedSenderEventTypes.begin(), options.client.expectedSenderEventTypes.end()) != types))
        throw std::invalid_argument("IPC replay client/factory expected scope mismatch");
    if (options.client.role != EventStoreClientRole::Sender || options.targetId.empty() || (!options.send && !options.sendBatch) ||
        types.empty() || types.size() > 16 || types.size() != options.eventTypes.size() || types.count("") ||
        options.leaseMs < 100 || options.leaseMs > 30000 || options.networkTimeoutMs < 1 ||
        options.completionReserveMs < 1 || options.completionReserveMs >= options.leaseMs ||
        options.networkTimeoutMs > options.leaseMs - options.completionReserveMs)
        throw std::invalid_argument("invalid IPC replay factory options");
    if ((options.lane == MqttEventReplayLane::MainManagement &&
         (options.targetId != "main" || types != std::set<std::string>{"ota_status"})) ||
        (options.lane == MqttEventReplayLane::MainBusiness &&
         (options.targetId != "main" || types != std::set<std::string>{"alarm", "change"})) ||
        (options.lane == MqttEventReplayLane::ForwardEvents &&
         !std::all_of(types.begin(), types.end(), [](const std::string& type) { return type == "alarm" || type == "change"; })))
        throw std::invalid_argument("IPC replay role has an invalid event scope");
    return [options, types](const MqttEventReplayRequest& request) -> std::unique_ptr<IMqttEventReplay> {
        const std::set<std::string> requested(request.includeTypes.begin(), request.includeTypes.end());
        if (request.lane != options.lane || request.targetId != options.targetId || requested != types ||
            requested.size() != request.includeTypes.size() || !request.excludeTypes.empty() ||
            !request.authorized || request.maxBytes == 0 || request.maxBytes > 32768 ||
            request.maxMessages == 0 || request.maxMessages > 16)
            throw std::invalid_argument("IPC replay factory scope or limit mismatch");
        EventStoreSenderOptions sender;
        sender.client = options.client;
        sender.client.expectedSenderTargetId = options.targetId;
        sender.client.expectedSenderEventTypes = options.eventTypes;
        sender.limit = static_cast<int>(request.maxMessages);
        sender.maxBytes = static_cast<int>(request.maxBytes);
        sender.leaseMs = options.leaseMs;
        sender.networkTimeoutMs = options.networkTimeoutMs;
        sender.completionReserveMs = options.completionReserveMs;
        sender.leaseClock = options.leaseClock;
        sender.authorized = request.authorized;
        if (options.sendBatch) sender.sendBatch = [options, types](const auto& messages, const auto& call,
            const auto& canSend, const auto& onAttempt, const auto& onConfirmed) {
            for (const auto& message : messages)
                if (!types.count(message.eventType)) throw std::runtime_error("IPC sender returned an out-of-scope event");
            options.sendBatch(messages, call, canSend, onAttempt, onConfirmed);
        };
        if (options.send) sender.send = [options, types](const EventStoreSenderMessage& message, const EventStoreSenderCall& call) {
            if (!types.count(message.eventType)) throw std::runtime_error("IPC sender returned an out-of-scope event");
            return options.send(message, call);
        };
        return std::unique_ptr<IMqttEventReplay>(new EventStoreReplay(std::move(sender)));
    };
}

} // namespace edge_gateway
