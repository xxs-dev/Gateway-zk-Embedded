#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace edge_gateway {

enum class MqttEventReplayLane { MainBusiness, MainManagement, ForwardEvents };

struct MqttEventReplayRequest {
    MqttEventReplayLane lane = MqttEventReplayLane::MainBusiness;
    std::string targetId;
    std::vector<std::string> includeTypes, excludeTypes;
    std::size_t maxBytes = 0;
    std::size_t maxMessages = 16;
    // Check before every network call, together with the sender's BOOTTIME budget.
    std::function<bool()> authorized;
};

struct MqttEventReplayProgress {
    bool pending = false;
    bool healthy = false;
    // Incremental for this invocation, including attempts whose ACK is unknown.
    std::size_t attemptedBytes = 0;
    // Only newly reconciled, effective storage ACKs, never just callback success.
    std::size_t ackedCount = 0;
    std::size_t alarmCount = 0, changeCount = 0, otherCount = 0;
    std::string error;
};

class IMqttEventReplay {
public:
    virtual ~IMqttEventReplay() = default;
    // One batch per instance. No next Claim after that batch is finished.
    // drainOnly forbids new Claim/network but resolves unknown registration and
    // mutations, ACKs confirmed sends and releases remaining claimed messages.
    // pending=false means no unresolved mutation or active batch remains.
    // Expected IPC failures should return pending=true with progress/error;
    // an exception also retains the instance and must never discard uncertainty.
    virtual MqttEventReplayProgress runOnce(bool drainOnly) = 0;
};

// Called on the replay owner thread only after authorization/ownership checks.
// Capture configuration and bounded network transport, not a prebuilt sender.
// Must enforce the fixed target/type scope and requested claim limits, fail
// closed on mismatch, and never construct or fall back to a legacy Outbox.
using MqttEventReplayFactory = std::function<std::unique_ptr<IMqttEventReplay>(const MqttEventReplayRequest&)>;

// Service-side lifetime guard; no concrete IPC implementation/link dependency.
class MqttEventReplaySession {
public:
    MqttEventReplaySession() = default;
    MqttEventReplaySession(const MqttEventReplaySession&) = delete;
    MqttEventReplaySession& operator=(const MqttEventReplaySession&) = delete;
    bool active() const noexcept { return static_cast<bool>(replay_); }
    MqttEventReplayProgress run(const MqttEventReplayFactory& factory,
        const MqttEventReplayRequest& request, bool drainOnly) {
        if (replay_ && owner_ != std::this_thread::get_id())
            throw std::logic_error("MQTT IPC replay cannot cross owner threads");
        if (!replay_) {
            if (drainOnly) return {};
            if (!factory || !request.authorized || request.maxBytes == 0 ||
                request.maxBytes > 32768 || request.maxMessages == 0 || request.maxMessages > 16)
                throw std::invalid_argument("invalid MQTT IPC replay request");
            draining_ = false;
            byteLimit_ = request.maxBytes;
            owner_ = std::this_thread::get_id();
            auto guarded = request;
            const auto authorized = request.authorized;
            guarded.authorized = [this, authorized] {
                if (draining_) return false;
                try { if (authorized()) return true; } catch (...) {}
                draining_ = true;
                return false;
            };
            if (!guarded.authorized()) return {};
            replay_ = factory(guarded);
            if (!replay_) throw std::runtime_error("MQTT IPC replay factory returned null");
        }
        // A recovered Claim retains its original limits. Do not send it using
        // a smaller management remainder after business consumed this round.
        draining_ = draining_ || drainOnly || request.maxBytes < byteLimit_;
        auto result = replay_->runOnce(draining_);
        if (result.attemptedBytes > byteLimit_)
            throw std::runtime_error("MQTT IPC replay exceeded its byte budget");
        if (draining_) result.healthy = false;
        if (!result.pending) replay_.reset();
        return result;
    }
private:
    std::unique_ptr<IMqttEventReplay> replay_;
    std::thread::id owner_;
    std::size_t byteLimit_ = 0;
    bool draining_ = false;
};

} // namespace edge_gateway
