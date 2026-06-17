#include "edge_gateway/iec_client.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
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
    static std::once_flag wsaOnce;
    static int wsaStartupResult = 0;
    std::call_once(wsaOnce, [] {
        WSADATA wsaData;
        std::memset(&wsaData, 0, sizeof(wsaData));
        wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    });
    if (wsaStartupResult != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

std::string normalizedProtocol(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void appendDecodedAsduValues(
    std::vector<IecDataValue>& out,
    const std::vector<std::uint8_t>& asduBytes,
    const IecProtocolConfig& config
) {
    const auto asdu = IecCodec::decodeAsdu(asduBytes, config);
    out.insert(out.end(), asdu.values.begin(), asdu.values.end());
}

bool controlConfirmationMatches(const PointDefinition& point, const IecDataValue& value, int expectedCause) {
    const int configuredIoa = point.write.iec.ioa >= 0
        ? point.write.iec.ioa
        : point.read.iec.ioa >= 0
            ? point.read.iec.ioa
            : point.write.address >= 0
                ? point.write.address
                : (point.address > 0 ? point.address : -1);
    if (configuredIoa >= 0 && configuredIoa != value.ioa) {
        return false;
    }
    const int configuredCa = point.write.iec.commonAddress > 0
        ? point.write.iec.commonAddress
        : point.read.iec.commonAddress;
    if (configuredCa > 0 && configuredCa != value.commonAddress) {
        return false;
    }
    const int configuredType = point.write.iec.typeId > 0
        ? point.write.iec.typeId
        : point.read.iec.typeId;
    if (configuredType > 0 && configuredType != value.typeId) {
        return false;
    }
    return value.cause == expectedCause;
}

bool isControlType(int typeId) {
    return typeId >= 45 && typeId <= 51;
}

IecDataValue protectionEventToValue(const IecProtectionEvent& event) {
    IecDataValue value;
    value.commonAddress = event.commonAddress;
    value.ioa = event.ioa;
    value.typeId = event.typeId;
    value.cause = event.cause;
    value.value = static_cast<double>(event.eventState);
    value.text = std::to_string(event.eventState);
    value.rawHex = event.rawHex;
    return value;
}

CommandResult failedResult(const std::string& message, std::int64_t ts = 0) {
    CommandResult result;
    result.success = false;
    result.message = message;
    result.ts = ts;
    return result;
}

}  // namespace

CommandResult IecClient::writeByPoint(
    const PointDefinition& point,
    double value,
    const std::string& cmdId,
    const std::string& machineCode,
    const std::string& meterCode,
    std::int64_t nowMsValue
) {
    CommandResult result;
    result.cmdId = cmdId;
    result.machineCode = machineCode;
    result.meterCode = meterCode;
    result.pointCode = point.pointCode;
    result.index = point.index;
    result.requestedValue = value;
    result.ts = nowMsValue;
    result.success = false;
    result.message = "IEC write is not supported for this transport";
    return result;
}

void IecClient::synchronizeClock(std::int64_t) {
    throw std::runtime_error("IEC clock sync is not supported for this transport");
}

IecParameterValue IecClient::readParameter(int, int, std::uint8_t, int) {
    throw std::runtime_error("IEC parameter read is not supported for this transport");
}

CommandResult IecClient::writeParameter(int, int, double, std::uint8_t, int) {
    return failedResult("IEC parameter write is not supported for this transport");
}

CommandResult IecClient::activateParameter(int, std::uint8_t, int) {
    return failedResult("IEC parameter activation is not supported for this transport");
}

std::vector<IecFileSegment> IecClient::callFile(int, int, int, std::uint8_t, int) {
    throw std::runtime_error("IEC file transfer is not supported for this transport");
}

IecTcpClient::IecTcpClient(std::string protocolType, TcpTransportConfig tcp, IecProtocolConfig iec)
    : protocolType_(normalizedProtocol(std::move(protocolType))),
      tcp_(std::move(tcp)),
      iec_(std::move(iec)) {
}

IecTcpClient::~IecTcpClient() {
    stopReceiveLoop();
    disconnect();
}

std::vector<IecDataValue> IecTcpClient::poll() {
    ensureConnected();
    std::vector<IecDataValue> values;
    if (protocolType_ == "iec104") {
        ensureIec104Started();
        if (iec_.pollOnCollect) {
            sendIec104IFrame(IecCodec::buildIec104InterrogationCommand(iec_, nextSendSequence(), currentReceiveSequence()));
        }
        if (!iec_.backgroundReceive) {
            const auto frames = drainIec104Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
            for (const auto& frame : frames) {
                handleIec104Frame(frame);
            }
        } else if (iec_.pollTimeoutMs > 0) {
            std::unique_lock<std::mutex> lock(stateMutex_);
            stateChanged_.wait_for(lock, std::chrono::milliseconds(iec_.pollTimeoutMs), [&] {
                return !bufferedValues_.empty();
            });
        }
        return drainBufferedValues();
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

std::vector<IecDataValue> IecTcpClient::drainBufferedValues() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return drainBufferedValuesLocked();
}

CommandResult IecTcpClient::writeByPoint(
    const PointDefinition& point,
    double value,
    const std::string& cmdId,
    const std::string& machineCode,
    const std::string& meterCode,
    std::int64_t nowMsValue
) {
    CommandResult result;
    result.cmdId = cmdId;
    result.machineCode = machineCode;
    result.meterCode = meterCode;
    result.pointCode = point.pointCode;
    result.index = point.index;
    result.requestedValue = value;
    result.ts = nowMsValue;

    try {
        if (protocolType_ != "iec104") {
            throw std::invalid_argument("IEC write is only supported for iec104");
        }
        if (!point.write.enable) {
            throw std::invalid_argument("point write is disabled");
        }
        if (point.write.minValue && value < *point.write.minValue) {
            throw std::invalid_argument("value below min");
        }
        if (point.write.maxValue && value > *point.write.maxValue) {
            throw std::invalid_argument("value above max");
        }
        if (!point.write.allowedValues.empty()) {
            const auto match = std::find_if(
                point.write.allowedValues.begin(),
                point.write.allowedValues.end(),
                [value](double candidate) {
                    return std::abs(candidate - value) <= 1e-9;
                }
            );
            if (match == point.write.allowedValues.end()) {
                throw std::invalid_argument("value is not in allowedValues");
            }
        }

        ensureConnected();
        ensureIec104Started();

        const auto timeoutMs = point.write.iec.timeoutMs > 0 ? point.write.iec.timeoutMs : iec_.t1Ms;
        if (point.write.iec.selectBeforeExecute) {
            sendIec104IFrame(IecCodec::buildIec104ControlCommand(iec_, point, value, nextSendSequence(), currentReceiveSequence(), true));
            if (!waitForControlConfirmation(point, value, 7, timeoutMs)) {
                throw std::runtime_error("IEC104 select confirmation timeout");
            }
        }

        sendIec104IFrame(IecCodec::buildIec104ControlCommand(iec_, point, value, nextSendSequence(), currentReceiveSequence(), false));
        if (!waitForControlConfirmation(point, value, 7, timeoutMs)) {
            throw std::runtime_error("IEC104 execute confirmation timeout");
        }
        if (point.write.iec.waitActivationTermination &&
            !waitForControlConfirmation(point, value, iec_.activationTerminationCot, timeoutMs)) {
            throw std::runtime_error("IEC104 activation termination timeout");
        }

        result.success = true;
        result.message = "ok";
        return result;
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
        return result;
    }
}

void IecTcpClient::synchronizeClock(std::int64_t nowMsValue) {
    if (protocolType_ != "iec104") {
        throw std::invalid_argument("IEC clock sync is only supported for iec104");
    }
    ensureConnected();
    ensureIec104Started();
    sendIec104IFrame(IecCodec::buildIec104ClockSyncCommand(iec_, nextSendSequence(), currentReceiveSequence(), nowMsValue));
}

IecParameterValue IecTcpClient::readParameter(int ioa, int typeId, std::uint8_t qualifier, int timeoutMs) {
    if (protocolType_ != "iec104") {
        throw std::invalid_argument("IEC parameter read is only supported for iec104");
    }
    ensureConnected();
    ensureIec104Started();
    sendIec104IFrame(IecCodec::buildIec104ParameterCommand(
        iec_, ioa, typeId, 0.0, qualifier, nextSendSequence(), currentReceiveSequence(), 5));
    IecParameterValue value;
    if (!waitForParameterConfirmation(ioa, typeId, 5, timeoutMs, &value) &&
        !waitForParameterConfirmation(ioa, typeId, 7, timeoutMs, &value)) {
        throw std::runtime_error("IEC104 parameter read timeout");
    }
    return value;
}

CommandResult IecTcpClient::writeParameter(int ioa, int typeId, double value, std::uint8_t qualifier, int timeoutMs) {
    CommandResult result;
    result.ts = nowMs();
    result.requestedValue = value;
    try {
        if (protocolType_ != "iec104") {
            throw std::invalid_argument("IEC parameter write is only supported for iec104");
        }
        ensureConnected();
        ensureIec104Started();
        sendIec104IFrame(IecCodec::buildIec104ParameterCommand(
            iec_, ioa, typeId, value, qualifier, nextSendSequence(), currentReceiveSequence(), 6));
        if (!waitForParameterConfirmation(ioa, typeId, 7, timeoutMs)) {
            throw std::runtime_error("IEC104 parameter write confirmation timeout");
        }
        result.success = true;
        result.message = "ok";
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

CommandResult IecTcpClient::activateParameter(int ioa, std::uint8_t qualifier, int timeoutMs) {
    CommandResult result;
    result.ts = nowMs();
    try {
        if (protocolType_ != "iec104") {
            throw std::invalid_argument("IEC parameter activation is only supported for iec104");
        }
        ensureConnected();
        ensureIec104Started();
        sendIec104IFrame(IecCodec::buildIec104ParameterActivationCommand(
            iec_, ioa, qualifier, nextSendSequence(), currentReceiveSequence()));
        if (!waitForParameterConfirmation(ioa, 113, 7, timeoutMs)) {
            throw std::runtime_error("IEC104 parameter activation confirmation timeout");
        }
        result.success = true;
        result.message = "ok";
    } catch (const std::exception& ex) {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

std::vector<IecFileSegment> IecTcpClient::callFile(
    int ioa,
    int nameOfFile,
    int nameOfSection,
    std::uint8_t qualifier,
    int timeoutMs
) {
    if (protocolType_ != "iec104") {
        throw std::invalid_argument("IEC file transfer is only supported for iec104");
    }
    ensureConnected();
    ensureIec104Started();
    sendIec104IFrame(IecCodec::buildIec104FileCallCommand(
        iec_, ioa, nameOfFile, nameOfSection, qualifier, nextSendSequence(), currentReceiveSequence()));
    return waitForFileSegments(nameOfFile, timeoutMs);
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
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        iec104Started_ = false;
        rxBuffer_.clear();
        sendSequence_ = 0;
        receiveSequence_ = 0;
        remoteReceiveSequence_ = 0;
        lastReceiveMs_ = nowMs();
        lastSendMs_ = lastReceiveMs_;
        lastAckMs_ = lastReceiveMs_;
        unacknowledgedReceived_ = 0;
    }
    if (protocolType_ == "iec104" && iec_.backgroundReceive) {
        startReceiveLoop();
    }
}

void IecTcpClient::ensureIec104Started() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (iec104Started_) {
            return;
        }
    }
    sendAll(IecCodec::buildIec104StartDtAct());
    if (iec_.backgroundReceive) {
        if (!waitForIec104Start(iec_.t0Ms)) {
            throw std::runtime_error("IEC104 STARTDT confirmation timeout");
        }
        return;
    }
    const auto frames = drainIec104Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
    for (const auto& frame : frames) {
        handleIec104Frame(frame);
    }
    if (!waitForIec104Start(10)) {
        throw std::runtime_error("IEC104 STARTDT confirmation timeout");
    }
}

void IecTcpClient::startReceiveLoop() {
    if (receiveRunning_.load()) {
        return;
    }
    receiveRunning_.store(true);
    receiveThread_ = std::thread(&IecTcpClient::receiveLoop, this);
}

void IecTcpClient::stopReceiveLoop() {
    receiveRunning_.store(false);
    disconnect();
    stateChanged_.notify_all();
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

void IecTcpClient::receiveLoop() {
    while (receiveRunning_.load()) {
        try {
            if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const auto frames = drainIec104Frames(iec_.idleReadTimeoutMs, std::max(1, iec_.maxPollFrames));
            for (const auto& frame : frames) {
                handleIec104Frame(frame);
            }
            const auto ts = nowMs();
            const auto idleMs = ts - lastReceiveMs_;
            std::uint16_t pendingAckSequence = 0;
            bool shouldSendDelayedAck = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                shouldSendDelayedAck = iec_.sendSFrameAck && unacknowledgedReceived_ > 0 && ts - lastAckMs_ >= iec_.t2Ms;
                if (shouldSendDelayedAck) {
                    pendingAckSequence = receiveSequence_;
                    unacknowledgedReceived_ = 0;
                    lastAckMs_ = ts;
                }
            }
            if (shouldSendDelayedAck) {
                sendAll(IecCodec::buildIec104SFrame(pendingAckSequence));
            }
            if (idleMs >= iec_.t3Ms && ts - lastSendMs_ >= iec_.t2Ms) {
                sendAll(IecCodec::buildIec104TestFrAct());
            }
        } catch (const std::exception& ex) {
            if (receiveRunning_.load()) {
                std::cerr << "IEC104 receive loop error: " << ex.what() << std::endl;
            }
            disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void IecTcpClient::disconnect() {
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        return;
    }
    closeSocket(static_cast<SocketHandle>(socket_));
    socket_ = static_cast<std::intptr_t>(kInvalidSocket);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        rxBuffer_.clear();
        iec104Started_ = false;
        unacknowledgedReceived_ = 0;
    }
    stateChanged_.notify_all();
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

std::uint16_t IecTcpClient::nextSendSequence() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    // IEC 60870-5-104 send/receive sequence numbers are 15-bit and wrap at 32768.
    const auto current = sendSequence_;
    sendSequence_ = static_cast<std::uint16_t>((sendSequence_ + 1) & 0x7FFF);
    return current;
}

std::uint16_t IecTcpClient::currentReceiveSequence() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return receiveSequence_;
}

void IecTcpClient::sendIec104IFrame(const std::vector<std::uint8_t>& bytes) {
    if (!IecCodec::isIec104IFrame(bytes)) {
        sendAll(bytes);
        return;
    }
    const auto frameSequence = IecCodec::iec104SendSequence(bytes);
    {
        std::unique_lock<std::mutex> lock(stateMutex_);
        const auto timeout = std::chrono::milliseconds(std::max(1, iec_.t1Ms));
        bool disconnected = false;
        const auto accepted = stateChanged_.wait_for(lock, timeout, [&] {
            // Abort the wait immediately if the link dropped, so the window
            // wait cannot deadlock when the background receiver has stopped
            // updating remoteReceiveSequence_.
            if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
                disconnected = true;
                return true;
            }
            // Sequence numbers live in a 15-bit space (0..32767); compute the
            // outstanding count modulo 2^15 so the window stays correct across
            // a sequence wrap.
            const auto outstanding = static_cast<std::uint16_t>(
                (sendSequence_ - remoteReceiveSequence_) & 0x7FFF);
            return outstanding <= static_cast<std::uint16_t>(std::max(1, iec_.kWindow));
        });
        if (disconnected) {
            throw std::runtime_error("IEC104 link disconnected while waiting for send window");
        }
        if (!accepted) {
            throw std::runtime_error("IEC104 send window timeout");
        }
    }
    sendAll(bytes);
    (void)frameSequence;
}

void IecTcpClient::sendAll(const std::vector<std::uint8_t>& bytes) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
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
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastSendMs_ = nowMs();
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
    if (rc == 0) {
        disconnect();
        throw std::runtime_error("IEC TCP connection closed");
    }
    if (rc < 0) {
#ifdef _WIN32
        const auto err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            return {};
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return {};
        }
#endif
        disconnect();
        throw std::runtime_error("IEC TCP receive failed");
    }
    if (rc <= 0) {
        return {};
    }
    bytes.resize(static_cast<std::size_t>(rc));
    return bytes;
}

