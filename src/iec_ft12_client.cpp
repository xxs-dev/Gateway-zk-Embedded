#include "edge_gateway/iec_ft12_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace edge_gateway {

namespace {

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();
// A GI/CI without termination blocks the next one for at least this long (or one period).
constexpr std::int64_t kUnterminatedRetryMs = 300000;

bool commandDue(std::int64_t nextDueMs, bool inProgress, std::int64_t sentMs, int intervalMs, std::int64_t nowMs) {
    if (nowMs < nextDueMs) {
        return false;
    }
    return !inProgress || nowMs - sentMs >= std::max<std::int64_t>(intervalMs, kUnterminatedRetryMs);
}

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string normalizedProtocol(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// GATEWAY_IEC103_DEBUG=1 (same convention as GATEWAY_DLT645_DEBUG): log every IEC 103 change
// frame (ASDU 1/2) with its raw octets, raw DPI and CP32Time2a fields, so that the DPI can be
// compared with the real breaker position on site. Off by default.
bool iec103DebugEnabled() {
    const char* value = std::getenv("GATEWAY_IEC103_DEBUG");
    return value != nullptr && std::string(value) != "0" && std::string(value) != "false";
}

void logIec103ChangeFrame(const std::vector<std::uint8_t>& asdu, const std::vector<IecDataValue>& decoded) {
    const std::size_t timeOffset = 4 + 3 + (asdu[0] == 2 ? 4 : 0);
    if (asdu.size() < timeOffset + 4) {
        std::cerr << "IEC103 change frame (truncated) raw=" << IecCodec::toHex(asdu) << std::endl;
        return;
    }
    const int ms = asdu[timeOffset] | (asdu[timeOffset + 1] << 8);
    std::ostringstream line;
    line << "IEC103 change frame ASDU" << static_cast<int>(asdu[0])
         << " COT=" << static_cast<int>(asdu[2]) << " CA=" << static_cast<int>(asdu[3])
         << " FUN=" << static_cast<int>(asdu[4]) << " INF=" << static_cast<int>(asdu[5])
         << " DPI=" << (asdu[6] & 0x03)
         << " CP32=" << std::setfill('0') << std::setw(2) << (asdu[timeOffset + 3] & 0x1F)
         << ':' << std::setw(2) << (asdu[timeOffset + 2] & 0x3F)
         << ':' << std::setw(2) << ms / 1000 << '.' << std::setw(3) << ms % 1000
         << " IV=" << ((asdu[timeOffset + 2] & 0x80) != 0 ? 1 : 0)
         << " SU=" << ((asdu[timeOffset + 3] & 0x80) != 0 ? 1 : 0)
         << " timestampMs=" << (decoded.empty() ? 0 : decoded.front().timestampMs)
         << " raw=" << IecCodec::toHex(asdu);
    std::cerr << line.str() << std::endl;
}

Ft12Profile profileOf(const std::string& protocolType) {
    if (protocolType == "iec101") {
        return Ft12Profile::Iec101;
    }
    if (protocolType == "iec103" || protocolType == "iec103_serial" || protocolType == "iec103_tcp") {
        return Ft12Profile::Iec103;
    }
    throw std::invalid_argument("FT1.2 client does not support protocol.type=" + protocolType);
}

}  // namespace

Ft12LinkOptions ft12LinkOptions(Ft12Profile profile, const IecProtocolConfig& config) {
    Ft12LinkOptions options;
    options.linkAddress = config.linkAddress;
    options.linkAddressSize = config.linkAddressSize;
    options.responseTimeoutMs = config.pollTimeoutMs;
    options.retransmissions = 2;
    options.requestStatusBeforeReset = profile == Ft12Profile::Iec101;
    return options;
}

Ft12PollSession::Ft12PollSession(Ft12Profile profile, IecProtocolConfig config, Ft12UnbalancedMaster& master)
    : profile_(profile),
      config_(std::move(config)),
      master_(master),
      debugChangeFrames_(profile == Ft12Profile::Iec103 && iec103DebugEnabled()) {
}

Ft12Profile Ft12PollSession::profile() const {
    return profile_;
}

std::string Ft12PollSession::name() const {
    return profile_ == Ft12Profile::Iec101 ? "IEC101" : "IEC103";
}

bool Ft12PollSession::generalInterrogationInProgress() const {
    return interrogationInProgress_;
}

void Ft12PollSession::ensureLink() {
    if (!master_.linkReady()) {
        master_.resetLink();
    }
    observeLinkEpoch(steadyNowMs());
}

void Ft12PollSession::observeLinkEpoch(std::int64_t nowMs) {
    if (master_.linkEpoch() == seenEpoch_) {
        return;
    }
    // A reset link (new connection or recovery after timeouts) starts over with a GI.
    seenEpoch_ = master_.linkEpoch();
    dataSinceLink_ = false;
    interrogationInProgress_ = false;
    counterInProgress_ = false;
    nextInterrogationMs_ = config_.pollOnCollect ? nowMs : kNever;
    nextCounterMs_ = profile_ == Ft12Profile::Iec101 && config_.counterInterrogation ? nowMs : kNever;
}

bool Ft12PollSession::interrogationDue(std::int64_t nowMs) const {
    return commandDue(nextInterrogationMs_, interrogationInProgress_, interrogationSentMs_,
        config_.interrogationIntervalMs, nowMs);
}

bool Ft12PollSession::counterInterrogationDue(std::int64_t nowMs) const {
    return !interrogationInProgress_ &&
        commandDue(nextCounterMs_, counterInProgress_, counterSentMs_, config_.counterInterrogationIntervalMs, nowMs);
}

void Ft12PollSession::sendInterrogation(std::int64_t nowMs) {
    if (interrogationInProgress_) {
        std::cerr << name() << " general interrogation was not terminated; sending the next one" << std::endl;
    }
    const auto asdu = profile_ == Ft12Profile::Iec101
        ? IecCodec::buildIec101InterrogationAsdu(config_)
        : IecCodec::buildIec103GeneralInterrogationAsdu(config_);
    const auto reply = master_.sendConfirm(asdu);
    if (reply.nack()) {
        // Station busy: the GI stays due and is retried in the next cycle.
        return;
    }
    interrogationInProgress_ = true;
    interrogationSentMs_ = nowMs;
    nextInterrogationMs_ = config_.interrogationIntervalMs > 0 ? nowMs + config_.interrogationIntervalMs : kNever;
}

void Ft12PollSession::sendCounterInterrogation(std::int64_t nowMs) {
    const auto reply = master_.sendConfirm(IecCodec::buildIec101CounterInterrogationAsdu(config_));
    if (reply.nack()) {
        return;
    }
    counterInProgress_ = true;
    counterSentMs_ = nowMs;
    nextCounterMs_ = config_.counterInterrogationIntervalMs > 0
        ? nowMs + config_.counterInterrogationIntervalMs
        : kNever;
}

void Ft12PollSession::noteSystemCommandReply(int typeId, int cause, bool negative) {
    bool& inProgress = typeId == 100 ? interrogationInProgress_ : counterInProgress_;
    const char* what = typeId == 100 ? "general interrogation" : "counter interrogation";
    if (cause == kIecCauseActivationTermination) {
        inProgress = false;
    } else if (cause == kIecCauseActivationConfirmation && negative) {
        inProgress = false;
        std::cerr << name() << " " << what << " rejected (negative ACT_CON)" << std::endl;
    } else if (cause >= kIecCauseUnknownType && cause <= kIecCauseUnknownObjectAddress) {
        inProgress = false;
        std::cerr << name() << " " << what << " rejected (COT " << cause << ")" << std::endl;
    }
}

void Ft12PollSession::absorb(const std::vector<std::uint8_t>& asdu, std::vector<IecDataValue>& out) {
    if (asdu.empty()) {
        return;
    }
    dataSinceLink_ = true;
    try {
        if (profile_ == Ft12Profile::Iec103) {
            if (asdu[0] == 8) {
                // ASDU 8: termination of general interrogation.
                interrogationInProgress_ = false;
                return;
            }
            const auto values = IecCodec::decodeIec103Data(asdu, config_);
            if (debugChangeFrames_ && (asdu[0] == 1 || asdu[0] == 2)) {
                logIec103ChangeFrame(asdu, values);
            }
            out.insert(out.end(), values.begin(), values.end());
            return;
        }
        const auto decoded = IecCodec::decodeAsdu(asdu, config_);
        if (decoded.typeId == 100 || decoded.typeId == 101) {
            noteSystemCommandReply(decoded.typeId, decoded.cause, decoded.negative);
            return;
        }
        out.insert(out.end(), decoded.values.begin(), decoded.values.end());
        for (const auto& event : decoded.protectionEvents) {
            out.push_back(IecCodec::protectionEventValue(event));
        }
    } catch (const std::exception& ex) {
        std::cerr << name() << " ASDU decode failed: " << ex.what() << std::endl;
    }
}

std::vector<IecDataValue> Ft12PollSession::poll() {
    ensureLink();
    const auto nowMs = steadyNowMs();
    std::vector<IecDataValue> values;
    // DFC=1: the station cannot accept further user data; only request data this cycle.
    if (!master_.dataFlowControl()) {
        if (interrogationDue(nowMs)) {
            sendInterrogation(nowMs);
        } else if (counterInterrogationDue(nowMs)) {
            sendCounterInterrogation(nowMs);
        }
    }
    const int budget = std::max(1, config_.maxPollFrames);
    bool class2Polled = false;
    for (int exchange = 0; exchange < budget; ++exchange) {
        int dataClass = 0;
        if (master_.accessDemand()) {
            dataClass = 1;
        } else if (!class2Polled || (config_.pollOnCollect && !dataSinceLink_)) {
            // Class 2 once per cycle; a station still preparing its first GI answer is polled
            // until it sets ACD or the frame budget is used up.
            dataClass = 2;
            class2Polled = true;
        } else {
            break;
        }
        const auto reply = master_.requestClassData(dataClass);
        if (reply.hasUserData()) {
            absorb(reply.asdu, values);
        }
    }
    if (config_.pollOnCollect && !dataSinceLink_) {
        throw std::runtime_error(name() + " station acknowledged the link but delivered no application data");
    }
    return values;
}

IecFt12Client::IecFt12Client(std::string protocolType, std::shared_ptr<ISerialPort> serialPort, IecProtocolConfig iec)
    : protocolType_(normalizedProtocol(std::move(protocolType))),
      iec_(std::move(iec)),
      serialPort_(std::move(serialPort)),
      tcpTransport_(false),
      master_(ft12LinkOptions(profileOf(protocolType_), iec_)),
      session_(profileOf(protocolType_), iec_, master_) {
    if (!serialPort_) {
        throw std::invalid_argument("IEC serial port is required");
    }
    if (protocolType_ == "iec103_tcp") {
        throw std::invalid_argument("protocol.type=iec103_tcp requires a TCP transport");
    }
}

IecFt12Client::IecFt12Client(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec)
    : protocolType_(normalizedProtocol(std::move(protocolType))),
      iec_(std::move(iec)),
      tcp_(std::move(tcp)),
      tcpTransport_(true),
      master_(ft12LinkOptions(profileOf(protocolType_), iec_)),
      session_(profileOf(protocolType_), iec_, master_) {
    if (protocolType_ == "iec101" || protocolType_ == "iec103_serial") {
        throw std::invalid_argument("protocol.type=" + protocolType_ + " requires a serial transport");
    }
}

IecFt12Client::~IecFt12Client() {
    master_.detach();
}

void IecFt12Client::ensureTransport() {
    if (transport_) {
        return;
    }
    if (tcpTransport_) {
        transport_ = SocketFt12Transport::connect(tcp_.host, tcp_.port, tcp_.connectTimeoutMs);
    } else {
        transport_.reset(new SerialFt12Transport(serialPort_));
    }
    master_.attach(transport_.get());
}

void IecFt12Client::dropTransport() {
    master_.detach();
    transport_.reset();
    if (serialPort_) {
        try {
            if (serialPort_->isOpen()) {
                serialPort_->close();
            }
        } catch (const std::exception&) {
        }
    }
}

std::vector<IecDataValue> IecFt12Client::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureTransport();
    try {
        return session_.poll();
    } catch (const Ft12TransportError&) {
        dropTransport();
        throw;
    }
}

}  // namespace edge_gateway
