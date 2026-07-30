#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/iec_client.hpp"

namespace edge_gateway {

// AM5SE Ethernet mode broadcasts master presence over UDP, then accepts the
// TCP connection initiated by the protection device. It is intentionally
// separate from the generic outbound IEC103 TCP client.
class Am5seIec103Client final : public IecClient {
public:
    explicit Am5seIec103Client(IecProtocolConfig config);
    ~Am5seIec103Client() override;

    std::vector<IecDataValue> poll() override;
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
    void disconnect();
    void closeListener();

    IecProtocolConfig config_;
    std::intptr_t listenerSocket_ = -1;
    std::intptr_t socket_ = -1;
    std::vector<std::uint8_t> rxBuffer_;
    bool frameCountBit_ = false;
    bool linkInitialized_ = false;
    bool generalInterrogationDone_ = false;
    std::int64_t lastDiscoveryMs_ = 0;
    std::function<void(const std::string&)> recordingProgressCallback_;
    mutable std::mutex operationMutex_;
};

}  // namespace edge_gateway