void IecTcpClient::handleIec104Frame(const std::vector<std::uint8_t>& frame) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastReceiveMs_ = nowMs();
    }
    if (IecCodec::isIec104StartDtCon(frame)) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            iec104Started_ = true;
        }
        stateChanged_.notify_all();
        return;
    }
    if (IecCodec::isIec104TestFrAct(frame)) {
        sendAll({0x68, 0x04, 0x83, 0x00, 0x00, 0x00});
        return;
    }
    if (IecCodec::isIec104TestFrCon(frame) || IecCodec::isIec104StopDtCon(frame) || IecCodec::isIec104SFrame(frame)) {
        if (IecCodec::isIec104SFrame(frame)) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            remoteReceiveSequence_ = IecCodec::iec104ReceiveSequence(frame);
        }
        stateChanged_.notify_all();
        return;
    }
    if (!IecCodec::isIec104IFrame(frame)) {
        return;
    }

    std::uint16_t ackSequence = 0;
    bool shouldAck = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        receiveSequence_ = static_cast<std::uint16_t>((IecCodec::iec104SendSequence(frame) + 1) & 0x7FFF);
        remoteReceiveSequence_ = IecCodec::iec104ReceiveSequence(frame);
        ackSequence = receiveSequence_;
        ++unacknowledgedReceived_;
        shouldAck = iec_.sendSFrameAck && unacknowledgedReceived_ >= static_cast<std::uint16_t>(std::max(1, iec_.wAck));
        if (shouldAck) {
            unacknowledgedReceived_ = 0;
            lastAckMs_ = nowMs();
        }
    }
    if (shouldAck) {
        sendAll(IecCodec::buildIec104SFrame(ackSequence));
    }

    const auto asdu = IecCodec::iec104AsduPayload(frame);
    if (asdu.empty()) {
        return;
    }
    try {
        const auto decoded = IecCodec::decodeAsdu(asdu, iec_);
        if (!decoded.values.empty()) {
            if (isControlType(decoded.typeId)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                controlValues_.insert(controlValues_.end(), decoded.values.begin(), decoded.values.end());
                if (controlValues_.size() > 1024) {
                    controlValues_.erase(controlValues_.begin(), controlValues_.begin() + static_cast<std::ptrdiff_t>(controlValues_.size() - 1024));
                }
                stateChanged_.notify_all();
                return;
            }
            bufferValues(decoded.values);
        }
        if (!decoded.protectionEvents.empty()) {
            std::vector<IecDataValue> values;
            values.reserve(decoded.protectionEvents.size());
            for (const auto& event : decoded.protectionEvents) {
                values.push_back(protectionEventToValue(event));
            }
            bufferValues(values);
        }
        if (!decoded.parameters.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            parameterValues_.insert(parameterValues_.end(), decoded.parameters.begin(), decoded.parameters.end());
            if (parameterValues_.size() > 1024) {
                parameterValues_.erase(parameterValues_.begin(), parameterValues_.begin() + static_cast<std::ptrdiff_t>(parameterValues_.size() - 1024));
            }
            stateChanged_.notify_all();
        }
        if (!decoded.fileSegments.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            fileSegments_.insert(fileSegments_.end(), decoded.fileSegments.begin(), decoded.fileSegments.end());
            if (fileSegments_.size() > 1024) {
                fileSegments_.erase(fileSegments_.begin(), fileSegments_.begin() + static_cast<std::ptrdiff_t>(fileSegments_.size() - 1024));
            }
            stateChanged_.notify_all();
        }
    } catch (const std::exception& ex) {
        std::cerr << "IEC104 ASDU decode failed: " << ex.what() << std::endl;
    }
}

