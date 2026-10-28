#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/interfaces.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/power_control_ownership.hpp"
#include "edge_gateway/priority_control_lease.hpp"
#include "edge_gateway/sqlite_sample_writer.hpp"

namespace edge_gateway {

// Time arguments: wallMs (system clock) timestamps values; monoMs (steady clock) drives every
// timeout (online, receive timeout, interface checks) so a wall-clock step cannot freeze or flip them.
class CanDriverService {
public:
    CanDriverService(
        DeviceConfig config,
        MemoryPointStore& store,
        std::shared_ptr<IMqttPublisher> mqttPublisher = nullptr
    );
    ~CanDriverService();

    CanDriverService(const CanDriverService&) = delete;
    CanDriverService& operator=(const CanDriverService&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    // Reads at most one frame (data or error frame) and applies it.
    std::size_t processReceiveOnce(int timeoutMs);
    std::size_t processWritebackOnce(std::int64_t nowMs);
    std::size_t flushPersistentOnce();
    void updateOnlineStatus(std::int64_t wallMs, std::int64_t monoMs);
    // Polls the interface (up/running, socket) every collect.interfaceCheckIntervalMs and recovers it.
    void checkInterfaceOnce(std::int64_t wallMs, std::int64_t monoMs);
    bool interfaceOnline() const { return interfaceOnline_; }
    static std::int64_t steadyNowMs();

private:
    struct RuntimeDevice {
        DeviceConfig config;
        std::int64_t lastSeenMono = 0;
        bool online = false;
        int onlineTimeoutMs = 5000;
        std::int64_t lastOnlinePublishedMono = 0;
        std::vector<std::uint32_t> onlineFrameIds;  // match any frame type
        std::vector<std::uint64_t> signalFrameKeys;
    };

    struct RuntimePoint {
        std::size_t deviceIndex = 0;
        PointDefinition point;
        bool readSignal = false;   // decoded from received frames
        bool writeSignal = false;  // encoded into a transmit frame
        std::uint64_t readKey = 0;
        std::uint64_t writeKey = 0;
        std::int64_t lastSeenMono = 0;
        bool stalePublished = false;
        std::int64_t lastErrorLogMono = 0;
        std::size_t suppressedErrors = 0;
    };

    // One transmitted frame: its layout and the last payload sent (mirror), so writing one signal
    // keeps the other signals of the frame. Starts from the persisted state (frameStateFile) when it
    // matches the current definition, else from write.can.payloadTemplate (or zeros). Pulse signals
    // are back at their idle value in the mirror right after every send.
    struct TxFrame {
        std::uint32_t frameId = 0;
        bool extended = false;
        bool fd = false;
        bool remoteRequest = false;
        std::vector<std::uint8_t> payload;
        std::string layout;                    // definition fingerprint (can_frame_state::layoutHash)
        std::vector<std::size_t> pulsePoints;  // runtimePoints_ indexes of pulse signals
    };

    struct StagedWrite;

    void initializeRuntimeDevices();
    void configureInterface() const;
    void openSocket();
    void openSocketLocked();
    void closeSocket();
    void closeSocketLocked();
    int duplicateSocket() const;
    enum class LinkState { Down, NoCarrier, Ready };
    LinkState linkStateLocked() const;
    void handleErrorFrame(std::uint32_t errorClass, const std::vector<std::uint8_t>& data, std::int64_t wallMs,
                          std::int64_t monoMs);
    void setInterfaceOnline(bool online, const std::string& reason, std::int64_t wallMs, std::int64_t monoMs);
    void applyDataFrame(std::uint32_t frameId, bool extended, const std::vector<std::uint8_t>& payload,
                        std::int64_t wallMs, std::int64_t monoMs, std::size_t& matched);
    void markPointBad(RuntimePoint& runtimePoint, const std::string& message, bool stale, std::int64_t wallMs);
    void logPointError(RuntimePoint& runtimePoint, const std::string& message, std::int64_t monoMs);
    void runStartupWrites();
    void sendSingleWrite(const RuntimePoint& runtimePoint, double value);
    void transmitLocked(const TxFrame& frame, const std::vector<std::uint8_t>& payload);
    // After a successful send: the mirror becomes payload with every pulse signal back at idle.
    void storeSentPayloadLocked(TxFrame& frame, const std::vector<std::uint8_t>& payload);
    void loadFrameStates();
    void persistFrameStates();
    void requireInterfaceUsable() const;
    std::size_t flushStagedWrites(std::vector<StagedWrite>& group);
    void finishWrite(StagedWrite& staged, std::size_t& processed);
    void receiveLoop();
    void writebackLoop();
    void persistLoop();
    void flushImmediatePersistentSamples();
    bool priorityControlBlocked(std::int64_t nowMs) const;
    void publishStatusEvent(const std::string& event, std::int64_t ts, const std::string& detailsJson = std::string()) const;
    void publishPointValue(const RuntimeDevice& device, const PointDefinition& point, const DecodedValue& decoded, std::int64_t ts,
        int quality = 1, const std::string& qualityMsg = "ok");
    void publishOnlinePoint(RuntimeDevice& device, bool online, std::int64_t wallMs, std::int64_t monoMs);
    void publishInterfacePoints(std::int64_t wallMs, std::int64_t monoMs);
    void updatePointStaleness(std::int64_t wallMs, std::int64_t monoMs);
    static std::int64_t nowMs();

    DeviceConfig config_;
    MemoryPointStore& store_;
    SqliteSampleWriter sqliteWriter_;
    PriorityControlLease priorityControlLease_;
    PowerControlOwnership powerControlOwnership_;
    std::shared_ptr<IMqttPublisher> mqttPublisher_;
    std::vector<RuntimeDevice> runtimeDevices_;
    std::vector<RuntimePoint> runtimePoints_;
    std::unordered_map<std::uint32_t, std::size_t> indexToRuntimePoint_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> readPointsByFrame_;
    std::vector<std::size_t> interfacePoints_;  // read.dataType == "interface_online"
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> flushedImmediatePersistentGeneration_{0};
    std::mutex persistentFlushMutex_;
    mutable std::mutex socketMutex_;
    std::unordered_map<std::uint64_t, TxFrame> txFrames_;  // guarded by socketMutex_
    std::string frameStatePath_;
    std::mutex frameStateMutex_;
    std::int64_t lastFrameStateErrorMono_ = 0;  // guarded by frameStateMutex_
    // Set while the interface is known down / bus-off: writes are refused instead of being queued
    // in the kernel and sent whenever the bus recovers.
    std::atomic<bool> interfaceFault_{false};
    int socketFd_ = -1;
    // Receive-thread state (also updated by tests calling the public methods from one thread).
    bool interfaceOnline_ = false;
    bool busOff_ = false;
    bool errorPassive_ = false;
    bool linkDownSeen_ = false;
    std::int64_t lastBusActivityMono_ = 0;  // last data frame or controller restart
    std::int64_t lastInterfaceCheckMono_ = 0;
    std::int64_t lastInterfacePublishedMono_ = 0;
    std::thread receiveThread_;
    std::thread writebackThread_;
    std::thread persistThread_;
};

}  // namespace edge_gateway
