#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_ft12_client.hpp"
#include "edge_gateway/iec103_soe.hpp"
#include <chrono>

namespace edge_gateway {

// AM5SE Ethernet mode supports both device-initiated reverse TCP and the device's built-in
// TCP server. Link layer and polling are the shared unbalanced FT1.2 master; this class adds
// the connection modes and the COMTRADE recording transfer. Events that arrive while a
// recording is transferred are kept and delivered through drainBufferedValues().
class Am5seIec103Client final : public IecClient {
public:
    explicit Am5seIec103Client(IecProtocolConfig config, TcpTransportConfig tcp = {});
    ~Am5seIec103Client() override;

    // Does not wait for a running recording transfer; that transfer keeps polling the
    // station and buffers its events.
    std::vector<IecDataValue> poll() override;
    void setSoeSink(std::function<void(const std::vector<Iec103SoeEvent>&)> sink);
    void setSoeRecordingSinks(
        std::function<void(const std::vector<Iec103DisturbanceRecord>&, bool, bool, std::int64_t)> directory,
        std::function<void(const Iec103DisturbanceRecord&, const Iec103ComtradeFiles&, std::int64_t)> downloaded);
    std::vector<IecDataValue> drainBufferedValues() override;
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
    void sendDiscoveryIfDue(bool force = false);
    void sendRecordingRequest(const std::vector<std::uint8_t>& asdu, const char* what);
    // Class-1 reply during a recording operation: keeps non-recording ASDUs as events.
    void keepEvents(const Ft12Reply& reply);
    void pollClass2IfDue(std::int64_t& lastClass2Ms);
    std::vector<std::uint8_t> pullComtradeFile(int fan, int fileType, std::int64_t deadlineMs);
    void disconnect();
    void closeListener();
    void captureSoe(const std::vector<std::vector<std::uint8_t>>& frames);
    void flushSoe();
    std::vector<Iec103DisturbanceRecord> listRecordingsLocked(int timeoutMs, bool& complete);
    void checkRecordingsIfDue();

    IecProtocolConfig config_;
    TcpTransportConfig tcp_;
    std::intptr_t listenerSocket_ = -1;
    std::unique_ptr<Ft12Transport> transport_;
    Ft12UnbalancedMaster master_;
    Ft12PollSession session_;
    std::int64_t lastDiscoveryMs_ = 0;
    std::function<void(const std::string&)> recordingProgressCallback_;
    std::mutex operationMutex_;
    std::function<void(const std::vector<Iec103SoeEvent>&)> soeSink_;
    std::vector<Iec103SoeEvent> pendingSoe_;
    std::string soeSession_;
    std::uint64_t soeSequence_ = 0;
    std::function<void(const std::vector<Iec103DisturbanceRecord>&, bool, bool, std::int64_t)> directorySink_;
    std::function<void(const Iec103DisturbanceRecord&, const Iec103ComtradeFiles&, std::int64_t)> downloadSink_;
    int recordingChecksRemaining_ = 0;
    std::chrono::steady_clock::time_point nextRecordingCheck_{};
    std::mutex bufferMutex_;
    std::vector<IecDataValue> bufferedValues_;
};

}  // namespace edge_gateway
