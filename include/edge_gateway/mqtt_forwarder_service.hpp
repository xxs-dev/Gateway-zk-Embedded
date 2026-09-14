#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "edge_gateway/event_stats_source.hpp"
#include "edge_gateway/mqtt_event_replay.hpp"
#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/models.hpp"
#include "edge_gateway/mqtt_control_result_store.hpp"
#include "edge_gateway/mqtt_event_outbox.hpp"
#include "edge_gateway/point_store_router.hpp"

namespace edge_gateway {

class PowerControlOwnership;
class ProcessFileLock;

class MqttForwarderService {
public:
    MqttForwarderService(
        MqttForwardConfig forwardConfig,
        PointStoreRouter& router,
        std::shared_ptr<IMqttDriverPublisher> publisher,
        std::string healthFile = {},
        std::unique_ptr<MqttEventOutbox> eventOutbox = nullptr,
        std::string eventOutboxPath = {},
        std::string eventReplayLockFile = {},
        std::string eventDelegationReadyFile = {},
        std::unique_ptr<IEventStatsSource> eventStats = nullptr,
        MqttEventReplayFactory eventReplayFactory = {},
        EventStoreIdentity eventStatsIdentity = {}
    );
    ~MqttForwarderService();

    MqttForwarderService(const MqttForwarderService&) = delete;
    MqttForwarderService& operator=(const MqttForwarderService&) = delete;

    void start();
    // IPC shutdown drains on the replay owner thread; may wait for EventStore.
    void stop();
    bool isRunning() const;
    void runOnce(std::int64_t nowMs);

    static MqttConfig makeTxOnlyMqttConfig(
        const MqttForwardConfig& forwardConfig,
        const std::string& machineCode
    );
    static MqttConfig makeMqttConfig(
        const MqttForwardConfig& forwardConfig,
        const std::string& machineCode
    );
    static MqttConfig makePrimaryFullMqttConfig(
        const MqttConfig& primaryConfig,
        const MqttFullUploadWorkerConfig& workerConfig,
        const std::string& machineCode
    );

private:
    struct PendingControlResult {
        std::string id;
        std::string fingerprint;
        int type = 1;
        double targetKw = 0.0;
        std::uint32_t generation = 0;
        std::int64_t acceptedAtMs = 0;
        std::int64_t deadlineMs = 0;
        std::vector<PointStoreRoute> routes;
        bool submitted = false;
    };

    void publishLoop();
    void replayEventsIfDue(std::int64_t nowMs);
    void replayIpcEventsIfDue(std::int64_t nowMs);
    void drainIpcEvents();
    bool eventDelegationReady(std::int64_t nowMs) const;
    void pollIncomingCommands(std::int64_t nowMs);
    void handleCommandMessage(const MqttIncomingMessage& message, std::int64_t nowMs);
    void processPendingControlResults(std::int64_t nowMs);
    void publishUndeliveredControlResults(std::int64_t nowMs);
    std::string buildFinalControlResultPayload(
        const PendingControlResult& pending,
        const std::vector<WritebackResultRecord>& results,
        bool timedOut,
        std::int64_t nowMs
    ) const;
    bool publishStoredControlResult(
        const MqttControlResultRecord& record,
        std::int64_t nowMs
    ) const;
    void publishControlReply(
        const std::string& id,
        int type,
        bool accepted,
        bool duplicate,
        const std::string& mode,
        bool hasTargetKw,
        double targetKw,
        std::uint32_t generation,
        const std::string& message,
        std::int64_t nowMs
    ) const;
    void releaseOwnSession();
    bool writeHealth(
        bool healthy,
        const std::string& error,
        std::size_t valueCount,
        std::int64_t nowMs
    ) const;

    MqttForwardConfig forwardConfig_;
    PointStoreRouter& router_;
    std::shared_ptr<IMqttDriverPublisher> publisher_;
    std::string healthFile_;
    std::unique_ptr<MqttEventOutbox> eventOutbox_;
    std::unique_ptr<IEventStatsSource> eventStats_;
    EventStoreIdentity eventStatsIdentity_;
    MqttEventReplayFactory eventReplayFactory_;
    MqttEventReplaySession eventReplay_;
    std::unique_ptr<ProcessFileLock> ipcEventReplayLock_;
    std::atomic<bool> eventReplayStopping_{false};
    std::string eventOutboxPath_;
    std::string eventReplayLockFile_;
    std::string eventDelegationReadyFile_;
    std::int64_t lastPublishMs_ = 0;
    std::int64_t lastAttemptMs_ = 0;
    std::int64_t nextAttemptMs_ = 0;
    mutable std::int64_t lastHealthMs_ = 0;
    int retryDelayMs_ = 0;
    std::uint64_t consecutiveFailures_ = 0;
    bool lastHealthy_ = false;
    bool publishInProgress_ = false;
    std::size_t lastValueCount_ = 0;
    std::string lastError_;
    std::int64_t lastEventReplayMs_ = 0;
    std::int64_t eventHeartbeatMonotonicMs_ = 0;
    std::int64_t eventLeaseUntilMonotonicMs_ = 0;
    std::int64_t eventLastAckAtMs_ = 0;
    bool eventStatsAwaitingDelegation_ = false;
    bool eventOutboxHealthy_ = false;
    bool eventDelegationActive_ = false;
    std::string eventLastError_;
    bool scheduleInitialized_ = false;
    std::atomic<bool> running_{false};
    std::thread loopThread_;
    std::unique_ptr<PowerControlOwnership> ownership_;
    std::unique_ptr<MqttControlResultStore> controlResultStore_;
    std::deque<PendingControlResult> pendingControlResults_;
};

}  // namespace edge_gateway
