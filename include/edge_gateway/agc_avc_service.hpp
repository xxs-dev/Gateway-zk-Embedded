#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/agc_avc_controller.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/priority_control_lease.hpp"

namespace edge_gateway {

class IAgcAvcPointBus {
public:
    virtual ~IAgcAvcPointBus() = default;
    virtual Optional<PointStoreRoute> routeByIndex(std::uint32_t index) const = 0;
    virtual std::vector<StoredPointValue> readSnapshot(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const = 0;
    virtual CommandSubmitResult publishVirtual(PointValue value) = 0;
    virtual CommandSubmitResult submitControl(const PendingWriteCommand& command) = 0;
    virtual Optional<WritebackResultRecord> readWriteResult(
        const PointStoreRoute& route,
        const std::string& cmdId
    ) const = 0;
};

class PointStoreAgcAvcBus final : public IAgcAvcPointBus {
public:
    explicit PointStoreAgcAvcBus(PointStoreRouter& router);

    Optional<PointStoreRoute> routeByIndex(std::uint32_t index) const override;
    std::vector<StoredPointValue> readSnapshot(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const override;
    CommandSubmitResult publishVirtual(PointValue value) override;
    CommandSubmitResult submitControl(const PendingWriteCommand& command) override;
    Optional<WritebackResultRecord> readWriteResult(
        const PointStoreRoute& route,
        const std::string& cmdId
    ) const override;

private:
    PointStoreRouter& router_;
};

struct AgcAvcValidationIssue {
    std::string severity;
    std::string path;
    std::string message;
};

class AgcAvcService {
public:
    AgcAvcService(AgcAvcConfig config, IAgcAvcPointBus& pointBus);
    ~AgcAvcService();

    std::vector<AgcAvcValidationIssue> validate() const;
    AgcAvcCycleOutput tick(std::int64_t nowMs);
    const AgcAvcCycleOutput& lastOutput() const;

private:
    struct PendingWriteObservation {
        PointStoreRoute route;
        std::string cmdId;
        std::size_t pcsIndex = 0;
        bool reactive = false;
        double target = 0.0;
        std::int64_t submittedAtMs = 0;
    };

    struct PendingFeedbackObservation {
        std::size_t pcsIndex = 0;
        bool reactive = false;
        double target = 0.0;
        std::int64_t startedAtMs = 0;
    };

    std::vector<std::uint32_t> collectInputIndexes() const;
    void processWriteResults(std::int64_t nowMs, const AgcAvcCycleInput& input);
    void registerWriteFailure(std::uint32_t index);
    void publishOutputs(const AgcAvcCycleOutput& output, std::int64_t nowMs);
    void submitAssignments(const AgcAvcCycleOutput& output, std::int64_t nowMs);

    AgcAvcConfig config_;
    IAgcAvcPointBus& pointBus_;
    AgcAvcController controller_;
    PriorityControlLease priorityLease_;
    PowerControlOwnership powerOwnership_;
    std::string ownershipSessionId_;
    std::vector<std::uint32_t> ownershipTargetIndexes_;
    bool ownershipHeld_ = false;
    bool commandLatched_ = false;
    AgcAvcCommandInput latchedCommand_;
    AgcAvcCycleOutput lastOutput_;
    int lastWriteStatus_ = 0;
    int consecutiveWriteFailures_ = 0;
    std::unordered_map<std::uint32_t, double> lastSubmittedValue_;
    std::unordered_map<std::uint32_t, std::int64_t> lastSubmittedAtMs_;
    std::unordered_map<std::uint32_t, PendingWriteObservation> pendingWrites_;
    std::vector<PendingFeedbackObservation> pendingFeedback_;
};

}  // namespace edge_gateway
