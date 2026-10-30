#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/iec_ft12_link.hpp"

namespace edge_gateway {

enum class Ft12Profile { Iec101, Iec103 };

Ft12LinkOptions ft12LinkOptions(Ft12Profile profile, const IecProtocolConfig& config);

// Application side of an unbalanced FT1.2 master: link start-up, general interrogation
// (IEC 101 C_IC_NA_1 COT 6 / IEC 103 ASDU 7 COT 9), IEC 101 counter interrogation, and the
// class 2 poll followed by class 1 requests while the station signals ACD.
class Ft12PollSession {
public:
    Ft12PollSession(Ft12Profile profile, IecProtocolConfig config, Ft12UnbalancedMaster& master);

    // One acquisition cycle. Throws Ft12LinkError / Ft12TransportError on link failure, and
    // std::runtime_error while a station with general interrogation enabled has delivered no
    // application data since the link was (re)established (E5/ACK alone is not an acquisition).
    std::vector<IecDataValue> poll();
    // Resets the link if needed and runs the start-up bookkeeping (used before recording I/O).
    void ensureLink();
    // Decodes one ASDU of a class 1/2 reply into point values and tracks GI/CI termination.
    void absorb(const std::vector<std::uint8_t>& asdu, std::vector<IecDataValue>& out);
    bool generalInterrogationInProgress() const;
    Ft12Profile profile() const;

private:
    void observeLinkEpoch(std::int64_t nowMs);
    bool interrogationDue(std::int64_t nowMs) const;
    bool counterInterrogationDue(std::int64_t nowMs) const;
    void sendInterrogation(std::int64_t nowMs);
    void sendCounterInterrogation(std::int64_t nowMs);
    void noteSystemCommandReply(int typeId, int cause, bool negative);
    std::string name() const;

    Ft12Profile profile_;
    IecProtocolConfig config_;
    Ft12UnbalancedMaster& master_;
    std::uint64_t seenEpoch_ = 0;
    bool dataSinceLink_ = false;
    bool interrogationInProgress_ = false;
    std::int64_t interrogationSentMs_ = 0;
    std::int64_t nextInterrogationMs_ = 0;
    bool counterInProgress_ = false;
    std::int64_t counterSentMs_ = 0;
    std::int64_t nextCounterMs_ = 0;
    // GATEWAY_IEC103_DEBUG: log raw IEC 103 change frames (ASDU 1/2).
    bool debugChangeFrames_ = false;
};

// IEC 101 (serial) and generic IEC 103 (serial or TCP) unbalanced master.
class IecFt12Client final : public IecClient {
public:
    IecFt12Client(std::string protocolType, std::shared_ptr<ISerialPort> serialPort, IecProtocolConfig iec);
    IecFt12Client(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec);
    ~IecFt12Client() override;

    std::vector<IecDataValue> poll() override;

private:
    void ensureTransport();
    void dropTransport();

    std::string protocolType_;
    IecProtocolConfig iec_;
    std::shared_ptr<ISerialPort> serialPort_;
    TcpTransportConfig tcp_;
    bool tcpTransport_ = false;
    std::unique_ptr<Ft12Transport> transport_;
    Ft12UnbalancedMaster master_;
    Ft12PollSession session_;
    std::mutex mutex_;
};

}  // namespace edge_gateway
