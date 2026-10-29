#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec103_soe.hpp"

namespace edge_gateway {

// AM5SE Ethernet mode supports both device-initiated reverse TCP and the
// device's built-in TCP server. Both modes share the IEC103 link, class poll,
// and COMTRADE recording implementation.
class Am5seIec103Client final : public IecClient {
public:
    explicit Am5seIec103Client(IecProtocolConfig config, TcpTransportConfig tcp = {});
    ~Am5seIec103Client() override;

    std::vector<IecDataValue> poll() override;
    void setSoeSink(std::function<void(const std::vector<Iec103SoeEvent>&)> sink);
    void setSoeRecordingSinks(
        std::function<void(const std::vector<Iec103DisturbanceRecord>&, bool, bool, std::int64_t)> directory,
        std::function<void(const Iec103DisturbanceRecord&, const Iec103ComtradeFiles&, std::int64_t)> downloaded);
    std::vector<Iec103DisturbanceRecord> listDisturbanceRecords(int timeoutMs) override;
    Iec103ComtradeFiles pullComtradeRecording(
        int fan,
        const std::string& outputDirectory,
        int timeoutMs
    ) override;
    void setRecordingProgressCallback(
        std::function<void(const std::string&)> callback
    ) override;

private:
    void ensureConnected();
    void ensureListener();
    void acceptReverseConnection();
    void connectToDeviceServer();
    void initializeLink();
    void sendDiscoveryIfDue(bool force = false);
    void sendAll(const std::vector<std::uint8_t>& bytes);
    std::vector<std::vector<std::uint8_t>> readFrames(int timeoutMs, int maxFrames);
    std::vector<std::vector<std::uint8_t>> exchange(
        const std::vector<std::uint8_t>& request,
        int timeoutMs,
        int maxFrames = 1
    );
    std::vector<std::vector<std::uint8_t>> requestClassData(int dataClass, int timeoutMs);
    std::vector<IecDataValue> decodeDataFrames(
        const std::vector<std::vector<std::uint8_t>>& frames
    ) const;
    std::vector<IecDataValue> performGeneralInterrogation(int timeoutMs);
    std::vector<std::uint8_t> pullComtradeFile(int fan, int fileType, int timeoutMs);
    bool nextFrameCountBit();
    // Sends an FCB-carrying request; on response timeout retransmits the
    // identical frame (same FCB) before resetting the link (IEC 60870-5-2).
    std::vector<std::vector<std::uint8_t>> exchangeWithFrameCount(
        const std::function<std::vector<std::uint8_t>(bool)>& build,
        int timeoutMs,
        int maxFrames = 1
    );
    void disconnect();
    void closeListener();
    void captureSoe(const std::vector<std::vector<std::uint8_t>>& frames);
    void flushSoe();
    std::vector<Iec103DisturbanceRecord> listRecordingsLocked(int timeoutMs, bool& complete);
    void checkRecordingsIfDue();

    IecProtocolConfig config_;
    TcpTransportConfig tcp_;
    std::intptr_t listenerSocket_ = -1;
    std::intptr_t socket_ = -1;
    std::vector<std::uint8_t> rxBuffer_;
    bool frameCountBit_ = false;
    bool linkInitialized_ = false;
    bool generalInterrogationDone_ = false;
    std::int64_t lastDiscoveryMs_ = 0;
    std::function<void(const std::string&)> recordingProgressCallback_;
    mutable std::mutex operationMutex_;
    std::function<void(const std::vector<Iec103SoeEvent>&)> soeSink_;
    std::vector<Iec103SoeEvent> pendingSoe_;
    std::string soeSession_;
    std::uint64_t soeSequence_ = 0;
    std::function<void(const std::vector<Iec103DisturbanceRecord>&, bool, bool, std::int64_t)> directorySink_;
    std::function<void(const Iec103DisturbanceRecord&, const Iec103ComtradeFiles&, std::int64_t)> downloadSink_;
    int recordingChecksRemaining_ = 0;
    std::chrono::steady_clock::time_point nextRecordingCheck_{}, directoryDeadline_{};
};

}  // namespace edge_gateway
