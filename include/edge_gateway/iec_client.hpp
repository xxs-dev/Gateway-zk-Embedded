#pragma once

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "edge_gateway/iec_codec.hpp"
#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

class IecClient {
public:
    virtual ~IecClient() = default;

    virtual std::vector<IecDataValue> poll() = 0;
    virtual std::vector<IecDataValue> drainBufferedValues() {
        return {};
    }
    virtual CommandResult writeByPoint(
        const PointDefinition& point,
        double value,
        const std::string& cmdId,
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    );
    virtual void synchronizeClock(std::int64_t nowMs);
    virtual IecParameterValue readParameter(int ioa, int typeId, std::uint8_t qualifier, int timeoutMs);
    virtual CommandResult writeParameter(int ioa, int typeId, double value, std::uint8_t qualifier, int timeoutMs);
    virtual CommandResult activateParameter(int ioa, std::uint8_t qualifier, int timeoutMs);
    virtual std::vector<IecFileSegment> callFile(int ioa, int nameOfFile, int nameOfSection, std::uint8_t qualifier, int timeoutMs);
    virtual std::vector<Iec103DisturbanceRecord> listDisturbanceRecords(int timeoutMs);
    virtual Iec103ComtradeFiles pullComtradeRecording(
        int fan,
        const std::string& outputDirectory,
        int timeoutMs
    );
    virtual void setRecordingProgressCallback(
        std::function<void(const std::string&)> callback
    );
};

// IEC 60870-5-104 controlling station (TCP).
class Iec104Client : public IecClient {
public:
    Iec104Client(TcpTransportConfig tcp, IecProtocolConfig iec);
    ~Iec104Client() override;

    std::vector<IecDataValue> poll() override;
    std::vector<IecDataValue> drainBufferedValues() override;
    CommandResult writeByPoint(
        const PointDefinition& point,
        double value,
        const std::string& cmdId,
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) override;
    // Sends C_CS_NA_1 and waits (t1) for its ACT_CON; throws on timeout or negative reply.
    void synchronizeClock(std::int64_t nowMs) override;
    IecParameterValue readParameter(int ioa, int typeId, std::uint8_t qualifier, int timeoutMs) override;
    CommandResult writeParameter(int ioa, int typeId, double value, std::uint8_t qualifier, int timeoutMs) override;
    CommandResult activateParameter(int ioa, std::uint8_t qualifier, int timeoutMs) override;
    std::vector<IecFileSegment> callFile(int ioa, int nameOfFile, int nameOfSection, std::uint8_t qualifier, int timeoutMs) override;

private:
    enum class Confirmation { Confirmed, Negative, Timeout };

    // GI (C_IC_NA_1) / counter interrogation (C_CI_NA_1) progress, guarded by stateMutex_.
    struct SystemCommandState {
        bool inProgress = false;
        bool confirmed = false;
        std::int64_t sentMs = 0;
        std::int64_t nextDueMs = 0;
    };

    void ensureConnected();
    void ensureIec104Started();
    void startReceiveLoop();
    void stopReceiveLoop();
    void receiveLoop();
    void disconnect(std::uint64_t expectedGeneration = 0);
    std::intptr_t duplicateConnectedSocket(std::uint64_t& generation) const;
    void configureSocketTimeouts(std::intptr_t socketHandle) const;
    // *handedToSocket is set right before the frame is written, so callers can tell a frame
    // that never left (window/t1 failure) from one that may have reached the station.
    void sendIec104IFrame(const std::vector<std::uint8_t>& bytes, bool* handedToSocket = nullptr);
    void sendAll(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> readSome(int timeoutMs);
    void handleIec104Frame(const std::vector<std::uint8_t>& frame);
    void handleSystemCommandReply(const IecAsdu& asdu);
    void sendDueSystemCommands(std::int64_t steadyNowMs);
    void bufferValues(const std::vector<IecDataValue>& values);
    std::vector<IecDataValue> drainBufferedValuesLocked();
    bool waitForIec104Start(int timeoutMs);
    // expectedSelect: 1 = select (S/E=1) echo, 0 = execute echo, -1 = do not check.
    // Negative: matching reply with P/N=1, or COT 44..47 (unknown type/cause/CA/IOA).
    Confirmation waitForControlConfirmation(const PointDefinition& point, double value, int expectedCause,
        int timeoutMs, int expectedSelect, std::string* detail);
    void clearControlConfirmations(const PointDefinition& point);
    void sendDeselect(const PointDefinition& point, double value);
    bool waitForParameterConfirmation(int ioa, int typeId, int expectedCause, int timeoutMs, IecParameterValue* value = nullptr);
    void clearParameterConfirmations(int ioa, int typeId);
    // Returns false (and changes nothing) when N(R) acknowledges frames that
    // were never sent, i.e. it lies outside [last acknowledged, V(S)].
    bool noteRemoteAcknowledgeLocked(std::uint16_t receiveSequence);
    bool t1SupervisionExpired(std::int64_t nowMsValue);
    std::vector<IecFileSegment> waitForFileSegments(int nameOfFile, int timeoutMs);
    // Throws (after closing the connection) on an APCI framing error.
    std::vector<std::vector<std::uint8_t>> drainIec104Frames(int timeoutMs, int maxFrames);

    TcpTransportConfig tcp_;
    IecProtocolConfig iec_;
    std::intptr_t socket_ = -1;
    std::uint16_t sendSequence_ = 0;
    std::uint16_t receiveSequence_ = 0;
    std::uint16_t remoteReceiveSequence_ = 0;
    bool iec104Started_ = false;
    // rxBuffer_ is only touched while holding rxMutex_ (by the thread that
    // drains frames). Other threads request a reset through rxResetRequested_.
    std::vector<std::uint8_t> rxBuffer_;
    std::mutex rxMutex_;
    std::atomic<bool> rxResetRequested_{false};
    // Serialises I-frame senders so N(S) is allocated in wire order.
    std::mutex iFrameMutex_;
    // Send time of every unacknowledged I-frame (t1 supervision).
    std::deque<std::pair<std::uint16_t, std::int64_t>> unackedIFrames_;
    std::int64_t testFrPendingSinceMs_ = 0;
    SystemCommandState interrogation_;
    SystemCommandState counterInterrogation_;
    // C_CS_NA_1 reply: 0 = none yet, 1 = positive ACT_CON, -1 = negative.
    int clockSyncReply_ = 0;
    std::atomic<bool> receiveRunning_{false};
    std::atomic<bool> connected_{false};
    std::thread receiveThread_;
    mutable std::mutex socketMutex_;
    std::uint64_t socketGeneration_ = 0;
    mutable std::mutex stateMutex_;
    std::mutex sendMutex_;
    std::condition_variable stateChanged_;
    std::vector<IecDataValue> bufferedValues_;
    std::vector<IecDataValue> controlValues_;
    std::vector<IecParameterValue> parameterValues_;
    std::vector<IecFileSegment> fileSegments_;
    std::int64_t lastReceiveMs_ = 0;
    std::int64_t lastSendMs_ = 0;
    std::int64_t lastAckMs_ = 0;
    std::uint16_t unacknowledgedReceived_ = 0;
};

}  // namespace edge_gateway
