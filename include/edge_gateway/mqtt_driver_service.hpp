#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/event_stats_source.hpp"
#include "edge_gateway/mqtt_event_replay.hpp"
#include "edge_gateway/agc_avc_command_mailbox.hpp"
#include "edge_gateway/ota_service.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/point_store_router.hpp"
#include "edge_gateway/priority_control_lease.hpp"

namespace edge_gateway {

// How long a control request waits for its writeback result before reporting "outcome unknown":
// controlResultWaitTimeoutMs, but never shorter than the time a queued write can still start
// (memoryStore.pendingWriteMaxAgeMs + settle time). Shared by MQTT and direct maintenance.
int effectiveControlResultWaitMs(const MqttDriverConfig& driverConfig, const std::vector<DeviceConfig>& deviceConfigs);

class ProcessFileLock;

class MqttDriverService {
public:
    MqttDriverService(
        MqttConfig mqttConfig,
        MqttDriverConfig driverConfig,
        std::vector<DeviceConfig> deviceConfigs,
        MemoryPointStore& store,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::unique_ptr<OtaService> otaService = nullptr,
        SystemMonitorConfig::ScadaUpperComputerSafetyConfig scadaSafetyConfig = {},
        std::unique_ptr<IEventStatsSource> eventStats = nullptr,
        MqttEventReplayFactory businessReplayFactory = {},
        MqttEventReplayFactory managementReplayFactory = {},
        EventStoreIdentity eventStatsIdentity = {}
    );
    MqttDriverService(
        MqttConfig mqttConfig,
        MqttDriverConfig driverConfig,
        std::vector<DeviceConfig> deviceConfigs,
        PointStoreRouter& router,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::unique_ptr<OtaService> otaService = nullptr,
        SystemMonitorConfig::ScadaUpperComputerSafetyConfig scadaSafetyConfig = {},
        std::unique_ptr<IEventStatsSource> eventStats = nullptr,
        MqttEventReplayFactory businessReplayFactory = {},
        MqttEventReplayFactory managementReplayFactory = {},
        EventStoreIdentity eventStatsIdentity = {}
    );
    ~MqttDriverService();

    MqttDriverService(const MqttDriverService&) = delete;
    MqttDriverService& operator=(const MqttDriverService&) = delete;

    void start();
    // IPC shutdown drains on the replay owner thread; may wait for EventStore.
    void stop();
    void setAgcAvcCommandMailboxRuntime(AgcAvcCommandMailboxRuntime runtime);
    bool isRunning() const;

    void runScanOnce(std::int64_t nowMs);
    // Called by the scan loop with the wall and monotonic clocks read together. A wall-clock step
    // (their difference changing by >= 1 s) moves the command-reply deadlines with it, so the reply
    // timing follows elapsed time like the monotonic pending-write expiry in the device daemons.
    void observeWallClock(std::int64_t wallMs, std::int64_t monotonicMs);
    void runEventReplayOnce(std::int64_t nowMs);
    void publishFullSnapshotNow(std::int64_t nowMs);
    void publishOnDemandNow(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs);
    static std::vector<std::uint32_t> resolveFullUploadIndexes(
        const MqttDriverConfig& driverConfig,
        const std::vector<DeviceConfig>& deviceConfigs,
        const PointStoreRouter& router
    );

private:
    struct PointRoute {
        std::string machineCode;
        std::string meterCode;
        std::string pointCode;
        bool writable = false;
        bool commandMailbox = false;
    };
    struct RealtimeSession {
        std::string sessionId;
        std::string machineCode;
        std::string meterCode;
        std::vector<std::uint32_t> indexes;
        std::int64_t expireAtMs = 0;
        std::int64_t nextPublishMs = 0;
        int intervalMs = 0;
        std::size_t requestedCount = 0;
    };
    struct PendingCommandReply {
        MqttCommandReply reply;
        PointStoreRoute route;
        // deadlineMs: non-final "unknown" reply; horizonMs: stop watching (final "unknown" reply).
        std::int64_t deadlineMs = 0;
        std::int64_t horizonMs = 0;
        bool timeoutReported = false;
        bool durableControl = false;
    };
    enum class CommandAdmission {
        Admitted,
        Duplicate,
        RateLimited
    };