void IecTcpClient::bufferValues(const std::vector<IecDataValue>& values) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    bufferedValues_.insert(bufferedValues_.end(), values.begin(), values.end());
    if (bufferedValues_.size() > 4096) {
        bufferedValues_.erase(bufferedValues_.begin(), bufferedValues_.begin() + static_cast<std::ptrdiff_t>(bufferedValues_.size() - 4096));
    }
    stateChanged_.notify_all();
}

std::vector<IecDataValue> IecTcpClient::drainBufferedValuesLocked() {
    std::vector<IecDataValue> values;
    values.swap(bufferedValues_);
    return values;
}

bool IecTcpClient::waitForIec104Start(int timeoutMs) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    return stateChanged_.wait_for(lock, std::chrono::milliseconds(std::max(1, timeoutMs)), [&] {
        return iec104Started_;
    });
}

bool IecTcpClient::waitForControlConfirmation(const PointDefinition& point, double value, int expectedCause, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(stateMutex_);
        stateChanged_.wait_until(lock, deadline, [&] {
            return !controlValues_.empty();
        });
        for (auto it = controlValues_.begin(); it != controlValues_.end(); ++it) {
            if (controlConfirmationMatches(point, *it, expectedCause)) {
                (void)value;
                controlValues_.erase(it);
                return true;
            }
        }
    }
    return false;
}

