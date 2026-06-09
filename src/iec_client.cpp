#include "edge_gateway/iec_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
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

void ensureSocketRuntime() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData;
        std::memset(&wsaData, 0, sizeof(wsaData));
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        initialized = true;
    }
#endif
}

std::string normalizedProtocol(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void appendDecodedAsduValues(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& asduBytes,
    const IecProtocolConfig& config
) {
    const auto asdu = IecCodec::decodeAsdu(asduBytes, config);
    out.insert(out.end(), asdu.values.begin(), asdu.values.end());
}

}  // namespace

IecTcpClient::IecTcpClient(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec)
    : protocolType_(normalizedProtocol(std::move(protocolType))),
      tcp_(std::move(tcp)),
      iec_(std::move(iec)) {
}

IecTcpClient::~IecTcpClient() {
    disconnect();
}

std::vector<IecDataValue> IecTcpClient::poll() {
    ensureConnected();
    std::vector<IecDataValue> values;
    if (protocolType_ == "iec104") {
        if (!iec104Started_) {
            sendAll(IecCodec::buildIec104StartDtAct());
            const auto frames = drainIec104Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
            for (const auto& frame : frames) {
                if (IecCodec::isIec104StartDtCon(frame)) {
                    iec104Started_ = true;
                    break;
                }
            }
            if (!iec104Started_) {
                throw std::runtime_error("IEC104 STARTDT confirmation timeout");
            }
        }
        if (iec_.pollOnCollect) {
            sendAll(IecCodec::buildIec104InterrogationCommand(iec_, sendSequence_++, receiveSequence_));
        }
        const auto frames = drainIec104Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
        for (const auto& frame : frames) {
            if (!IecCodec::isIec104IFrame(frame)) {
                continue;
            }
            receiveSequence_ = static_cast<std::uint16_t>(IecCodec::iec104SendSequence(frame) + 1);
            const auto asdu = IecCodec::iec104AsduPayload(frame);
            if (!asdu.empty()) {
                appendDecodedAsduValues(values, asdu, iec_);
            }
        }
        return values;
    }

    if (protocolType_ == "iec103" || protocolType_ == "iec103_tcp") {
        if (iec_.pollOnCollect) {
            sendAll(IecCodec::buildIec103GeneralInterrogationFrame(iec_));
        }
        const auto frames = drainFt12Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
        for (const auto& frame : frames) {
            const auto userData = IecCodec::ft12UserData(frame, iec_.linkAddressSize);
            const auto decoded = IecCodec::decodeIec103Data(userData, iec_);
            values.insert(values.end(), decoded.begin(), decoded.end());
        }
        return values;
    }

    throw std::invalid_argument("IecTcpClient does not support protocol.type=" + protocolType_);
}

