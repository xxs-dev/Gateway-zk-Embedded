#pragma once

// IEC 60870-5-1/-2 FT1.2 link layer, unbalanced transmission, primary (master) station.
// Shared by IEC 101, IEC 103 (serial and TCP) and the AM5SE IEC 103 network client.

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edge_gateway/serial_port.hpp"

namespace edge_gateway {

// The transport (serial line or TCP connection) failed; the connection must be re-established.
class Ft12TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The secondary station did not answer after all retransmissions, or reported the link
// service as not functioning. The link must be reset before further user data.
class Ft12LinkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Byte stream carrying FT1.2 frames.
class Ft12Transport {
public:
    virtual ~Ft12Transport() = default;
    // Throws Ft12TransportError.
    virtual void write(const std::vector<std::uint8_t>& bytes) = 0;
    // Bytes that arrive within timeoutMs (0 = only what is already buffered); empty on
    // timeout. Throws Ft12TransportError when the transport is lost.
    virtual std::vector<std::uint8_t> read(int timeoutMs) = 0;
};

class SerialFt12Transport final : public Ft12Transport {
public:
    explicit SerialFt12Transport(std::shared_ptr<ISerialPort> port);

    void write(const std::vector<std::uint8_t>& bytes) override;
    std::vector<std::uint8_t> read(int timeoutMs) override;

private:
    void ensureOpen();

    std::shared_ptr<ISerialPort> port_;
};

// Owns a connected stream socket and closes it on destruction.
class SocketFt12Transport final : public Ft12Transport {
public:
    explicit SocketFt12Transport(std::intptr_t socketHandle);
    ~SocketFt12Transport() override;

    SocketFt12Transport(const SocketFt12Transport&) = delete;
    SocketFt12Transport& operator=(const SocketFt12Transport&) = delete;

    // Connects to host:port within timeoutMs; throws Ft12TransportError on failure.
    static std::unique_ptr<SocketFt12Transport> connect(const std::string& host, int port, int timeoutMs);

    void write(const std::vector<std::uint8_t>& bytes) override;
    std::vector<std::uint8_t> read(int timeoutMs) override;

private:
    std::intptr_t socket_;
};

struct Ft12Frame {
    bool singleCharacter = false;  // E5
    bool variable = false;         // 68 L L 68 C A ASDU CS 16
    std::uint8_t control = 0;
    int address = -1;
    std::vector<std::uint8_t> asdu;  // variable frames: user data after control and address
    std::vector<std::uint8_t> raw;
};

// Incremental FT1.2 frame parser; skips bytes that do not start a valid frame.
class Ft12FrameParser {
public:
    explicit Ft12FrameParser(int linkAddressSize);

    void append(const std::vector<std::uint8_t>& bytes);
    bool next(Ft12Frame& frame);
    void clear();

private:
    int linkAddressSize_;
    std::vector<std::uint8_t> buffer_;
};

std::vector<std::uint8_t> buildFt12FixedFrame(std::uint8_t control, int linkAddress, int linkAddressSize);
std::vector<std::uint8_t> buildFt12VariableFrame(
    std::uint8_t control,
    int linkAddress,
    int linkAddressSize,
    const std::vector<std::uint8_t>& asdu
);

struct Ft12LinkOptions {
    int linkAddress = 1;
    int linkAddressSize = 1;
    // Wait for the reply to one request before retransmitting it.
    int responseTimeoutMs = 1000;
    // Identical retransmissions (same FCB) after a missing reply before the link is declared down.
    int retransmissions = 2;
    // IEC 101 link initialisation: request status of link (FC 9) before reset of remote link.
    bool requestStatusBeforeReset = false;
};

struct Ft12Reply {
    bool singleCharacter = false;  // E5: ACK, or "no data" for a class request
    int function = -1;             // secondary function code (-1 for E5)
    bool accessDemand = false;     // ACD: class 1 data available
    bool dataFlowControl = false;  // DFC: no further user data accepted
    std::vector<std::uint8_t> asdu;

    bool nack() const { return function == 1; }
    bool hasUserData() const { return function == 8 && !asdu.empty(); }
};

class Ft12UnbalancedMaster {
public:
    explicit Ft12UnbalancedMaster(Ft12LinkOptions options);

    // A new or re-established transport: the link has to be reset before user data.
    void attach(Ft12Transport* transport);
    void detach();
    bool attached() const;
    bool linkReady() const;
    // Incremented by every successful link reset; lets the application redo its start-up
    // (general interrogation) after a link reset caused by timeouts.
    std::uint64_t linkEpoch() const;
    void markLinkDown();

    // [Request status of link (FC 9) -> status (FC 11),] reset of remote link / reset CU
    // (FC 0) -> ACK. Afterwards the next FCV=1 frame carries FCB=1. Throws Ft12LinkError.
    void resetLink();
    // SEND/CONFIRM user data (FC 3, FCV=1, FCB toggled). Returns ACK or NACK.
    Ft12Reply sendConfirm(const std::vector<std::uint8_t>& asdu);
    // REQUEST/RESPOND class 1 (FC 10) or class 2 (FC 11) data. Returns user data (FC 8) or
    // "no data" (FC 9 or E5; a fixed ACK FC 0 is also accepted as a temporary compatibility
    // branch for the field AM5 relay, see Ft12UnbalancedMaster::acceptable).
    Ft12Reply requestClassData(int dataClass);
    // ACD / DFC of the last accepted reply.
    bool accessDemand() const;
    bool dataFlowControl() const;
    const Ft12LinkOptions& options() const;

    // Application persistence runs before the next exchange and on accepted replies only.
    // A failing sink must not advance FCB or acknowledge another event.
    void setExchangeCallbacks(std::function<void()> before,
        std::function<void(const std::vector<std::uint8_t>&)> accepted);
    // Absolute steady-clock deadline for a bounded multi-exchange operation; 0 clears it.
    void setOperationDeadline(std::int64_t deadlineMs);

private:
    enum class Expect { Confirm, Status, Respond };

    Ft12Reply exchange(std::uint8_t function, bool frameCount, const std::vector<std::uint8_t>* asdu,
        Expect expect, const char* service);
    bool awaitReply(Expect expect, Ft12Reply& reply, std::vector<std::uint8_t>& raw);
    bool acceptable(const Ft12Frame& frame, Expect expect);
    void discardStaleInput();
    Ft12Transport& transport();

    Ft12LinkOptions options_;
    Ft12Transport* transport_ = nullptr;
    Ft12FrameParser parser_;
    bool linkReady_ = false;
    bool frameCountBit_ = false;
    bool accessDemand_ = false;
    bool dataFlowControl_ = false;
    std::uint64_t linkEpoch_ = 0;
    // Retransmitted requests may each be answered: the identical late replies of the last
    // exchange are recognised and dropped instead of being taken as the next reply.
    std::vector<std::uint8_t> lastReplyRaw_;
    int pendingDuplicates_ = 0;
    std::function<void()> beforeExchange_;
    std::function<void(const std::vector<std::uint8_t>&)> acceptedReply_;
    std::int64_t operationDeadlineMs_ = 0;
};

}  // namespace edge_gateway