bool IecTcpClient::waitForParameterConfirmation(
    int ioa,
    int typeId,
    int expectedCause,
    int timeoutMs,
    IecParameterValue* value
) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(stateMutex_);
        stateChanged_.wait_until(lock, deadline, [&] {
            return !parameterValues_.empty();
        });
        for (auto it = parameterValues_.begin(); it != parameterValues_.end(); ++it) {
            if (it->ioa == ioa && it->typeId == typeId && it->cause == expectedCause) {
                if (value) {
                    *value = *it;
                }
                parameterValues_.erase(it);
                return true;
            }
        }
    }
    return false;
}

std::vector<IecFileSegment> IecTcpClient::waitForFileSegments(int nameOfFile, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    std::vector<IecFileSegment> result;
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> lock(stateMutex_);
        stateChanged_.wait_until(lock, deadline, [&] {
            return !fileSegments_.empty();
        });
        for (auto it = fileSegments_.begin(); it != fileSegments_.end();) {
            if (it->nameOfFile == nameOfFile) {
                result.push_back(*it);
                const bool last = it->lastSectionOrSegment != 0 || it->qualifier == 0x80;
                it = fileSegments_.erase(it);
                if (last) {
                    return result;
                }
            } else {
                ++it;
            }
        }
        if (!result.empty()) {
            return result;
        }
    }
    return result;
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