void IecTcpClient::ensureConnected() {
    if (socket_ != static_cast<std::intptr_t>(kInvalidSocket)) {
        return;
    }
    ensureSocketRuntime();

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const auto port = std::to_string(tcp_.port);
    if (getaddrinfo(tcp_.host.c_str(), port.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("IEC TCP getaddrinfo failed");
    }

    SocketHandle connected = kInvalidSocket;
    for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
        connected = static_cast<SocketHandle>(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
        if (connected == kInvalidSocket) {
            continue;
        }
        if (connect(connected, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(connected);
        connected = kInvalidSocket;
    }
    freeaddrinfo(result);
    if (connected == kInvalidSocket) {
        throw std::runtime_error("IEC TCP connect failed: " + tcp_.host + ":" + std::to_string(tcp_.port));
    }

    socket_ = static_cast<std::intptr_t>(connected);
    configureSocketTimeouts();
    iec104Started_ = false;
}

void IecTcpClient::disconnect() {
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        return;
    }
    closeSocket(static_cast<SocketHandle>(socket_));
    socket_ = static_cast<std::intptr_t>(kInvalidSocket);
    rxBuffer_.clear();
    iec104Started_ = false;
}

void IecTcpClient::configureSocketTimeouts() const {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(tcp_.timeoutMs);
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout;
    timeout.tv_sec = tcp_.timeoutMs / 1000;
    timeout.tv_usec = (tcp_.timeoutMs % 1000) * 1000;
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

void IecTcpClient::sendAll(const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(static_cast<SocketHandle>(socket_), reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(static_cast<SocketHandle>(socket_), bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (rc <= 0) {
            disconnect();
            throw std::runtime_error("IEC TCP send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

std::vector<std::uint8_t> IecTcpClient::readSome(int timeoutMs) {
    (void)timeoutMs;
    std::vector<std::uint8_t> bytes(512);
#ifdef _WIN32
    const auto rc = recv(static_cast<SocketHandle>(socket_), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
#else
    const auto rc = recv(static_cast<SocketHandle>(socket_), bytes.data(), bytes.size(), 0);
#endif
    if (rc <= 0) {
        return {};
    }
    bytes.resize(static_cast<std::size_t>(rc));
    return bytes;
}

std::vector<std::vector<std::uint8_t>> IecTcpClient::drainIec104Frames(int timeoutMs, int maxFrames) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        auto chunk = readSome(timeoutMs);
        if (chunk.empty() && rxBuffer_.empty()) {
            break;
        }
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
        while (rxBuffer_.size() >= 2) {
            const auto start = std::find(rxBuffer_.begin(), rxBuffer_.end(), static_cast<std::uint8_t>(0x68));
            if (start != rxBuffer_.begin()) {
                rxBuffer_.erase(rxBuffer_.begin(), start);
            }
            if (rxBuffer_.size() < 2) {
                break;
            }
            const auto frameSize = static_cast<std::size_t>(rxBuffer_[1]) + 2;
            if (frameSize < 6) {
                rxBuffer_.erase(rxBuffer_.begin());
                continue;
            }
            if (rxBuffer_.size() < frameSize) {
                break;
            }
            std::vector<std::uint8_t> frame(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            if (IecCodec::isIec104Frame(frame)) {
                frames.push_back(std::move(frame));
                if (static_cast<int>(frames.size()) >= maxFrames) {
                    return frames;
                }
            }
        }
        if (!frames.empty() && chunk.empty()) {
            break;
        }
    }
    return frames;
}

std::vector<std::vector<std::uint8_t>> IecTcpClient::drainFt12Frames(int timeoutMs, int maxFrames) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        auto chunk = readSome(timeoutMs);
        if (chunk.empty() && rxBuffer_.empty()) {
            break;
        }
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
        while (rxBuffer_.size() >= 6) {
            const auto start = std::find(rxBuffer_.begin(), rxBuffer_.end(), static_cast<std::uint8_t>(0x68));
            if (start != rxBuffer_.begin()) {
                rxBuffer_.erase(rxBuffer_.begin(), start);
            }
            if (rxBuffer_.size() < 6) {
                break;
            }
            const auto frameSize = static_cast<std::size_t>(rxBuffer_[1]) + 6;
            if (rxBuffer_[2] != rxBuffer_[1] || rxBuffer_[3] != 0x68 || frameSize < 6) {
                rxBuffer_.erase(rxBuffer_.begin());
                continue;
            }
            if (rxBuffer_.size() < frameSize) {
                break;
            }
            std::vector<std::uint8_t> frame(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            if (IecCodec::isFt12VariableFrame(frame)) {
                frames.push_back(std::move(frame));
                if (static_cast<int>(frames.size()) >= maxFrames) {
                    return frames;
                }
            }
        }
        if (!frames.empty() && chunk.empty()) {
            break;
        }
    }
    return frames;
}

IecSerialClient::IecSerialClient(
    std::string protocolType,
    std::shared_ptr<ISerialPort> serialPort,
    SerialPortOptions serial,
    IecProtocolConfig iec
) : protocolType_(normalizedProtocol(std::move(protocolType))),
    serialPort_(std::move(serialPort)),
    serial_(std::move(serial)),
    iec_(std::move(iec)) {
    if (!serialPort_) {
        throw std::invalid_argument("IEC serial port is required");
    }
}

std::vector<IecDataValue> IecSerialClient::poll() {
    ensurePortOpen();
    std::vector<IecDataValue> values;
    if (protocolType_ == "iec101") {
        if (iec_.pollOnCollect) {
            sendAll(IecCodec::buildIec101InterrogationFrame(iec_));
        }
        const auto frames = drainFt12Frames(serial_.timeoutMs, iec_.maxPollFrames);
        for (const auto& frame : frames) {
            const auto userData = IecCodec::ft12UserData(frame, iec_.linkAddressSize);
            const auto headerSize = static_cast<std::size_t>(1 + std::max(0, iec_.linkAddressSize));
            if (userData.size() <= headerSize) {
                continue;
            }
            const std::vector<std::uint8_t> asdu(userData.begin() + static_cast<std::ptrdiff_t>(headerSize), userData.end());
            appendDecodedAsduValues(values, asdu, iec_);
        }
        return values;
    }

    if (protocolType_ == "iec103" || protocolType_ == "iec103_serial") {
        if (iec_.pollOnCollect) {
            sendAll(IecCodec::buildIec103GeneralInterrogationFrame(iec_));
        }
        const auto frames = drainFt12Frames(serial_.timeoutMs, iec_.maxPollFrames);
        for (const auto& frame : frames) {
            const auto userData = IecCodec::ft12UserData(frame, iec_.linkAddressSize);
            const auto decoded = IecCodec::decodeIec103Data(userData, iec_);
            values.insert(values.end(), decoded.begin(), decoded.end());
        }
        return values;
    }

    throw std::invalid_argument("IecSerialClient does not support protocol.type=" + protocolType_);
}

void IecSerialClient::ensurePortOpen() {
    if (!serialPort_->isOpen()) {
        serialPort_->open();
    }
}

void IecSerialClient::sendAll(const std::vector<std::uint8_t>& bytes) {
    serialPort_->write(bytes);
}

std::vector<std::vector<std::uint8_t>> IecSerialClient::drainFt12Frames(int timeoutMs, int maxFrames) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        auto chunk = serialPort_->read(512, timeoutMs);
        if (chunk.empty() && rxBuffer_.empty()) {
            break;
        }
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
        while (rxBuffer_.size() >= 6) {
            const auto start = std::find(rxBuffer_.begin(), rxBuffer_.end(), static_cast<std::uint8_t>(0x68));
            if (start != rxBuffer_.begin()) {
                rxBuffer_.erase(rxBuffer_.begin(), start);
            }
            if (rxBuffer_.size() < 6) {
                break;
            }
            const auto frameSize = static_cast<std::size_t>(rxBuffer_[1]) + 6;
            if (rxBuffer_[2] != rxBuffer_[1] || rxBuffer_[3] != 0x68 || frameSize < 6) {
                rxBuffer_.erase(rxBuffer_.begin());
                continue;
            }
            if (rxBuffer_.size() < frameSize) {
                break;
            }
            std::vector<std::uint8_t> frame(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            if (IecCodec::isFt12VariableFrame(frame)) {
                frames.push_back(std::move(frame));
                if (static_cast<int>(frames.size()) >= maxFrames) {
                    return frames;
                }
            }
        }
        if (!frames.empty() && chunk.empty()) {
            break;
        }
    }
    return frames;
}

}  // namespace edge_gateway