    void runScanOnceInternal(std::int64_t nowMs, int incomingTimeoutMs);
    void processIncomingMessages(std::int64_t nowMs);
    void processIncomingMessages(std::int64_t nowMs, int timeoutMs);
    void replayPendingOtaStatuses(std::int64_t nowMs) noexcept;
    void replayEventOutboxIfNeeded(std::int64_t nowMs);
    void replayIpcEventsIfNeeded(std::int64_t nowMs);
    void drainIpcEvents();
    bool isolatedEventForwarderIsActive(std::int64_t nowMs) const;
    void publishEventDelegationReady(std::int64_t nowMs);
    void removeEventDelegationReady();
    bool shouldDeferSnapshotForEventBacklog(std::int64_t nowMs);
    bool isolatedFullForwarderIsActive(std::int64_t nowMs) const;
    bool publishIsolatedFallback(std::int64_t nowMs);
    bool hasActiveRealtimeSessions(std::int64_t nowMs) const;
    void cleanupExpiredRealtimeSessions(std::int64_t nowMs);
    void publishDueRealtimeSessions(std::int64_t nowMs);
    void processPendingCommandReplies(std::int64_t nowMs);
    int scanLoopIncomingTimeoutMs(std::int64_t nowMs) const;
    void handleCommandRequest(const std::string& payload, std::int64_t nowMs, bool retained = false);
    CommandAdmission admitCommand(
        const std::string& meterCode,
        const std::string& cmdId,
        std::int64_t nowMs,
        bool durableControl
    );
    bool admitMailboxCommand(const std::string& meterCode, std::int64_t nowMs);
    void handleOtaRequest(const std::string& payload, std::int64_t nowMs);
    void handleOtaQueryRequest(const std::string& payload);
    void reportOtaQueryDiagnostic(const char* reason);
    void handleRealtimeRequest(const std::string& payload, std::int64_t nowMs);
    void processRealtimeRequest(const std::string& payload, std::int64_t nowMs, bool& admitted);
    void startOtaJob(const OtaRequest& request, const std::string& machineCode, std::int64_t nowMs);
    void publishStatusEvent(
        const std::string& event,
        std::int64_t ts,
        const std::string& detailsJson = std::string(),
        bool includeExceptionDetails = true
    ) const;
    void scanLoop();
    void replayLoop();
    std::vector<StoredPointValue> filterValues(const std::vector<std::uint32_t>& indexes, std::int64_t nowMs) const;
    std::vector<StoredPointValue> filterValuesByMeter(
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) const;
    void publishRealtimeValues(
        std::vector<StoredPointValue> values,
        std::size_t requestedCount,
        std::int64_t nowMs,
        const std::string& sessionId
    );
    bool isLegacyTelemetryDue(std::int64_t nowMs) const;
    void publishLegacyTelemetryNow(std::int64_t nowMs);
    void publishLegacyTelemetry(
        const std::vector<StoredPointValue>& values,
        std::int64_t nowMs
    );
    void recordScanHealth(double durationMs, bool failed, std::int64_t nowMs);
    void recordReplayHealth(double durationMs, bool failed);
    void publishHealthFileLocked(std::int64_t nowMs);
    void enrichValue(StoredPointValue& value) const;
    std::vector<StoredPointValue> enrichValues(std::vector<StoredPointValue> values) const;
    std::string primaryMachineCode() const;

    MqttConfig mqttConfig_;
    MqttDriverConfig driverConfig_;
    std::unique_ptr<PointStoreRouter> ownedRouter_;
    PointStoreRouter& router_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::unique_ptr<IEventStatsSource> eventStats_;
    EventStoreIdentity eventStatsIdentity_;
    MqttEventReplayFactory businessReplayFactory_, managementReplayFactory_;
    MqttEventReplaySession businessReplay_, managementReplay_;
    std::unique_ptr<ProcessFileLock> ipcBusinessReplayLock_;
    std::atomic<bool> eventReplayStopping_{false};
    std::unique_ptr<ProcessFileLock> eventDelegationLiveLock_;
    std::unique_ptr<OtaService> otaService_;
    PriorityControlLease priorityControlLease_;
    SystemMonitorConfig::ScadaUpperComputerSafetyConfig scadaSafetyConfig_;
    std::unordered_set<std::string> machineCodes_;
    std::unordered_map<std::uint32_t, PointRoute> pointRoutes_;
    std::int64_t lastFullUploadMs_ = 0;
    std::int64_t lastLegacyTelemetryMs_ = 0;
    std::int64_t lastEventOutboxReplayMs_ = 0;
    mutable std::mutex eventStateMutex_;
    std::int64_t eventLastAckAtMs_ = 0;
    std::string eventLastError_;
    std::int64_t lastEventDelegationPublishMs_ = 0;
    std::int64_t lastOtaReplayAttemptMs_ = 0;
    bool subscribeUnavailable_ = false;
    std::int64_t otaReplayFirstSuccessMs_ = 0;
    int otaReplayRetryDelayMs_ = 5000;
    int otaReplaySuccessRounds_ = 0;
    std::int64_t otaQueryWindowStartedMs_ = 0;
    std::size_t otaQueryAttemptsInWindow_ = 0;
    std::int64_t lastOtaQueryDiagnosticMs_ = 0;
    std::size_t suppressedOtaQueryDiagnostics_ = 0;
    std::int64_t lastSnapshotDeferredMs_ = 0;
    bool isolatedFullForwarderWasActive_ = false;
    std::int64_t isolatedFallbackWaitStartedMs_ = 0;
    std::int64_t isolatedFallbackNextAttemptMs_ = 0;
    int isolatedFallbackRetryDelayMs_ = 0;
    std::unordered_map<std::string, RealtimeSession> realtimeSessions_;
    std::deque<PendingCommandReply> pendingCommandReplies_;
    std::int64_t lastObservedWallMs_ = 0;
    std::int64_t lastObservedMonotonicMs_ = 0;
    int resultWaitMs_ = 0;
    std::mutex commandRateMutex_;
    std::unordered_map<std::string, std::deque<std::int64_t>> commandWindowByMeter_;
    std::unordered_map<std::string, std::deque<std::int64_t>> mailboxWindowByMeter_;
    std::unordered_map<std::string, std::int64_t> recentCommandIds_;
    AgcAvcCommandMailboxRuntime agcAvcCommandMailbox_;
    std::atomic<bool> running_{false};
    std::atomic<bool> otaInProgress_{false};
    std::mutex otaMutex_;
    std::string otaJobId_;
    std::thread scanThread_;
    std::thread replayThread_;
    std::thread otaThread_;
    std::mutex healthMutex_;
    std::deque<double> scanDurationsMs_;
    std::deque<double> replayDurationsMs_;
    std::uint64_t totalScanCycles_ = 0;
    std::uint64_t scanDeadlineMissCycles_ = 0;
    std::uint64_t scanFailedCycles_ = 0;
    std::uint64_t totalReplayCycles_ = 0;
    std::uint64_t replayFailedCycles_ = 0;
    std::atomic<std::uint64_t> fullSnapshotsPublished_{0};
    std::int64_t lastHealthPublishMs_ = 0;
};

}  // namespace edge_gateway
