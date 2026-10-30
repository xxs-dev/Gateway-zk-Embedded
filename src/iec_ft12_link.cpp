#include "edge_gateway/iec_ft12_link.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "edge_gateway/channel_trace.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

constexpr std::uint8_t kPrm = 0x40;
constexpr std::uint8_t kFcb = 0x20;
constexpr std::uint8_t kFcv = 0x10;
constexpr std::uint8_t kAcd = 0x20;
constexpr std::uint8_t kDfc = 0x10;

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void closeSocket(SocketHandle socketHandle) {
    if (socketHandle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

bool setSocketBlocking(SocketHandle socketHandle, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0UL : 1UL;
    return ioctlsocket(socketHandle, FIONBIO, &mode) == 0;
#else
    const auto flags = fcntl(socketHandle, F_GETFL, 0);
    return flags >= 0 && fcntl(
        socketHandle,
        F_SETFL,
        blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

bool connectInProgress(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN;
#endif
}

bool waitConnected(SocketHandle socketHandle, int timeoutMs) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(socketHandle, &writeSet);
    fd_set errorSet;
    FD_ZERO(&errorSet);
    FD_SET(socketHandle, &errorSet);
    timeval timeout{};
    timeout.tv_sec = std::max(1, timeoutMs) / 1000;
    timeout.tv_usec = (std::max(1, timeoutMs) % 1000) * 1000;
#ifdef _WIN32
    const auto result = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
    const auto result = select(socketHandle + 1, nullptr, &writeSet, &errorSet, &timeout);
#endif
    if (result <= 0) {
        return false;
    }
    int socketError = 0;
#ifdef _WIN32
    int length = sizeof(socketError);
    return getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
        reinterpret_cast<char*>(&socketError), &length) == 0 && socketError == 0;
#else
    socklen_t length = sizeof(socketError);
    return getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 && socketError == 0;
#endif
}

std::uint8_t checksum(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end) {
    std::uint32_t sum = 0;
    for (auto i = begin; i < end && i < bytes.size(); ++i) {
        sum += bytes[i];
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

void appendAddress(std::vector<std::uint8_t>& bytes, int linkAddress, int linkAddressSize) {
    for (int i = 0; i < linkAddressSize; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(linkAddress) >> (8 * i)) & 0xFFU));
    }
}

int readAddress(const std::vector<std::uint8_t>& bytes, std::size_t offset, int linkAddressSize) {
    int address = 0;
    for (int i = 0; i < linkAddressSize; ++i) {
        address |= static_cast<int>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return address;
}

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

SerialFt12Transport::SerialFt12Transport(std::shared_ptr<ISerialPort> port) : port_(std::move(port)) {
    if (!port_) {
        throw std::invalid_argument("FT1.2 serial port is required");
    }
}

void SerialFt12Transport::ensureOpen() {
    if (!port_->isOpen()) {
        port_->open();
    }
}

void SerialFt12Transport::write(const std::vector<std::uint8_t>& bytes) {
    try {
        ensureOpen();
        port_->write(bytes);
    } catch (const std::exception& ex) {
        throw Ft12TransportError(std::string("FT1.2 serial write failed: ") + ex.what());
    }
}

std::vector<std::uint8_t> SerialFt12Transport::read(int timeoutMs) {
    try {
        ensureOpen();
        return port_->read(512, std::max(0, timeoutMs));
    } catch (const std::exception& ex) {
        throw Ft12TransportError(std::string("FT1.2 serial read failed: ") + ex.what());
    }
}

SocketFt12Transport::SocketFt12Transport(std::intptr_t socketHandle) : socket_(socketHandle) {
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        throw std::invalid_argument("FT1.2 socket transport requires a connected socket");
    }
}

SocketFt12Transport::~SocketFt12Transport() {
    const auto handle = static_cast<SocketHandle>(socket_);
#ifdef _WIN32
    shutdown(handle, SD_BOTH);
#else
    shutdown(handle, SHUT_RDWR);
#endif
    closeSocket(handle);
}

std::unique_ptr<SocketFt12Transport> SocketFt12Transport::connect(const std::string& host, int port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses) != 0) {
        throw Ft12TransportError("FT1.2 TCP cannot resolve " + host);
    }
    SocketHandle connected = kInvalidSocket;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        connected = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connected == kInvalidSocket) {
            continue;
        }
        if (!setSocketBlocking(connected, false)) {
            closeSocket(connected);
            connected = kInvalidSocket;
            continue;
        }
        const auto result = ::connect(connected, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if ((result != 0 && (!connectInProgress(lastSocketError()) || !waitConnected(connected, timeoutMs))) ||
            !setSocketBlocking(connected, true)) {
            closeSocket(connected);
            connected = kInvalidSocket;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    if (connected == kInvalidSocket) {
        throw Ft12TransportError("FT1.2 TCP connect failed: " + host + ":" + portText);
    }
    int keepAlive = 1;
#ifdef _WIN32
    setsockopt(connected, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
#else
    setsockopt(connected, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
#endif
    return std::unique_ptr<SocketFt12Transport>(new SocketFt12Transport(static_cast<std::intptr_t>(connected)));
}

void SocketFt12Transport::write(const std::vector<std::uint8_t>& bytes) {
    const auto handle = static_cast<SocketHandle>(socket_);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto result = send(handle, reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(bytes.size() - sent), 0);
#else
        const auto result = send(handle, bytes.data() + sent, bytes.size() - sent, kSendFlags);
#endif
        if (result <= 0) {
            throw Ft12TransportError("FT1.2 TCP send failed");
        }
        traceBytes(true, bytes.data() + sent, static_cast<std::size_t>(result));
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::uint8_t> SocketFt12Transport::read(int timeoutMs) {
    const auto handle = static_cast<SocketHandle>(socket_);
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(handle, &readSet);
    timeval timeout{};
    timeout.tv_sec = std::max(0, timeoutMs) / 1000;
    timeout.tv_usec = (std::max(0, timeoutMs) % 1000) * 1000;
#ifdef _WIN32
    const auto ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const auto ready = select(handle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready == 0) {
        return {};
    }
    if (ready < 0) {
#ifndef _WIN32
        if (errno == EINTR) {
            return {};
        }
#endif
        throw Ft12TransportError("FT1.2 TCP receive wait failed");
    }
    std::vector<std::uint8_t> chunk(1024);
#ifdef _WIN32
    const auto received = recv(handle, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
#else
    const auto received = recv(handle, chunk.data(), chunk.size(), 0);
#endif
    if (received == 0) {
        throw Ft12TransportError("FT1.2 TCP connection closed by peer");
    }
    if (received < 0) {
#ifndef _WIN32
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return {};
        }
#endif
        throw Ft12TransportError("FT1.2 TCP receive failed");
    }
    chunk.resize(static_cast<std::size_t>(received));
    traceBytes(false, chunk.data(), chunk.size());
    return chunk;
}

Ft12FrameParser::Ft12FrameParser(int linkAddressSize) : linkAddressSize_(std::max(0, linkAddressSize)) {
}

void Ft12FrameParser::append(const std::vector<std::uint8_t>& bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void Ft12FrameParser::clear() {
    buffer_.clear();
}

bool Ft12FrameParser::next(Ft12Frame& frame) {
    const auto fixedSize = static_cast<std::size_t>(4 + linkAddressSize_);
    while (!buffer_.empty()) {
        const auto start = std::find_if(buffer_.begin(), buffer_.end(), [](std::uint8_t byte) {
            return byte == 0x68 || byte == 0x10 || byte == 0xE5;
        });
        buffer_.erase(buffer_.begin(), start);
        if (buffer_.empty()) {
            return false;
        }
        if (buffer_.front() == 0xE5) {
            frame = Ft12Frame();
            frame.singleCharacter = true;
            frame.raw.assign(1, 0xE5);
            buffer_.erase(buffer_.begin());
            return true;
        }
        if (buffer_.front() == 0x10) {
            if (buffer_.size() < fixedSize) {
                return false;
            }
            const bool valid = buffer_[fixedSize - 1] == 0x16 &&
                buffer_[fixedSize - 2] == checksum(buffer_, 1, fixedSize - 2);
            if (!valid) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            frame = Ft12Frame();
            frame.raw.assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(fixedSize));
            frame.control = buffer_[1];
            frame.address = readAddress(buffer_, 2, linkAddressSize_);
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(fixedSize));
            return true;
        }
        if (buffer_.size() < 4) {
            return false;
        }
        const auto length = static_cast<std::size_t>(buffer_[1]);
        if (buffer_[2] != buffer_[1] || buffer_[3] != 0x68 || length < static_cast<std::size_t>(1 + linkAddressSize_)) {
            buffer_.erase(buffer_.begin());
            continue;
        }
        const auto frameSize = length + 6;
        if (buffer_.size() < frameSize) {
            return false;
        }
        const bool valid = buffer_[frameSize - 1] == 0x16 &&
            buffer_[frameSize - 2] == checksum(buffer_, 4, frameSize - 2);
        if (!valid) {
            buffer_.erase(buffer_.begin());
            continue;
        }
        frame = Ft12Frame();
        frame.variable = true;
        frame.raw.assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
        frame.control = buffer_[4];
        frame.address = readAddress(buffer_, 5, linkAddressSize_);
        frame.asdu.assign(
            buffer_.begin() + static_cast<std::ptrdiff_t>(5 + linkAddressSize_),
            buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize - 2));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
        return true;
    }
    return false;
}

std::vector<std::uint8_t> buildFt12FixedFrame(std::uint8_t control, int linkAddress, int linkAddressSize) {
    if (linkAddressSize < 0 || linkAddressSize > 2) {
        throw std::invalid_argument("IEC FT1.2 link address size must be between 0 and 2");
    }
    std::vector<std::uint8_t> frame = {0x10, control};
    appendAddress(frame, linkAddress, linkAddressSize);
    frame.push_back(checksum(frame, 1, frame.size()));
    frame.push_back(0x16);
    return frame;
}

std::vector<std::uint8_t> buildFt12VariableFrame(
    std::uint8_t control,
    int linkAddress,
    int linkAddressSize,
    const std::vector<std::uint8_t>& asdu
) {
    if (linkAddressSize < 0 || linkAddressSize > 2) {
        throw std::invalid_argument("IEC FT1.2 link address size must be between 0 and 2");
    }
    std::vector<std::uint8_t> userData = {control};
    appendAddress(userData, linkAddress, linkAddressSize);
    userData.insert(userData.end(), asdu.begin(), asdu.end());
    if (userData.size() > 255) {
        throw std::invalid_argument("IEC FT1.2 frame too large");
    }
    std::vector<std::uint8_t> frame = {
        0x68,
        static_cast<std::uint8_t>(userData.size()),
        static_cast<std::uint8_t>(userData.size()),
        0x68
    };
    frame.insert(frame.end(), userData.begin(), userData.end());
    frame.push_back(checksum(frame, 4, frame.size()));
    frame.push_back(0x16);
    return frame;
}

Ft12UnbalancedMaster::Ft12UnbalancedMaster(Ft12LinkOptions options)
    : options_(options),
      parser_(options.linkAddressSize) {
    options_.responseTimeoutMs = std::max(1, options_.responseTimeoutMs);
    options_.retransmissions = std::max(0, options_.retransmissions);
}

void Ft12UnbalancedMaster::attach(Ft12Transport* transport) {
    transport_ = transport;
    parser_.clear();
    lastReplyRaw_.clear();
    pendingDuplicates_ = 0;
    accessDemand_ = false;
    dataFlowControl_ = false;
    linkReady_ = false;
}

void Ft12UnbalancedMaster::detach() {
    attach(nullptr);
}

bool Ft12UnbalancedMaster::attached() const {
    return transport_ != nullptr;
}

bool Ft12UnbalancedMaster::linkReady() const {
    return transport_ != nullptr && linkReady_;
}

std::uint64_t Ft12UnbalancedMaster::linkEpoch() const {
    return linkEpoch_;
}

void Ft12UnbalancedMaster::markLinkDown() {
    linkReady_ = false;
}

bool Ft12UnbalancedMaster::accessDemand() const {
    return accessDemand_;
}

bool Ft12UnbalancedMaster::dataFlowControl() const {
    return dataFlowControl_;
}

const Ft12LinkOptions& Ft12UnbalancedMaster::options() const {
    return options_;
}

Ft12Transport& Ft12UnbalancedMaster::transport() {
    if (transport_ == nullptr) {
        throw Ft12TransportError("FT1.2 transport is not connected");
    }
    return *transport_;
}

void Ft12UnbalancedMaster::setExchangeCallbacks(std::function<void()> before,
    std::function<void(const std::vector<std::uint8_t>&)> accepted) {
    beforeExchange_ = std::move(before);
    acceptedReply_ = std::move(accepted);
}

void Ft12UnbalancedMaster::setOperationDeadline(std::int64_t deadlineMs) {
    operationDeadlineMs_ = deadlineMs;
}

void Ft12UnbalancedMaster::resetLink() {
    linkReady_ = false;
    if (options_.requestStatusBeforeReset) {
        exchange(9, false, nullptr, Expect::Status, "request status of link");
    }
    const auto reply = exchange(0, false, nullptr, Expect::Confirm, "reset of remote link");
    if (reply.nack()) {
        throw Ft12LinkError("FT1.2 reset of remote link was not accepted (NACK)");
    }
    // IEC 60870-5-2: after a reset the secondary expects FCB=1 in the next FCV=1 frame.
    frameCountBit_ = false;
    linkReady_ = true;
    ++linkEpoch_;
}

Ft12Reply Ft12UnbalancedMaster::sendConfirm(const std::vector<std::uint8_t>& asdu) {
    if (!linkReady()) {
        throw Ft12LinkError("FT1.2 link is not reset");
    }
    return exchange(3, true, &asdu, Expect::Confirm, "SEND/CONFIRM user data");
}

Ft12Reply Ft12UnbalancedMaster::requestClassData(int dataClass) {
    if (dataClass != 1 && dataClass != 2) {
        throw std::invalid_argument("FT1.2 data class must be 1 or 2");
    }
    if (!linkReady()) {
        throw Ft12LinkError("FT1.2 link is not reset");
    }
    return exchange(dataClass == 1 ? 10 : 11, true, nullptr, Expect::Respond,
        dataClass == 1 ? "request class 1 data" : "request class 2 data");
}

Ft12Reply Ft12UnbalancedMaster::exchange(
    std::uint8_t function,
    bool frameCount,
    const std::vector<std::uint8_t>* asdu,
    Expect expect,
    const char* service
) {
    auto& link = transport();
    if (beforeExchange_) beforeExchange_();
    if (operationDeadlineMs_ > 0 && steadyMs() >= operationDeadlineMs_) {
        throw std::runtime_error("FT1.2 operation deadline exceeded");
    }
    std::uint8_t control = static_cast<std::uint8_t>(kPrm | function);
    if (frameCount) {
        // A new service toggles FCB; its retransmissions below repeat the same frame.
        frameCountBit_ = !frameCountBit_;
        control = static_cast<std::uint8_t>(control | kFcv | (frameCountBit_ ? kFcb : 0));
    }
    const auto request = asdu != nullptr
        ? buildFt12VariableFrame(control, options_.linkAddress, options_.linkAddressSize, *asdu)
        : buildFt12FixedFrame(control, options_.linkAddress, options_.linkAddressSize);
    discardStaleInput();
    const int attempts = 1 + options_.retransmissions;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (operationDeadlineMs_ > 0 && steadyMs() >= operationDeadlineMs_) break;
        link.write(request);
        Ft12Reply reply;
        std::vector<std::uint8_t> raw;
        if (!awaitReply(expect, reply, raw)) {
            continue;
        }
        lastReplyRaw_ = raw;
        // Each earlier copy of this request may still be answered with the same reply.
        pendingDuplicates_ = attempt;
        if (reply.function == 14 || reply.function == 15) {
            linkReady_ = false;
            throw Ft12LinkError(std::string("FT1.2 secondary reports link service ") +
                (reply.function == 14 ? "not functioning" : "not implemented") + " for " + service);
        }
        accessDemand_ = reply.accessDemand;
        dataFlowControl_ = reply.dataFlowControl;
        if (acceptedReply_) acceptedReply_(raw);
        return reply;
    }
    linkReady_ = false;
    lastReplyRaw_.clear();
    pendingDuplicates_ = 0;
    parser_.clear();
    throw Ft12LinkError(std::string("FT1.2 no reply to ") + service + " after " +
        std::to_string(attempts) + " attempts");
}

bool Ft12UnbalancedMaster::awaitReply(Expect expect, Ft12Reply& reply, std::vector<std::uint8_t>& raw) {
    auto& link = transport();
    auto deadline = steadyMs() + options_.responseTimeoutMs;
    if (operationDeadlineMs_ > 0) deadline = std::min(deadline, operationDeadlineMs_);
    while (true) {
        Ft12Frame frame;
        while (parser_.next(frame)) {
            if (!acceptable(frame, expect)) {
                continue;
            }
            reply = Ft12Reply();
            reply.singleCharacter = frame.singleCharacter;
            if (!frame.singleCharacter) {
                reply.function = frame.control & 0x0F;
                reply.accessDemand = (frame.control & kAcd) != 0;
                reply.dataFlowControl = (frame.control & kDfc) != 0;
                reply.asdu = frame.asdu;
            }
            raw = frame.raw;
            return true;
        }
        const auto remaining = deadline - steadyMs();
        if (remaining <= 0) {
            return false;
        }
        parser_.append(link.read(static_cast<int>(remaining)));
    }
}

bool Ft12UnbalancedMaster::acceptable(const Ft12Frame& frame, Expect expect) {
    if (pendingDuplicates_ > 0 && frame.raw == lastReplyRaw_) {
        --pendingDuplicates_;
        return false;
    }
    if (frame.singleCharacter) {
        // E5 replaces ACK (SEND/CONFIRM) and "no data" (REQUEST/RESPOND), never a status.
        return expect != Expect::Status;
    }
    if ((frame.control & kPrm) != 0) {
        return false;
    }
    if (options_.linkAddressSize > 0 && frame.address != options_.linkAddress) {
        return false;
    }
    const int function = frame.control & 0x0F;
    if (function == 14 || function == 15) {
        return !frame.variable;
    }
    switch (expect) {
        case Expect::Confirm:
            return !frame.variable && (function == 0 || function == 1);
        case Expect::Status:
            return !frame.variable && function == 11;
        case Expect::Respond:
            // IEC 60870-5-2 "no data" reply is FC 9 (or E5).
            // COMPATIBILITY BRANCH (FC 0 accepted as "no data"): kept only because the reply code
            // the field-verified AM5 relay sends to an empty class 1/2 request was never recorded,
            // and the previous AM5SE driver accepted any frame there. Rejecting FC 0 could make
            // that relay's link reset every cycle. Remove the `|| function == 0` once its no-data
            // reply code has been confirmed on site (channel trace) to be FC 9 or E5. Stale ACKs
            // of earlier exchanges are already removed by the pre-send flush and duplicate filter.
            return frame.variable ? function == 8 && !frame.asdu.empty() : (function == 9 || function == 0);
    }
    return false;
}

void Ft12UnbalancedMaster::discardStaleInput() {
    auto& link = transport();
    while (true) {
        const auto bytes = link.read(0);
        if (bytes.empty()) {
            break;
        }
        parser_.append(bytes);
    }
    Ft12Frame frame;
    while (parser_.next(frame)) {
        if (pendingDuplicates_ > 0 && frame.raw == lastReplyRaw_) {
            --pendingDuplicates_;
        }
    }
    parser_.clear();
}

}  // namespace edge_gateway
