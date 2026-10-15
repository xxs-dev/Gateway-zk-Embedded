#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/channel_trace.hpp"

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
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
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

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isConnectInProgress(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN;
#endif
}

bool isInterrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool setSocketBlocking(SocketHandle socketHandle, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0UL : 1UL;
    return ioctlsocket(socketHandle, FIONBIO, &mode) == 0;
#else
    const auto flags = fcntl(socketHandle, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(
        socketHandle,
        F_SETFL,
        blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)
    ) == 0;
#endif
}

bool waitForConnect(SocketHandle socketHandle, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remainingUs / 1000000);
        timeout.tv_usec = static_cast<long>(remainingUs % 1000000);
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socketHandle, &writeSet);
        fd_set errorSet;
        FD_ZERO(&errorSet);
        FD_SET(socketHandle, &errorSet);
#ifdef _WIN32
        const auto rc = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
        const auto rc = select(socketHandle + 1, nullptr, &writeSet, &errorSet, &timeout);
#endif
        if (rc > 0) {
            return true;
        }
        if (rc == 0) {
            return false;
        }
        if (!isInterrupted(lastSocketError())) {
            return false;
        }
    }
}

SocketHandle duplicateSocket(SocketHandle socketHandle) {
#ifdef _WIN32
    WSAPROTOCOL_INFO protocolInfo{};
    if (WSADuplicateSocket(socketHandle, GetCurrentProcessId(), &protocolInfo) != 0) {
        return kInvalidSocket;
    }
    return WSASocket(
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        &protocolInfo,
        0,
        0
    );
#else
    return dup(socketHandle);
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

bool controlAddressMatches(const PointDefinition& point, const IecDataValue& value) {
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
    return true;
}

bool controlEchoMatches(const PointDefinition& point, double requestedValue, const IecDataValue& value, int expectedSelect) {
    if (expectedSelect >= 0 && value.commandQualifier >= 0) {
        const int select = (value.commandQualifier & 0x80) != 0 ? 1 : 0;
        if (select != expectedSelect) {
            return false;
        }
    }
    double expected = 0.0;
    try {
        expected = IecCodec::expectedControlEchoValue(point, requestedValue);
    } catch (const std::exception&) {
        return true;
    }
    const double tolerance = 1e-6 * std::max(1.0, std::abs(expected));
    return std::abs(value.value - expected) <= tolerance;
}

void patchIec104Sequences(std::vector<std::uint8_t>& frame, std::uint16_t sendSequence, std::uint16_t receiveSequence) {
    const auto send = static_cast<std::uint16_t>(sendSequence << 1);
    const auto recv = static_cast<std::uint16_t>(receiveSequence << 1);
    frame[2] = static_cast<std::uint8_t>(send & 0xFF);
    frame[3] = static_cast<std::uint8_t>((send >> 8) & 0xFF);
    frame[4] = static_cast<std::uint8_t>(recv & 0xFF);
    frame[5] = static_cast<std::uint8_t>((recv >> 8) & 0xFF);
}

bool isControlType(int typeId) {
    return typeId >= 45 && typeId <= 51;
}

// Extracts FT1.2 frames from buffer. Variable frames (68 L L 68 ... 16) are
// returned; a single-character ACK (E5) or a valid fixed frame (10 C A CS 16)
// is a legitimate link-layer answer (e.g. to SEND/CONFIRM in unbalanced
// mode) and only sets sawLinkResponse. Returns true once maxFrames is reached.
bool extractFt12Frames(
    std::vector<std::uint8_t>& buffer,
    int linkAddressSize,
    std::vector<std::vector<std::uint8_t>>& frames,
    int maxFrames,
    bool& sawLinkResponse
) {
    const auto fixedSize = static_cast<std::size_t>(4 + std::max(0, linkAddressSize));
    while (!buffer.empty()) {
        const auto start = std::find_if(buffer.begin(), buffer.end(), [](std::uint8_t byte) {
            return byte == 0x68 || byte == 0x10 || byte == 0xE5;
        });
        if (start != buffer.begin()) {
            buffer.erase(buffer.begin(), start);
        }
        if (buffer.empty()) {
            break;
        }
        if (buffer.front() == 0xE5) {
            sawLinkResponse = true;
            buffer.erase(buffer.begin());
            continue;
        }
        if (buffer.front() == 0x10) {
            if (buffer.size() < fixedSize) {
                break;
            }
            std::vector<std::uint8_t> fixed(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(fixedSize));
            if (IecCodec::isFt12FixedFrame(fixed, linkAddressSize)) {
                sawLinkResponse = true;
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(fixedSize));
            } else {
                buffer.erase(buffer.begin());
            }
            continue;
        }
        if (buffer.size() < 6) {
            break;
        }
        const auto frameSize = static_cast<std::size_t>(buffer[1]) + 6;
        if (buffer[2] != buffer[1] || buffer[3] != 0x68) {
            buffer.erase(buffer.begin());
            continue;
        }
        if (buffer.size() < frameSize) {
            break;
        }
        std::vector<std::uint8_t> frame(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameSize));
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameSize));
        if (IecCodec::isFt12VariableFrame(frame)) {
            sawLinkResponse = true;
            frames.push_back(std::move(frame));
            if (static_cast<int>(frames.size()) >= maxFrames) {
                return true;
            }
        }
    }
    return false;
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

class SocketGuard {
public:
    explicit SocketGuard(SocketHandle socketHandle) : socketHandle_(socketHandle) {}
    ~SocketGuard() { closeSocket(socketHandle_); }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

private:
    SocketHandle socketHandle_;
};

std::vector<Iec103DisturbanceRecord> IecClient::listDisturbanceRecords(int) {
    throw std::runtime_error("IEC103 disturbance directory is not supported for this transport");
}

Iec103ComtradeFiles IecClient::pullComtradeRecording(int, const std::string&, int) {
    throw std::runtime_error("IEC103 COMTRADE transfer is not supported for this transport");
}

void IecClient::setRecordingProgressCallback(
    std::function<void(const std::string&)> callback
) {
    (void)callback;
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
        if (iec_.pollOnCollect && interrogationDue(nowMs())) {
            sendIec104IFrame(IecCodec::buildIec104InterrogationCommand(iec_, 0, 0));
        }
        if (!iec_.backgroundReceive) {
            const auto frames = drainIec104Frames(iec_.pollTimeoutMs, iec_.maxPollFrames);
            for (const auto& frame : frames) {
                handleIec104Frame(frame);
            }
            if (t1SupervisionExpired(nowMs())) {
                disconnect();
                throw std::runtime_error("IEC104 t1 timeout: no acknowledgement from controlled station");
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
        bool sawLinkResponse = false;
        const auto frames = drainFt12Frames(iec_.pollTimeoutMs, iec_.maxPollFrames, &sawLinkResponse);
        if (iec_.pollOnCollect && !sawLinkResponse) {
            // An interrogated station that stays silent is offline; do not
            // report an empty answer as a successful poll.
            throw std::runtime_error("IEC103 no response to general interrogation");
        }
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
        // Late confirmations of an earlier (timed-out) command must never
        // confirm this one.
        clearControlConfirmations(point);
        if (point.write.iec.selectBeforeExecute) {
            sendIec104IFrame(IecCodec::buildIec104ControlCommand(iec_, point, value, 0, 0, true));
            if (!waitForControlConfirmation(point, value, 7, timeoutMs, 1)) {
                throw std::runtime_error("IEC104 select confirmation timeout");
            }
            clearControlConfirmations(point);
        }

        sendIec104IFrame(IecCodec::buildIec104ControlCommand(iec_, point, value, 0, 0, false));
        if (!waitForControlConfirmation(point, value, 7, timeoutMs, 0)) {
            throw std::runtime_error("IEC104 execute confirmation timeout");
        }
        if (point.write.iec.waitActivationTermination &&
            !waitForControlConfirmation(point, value, iec_.activationTerminationCot, timeoutMs, 0)) {
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
    sendIec104IFrame(IecCodec::buildIec104ClockSyncCommand(iec_, 0, 0, nowMsValue));
}

IecParameterValue IecTcpClient::readParameter(int ioa, int typeId, std::uint8_t qualifier, int timeoutMs) {
    if (protocolType_ != "iec104") {
        throw std::invalid_argument("IEC parameter read is only supported for iec104");
    }
    ensureConnected();
    ensureIec104Started();
    clearParameterConfirmations(ioa, typeId);
    sendIec104IFrame(IecCodec::buildIec104ParameterCommand(
        iec_, ioa, typeId, 0.0, qualifier, 0, 0, 5));
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
        clearParameterConfirmations(ioa, typeId);
        sendIec104IFrame(IecCodec::buildIec104ParameterCommand(
            iec_, ioa, typeId, value, qualifier, 0, 0, 6));
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
        clearParameterConfirmations(ioa, 113);
        sendIec104IFrame(IecCodec::buildIec104ParameterActivationCommand(
            iec_, ioa, qualifier, 0, 0));
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
        iec_, ioa, nameOfFile, nameOfSection, qualifier, 0, 0));
    return waitForFileSegments(nameOfFile, timeoutMs);
}

void IecTcpClient::ensureConnected() {
    if (connected_.load()) {
        return;
    }
    ensureSocketRuntime();

    std::lock_guard<std::mutex> socketLock(socketMutex_);
    if (socket_ != static_cast<std::intptr_t>(kInvalidSocket)) {
        connected_.store(true);
        return;
    }

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
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(1, tcp_.connectTimeoutMs));
    for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
        connected = static_cast<SocketHandle>(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
        if (connected == kInvalidSocket) {
            continue;
        }
        if (!setSocketBlocking(connected, false)) {
            closeSocket(connected);
            connected = kInvalidSocket;
            continue;
        }
        const auto connectRc = connect(connected, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
        if (connectRc != 0) {
            const auto error = lastSocketError();
            if (!isConnectInProgress(error) || !waitForConnect(connected, deadline)) {
                closeSocket(connected);
                connected = kInvalidSocket;
                continue;
            }
            int socketError = 0;
#ifdef _WIN32
            int optionLength = sizeof(socketError);
            const auto optionRc = getsockopt(
                connected,
                SOL_SOCKET,
                SO_ERROR,
                reinterpret_cast<char*>(&socketError),
                &optionLength
            );
#else
            socklen_t optionLength = sizeof(socketError);
            const auto optionRc = getsockopt(connected, SOL_SOCKET, SO_ERROR, &socketError, &optionLength);
#endif
            if (optionRc != 0 || socketError != 0) {
                closeSocket(connected);
                connected = kInvalidSocket;
                continue;
            }
        }
        if (!setSocketBlocking(connected, true)) {
            closeSocket(connected);
            connected = kInvalidSocket;
            continue;
        }
        break;
    }
    freeaddrinfo(result);
    if (connected == kInvalidSocket) {
        throw std::runtime_error("IEC TCP connect failed: " + tcp_.host + ":" + std::to_string(tcp_.port));
    }

    try {
        configureSocketTimeouts(static_cast<std::intptr_t>(connected));
    } catch (...) {
        closeSocket(connected);
        throw;
    }
    socket_ = static_cast<std::intptr_t>(connected);
    ++socketGeneration_;
    if (socketGeneration_ == 0) {
        ++socketGeneration_;
    }
    connected_.store(true);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        iec104Started_ = false;
        rxResetRequested_.store(true);
        sendSequence_ = 0;
        receiveSequence_ = 0;
        remoteReceiveSequence_ = 0;
        lastReceiveMs_ = nowMs();
        lastSendMs_ = lastReceiveMs_;
        lastAckMs_ = lastReceiveMs_;
        unacknowledgedReceived_ = 0;
        unackedIFrames_.clear();
        testFrPendingSinceMs_ = 0;
        lastInterrogationMs_ = 0;
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
    bool expected = false;
    if (!receiveRunning_.compare_exchange_strong(expected, true)) {
        return;
    }
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
            if (!connected_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const auto frames = drainIec104Frames(iec_.idleReadTimeoutMs, std::max(1, iec_.maxPollFrames));
            for (const auto& frame : frames) {
                handleIec104Frame(frame);
            }
            const auto ts = nowMs();
            std::uint16_t pendingAckSequence = 0;
            bool shouldSendDelayedAck = false;
            bool shouldSendTest = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                shouldSendDelayedAck = iec_.sendSFrameAck && unacknowledgedReceived_ > 0 && ts - lastAckMs_ >= iec_.t2Ms;
                if (shouldSendDelayedAck) {
                    pendingAckSequence = receiveSequence_;
                    unacknowledgedReceived_ = 0;
                    lastAckMs_ = ts;
                }
                shouldSendTest = ts - lastReceiveMs_ >= iec_.t3Ms && ts - lastSendMs_ >= iec_.t2Ms;
            }
            if (t1SupervisionExpired(ts)) {
                std::cerr << "IEC104 t1 timeout: closing connection to "
                          << tcp_.host << ":" << tcp_.port << std::endl;
                disconnect();
                continue;
            }
            if (shouldSendDelayedAck) {
                sendAll(IecCodec::buildIec104SFrame(pendingAckSequence));
            }
            if (shouldSendTest) {
                sendAll(IecCodec::buildIec104TestFrAct());
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (testFrPendingSinceMs_ == 0) {
                    testFrPendingSinceMs_ = nowMs();
                }
            }
        } catch (const std::exception& ex) {
            if (receiveRunning_.load()) {
                std::cerr << "IEC104 receive loop error: " << ex.what() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void IecTcpClient::disconnect(std::uint64_t expectedGeneration) {
    SocketHandle socketHandle = kInvalidSocket;
    {
        std::lock_guard<std::mutex> socketLock(socketMutex_);
        if (expectedGeneration != 0 && expectedGeneration != socketGeneration_) {
            return;
        }
        if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
            connected_.store(false);
            return;
        }
        socketHandle = static_cast<SocketHandle>(socket_);
        socket_ = static_cast<std::intptr_t>(kInvalidSocket);
        connected_.store(false);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            rxResetRequested_.store(true);
            iec104Started_ = false;
            unacknowledgedReceived_ = 0;
            unackedIFrames_.clear();
            testFrPendingSinceMs_ = 0;
        }
    }
#ifdef _WIN32
    shutdown(socketHandle, SD_BOTH);
#else
    shutdown(socketHandle, SHUT_RDWR);
#endif
    closeSocket(socketHandle);
    stateChanged_.notify_all();
}

std::intptr_t IecTcpClient::duplicateConnectedSocket(std::uint64_t& generation) const {
    std::lock_guard<std::mutex> socketLock(socketMutex_);
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        generation = 0;
        return static_cast<std::intptr_t>(kInvalidSocket);
    }
    generation = socketGeneration_;
    return static_cast<std::intptr_t>(duplicateSocket(static_cast<SocketHandle>(socket_)));
}

void IecTcpClient::configureSocketTimeouts(std::intptr_t socketValue) const {
    const auto socketHandle = static_cast<SocketHandle>(socketValue);
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(tcp_.timeoutMs);
    if (setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        throw std::runtime_error("IEC TCP configure socket timeout failed");
    }
#else
    timeval timeout;
    timeout.tv_sec = tcp_.timeoutMs / 1000;
    timeout.tv_usec = (tcp_.timeoutMs % 1000) * 1000;
    if (setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("IEC TCP configure socket timeout failed");
    }
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
    // Serialise I-frame senders (collector GI vs. command thread) so that
    // N(S) is allocated and put on the wire in the same order. The receive
    // thread never takes iFrameMutex_, so it can keep processing acks while
    // we wait for the send window.
    std::lock_guard<std::mutex> iFrameLock(iFrameMutex_);
    auto frame = bytes;
    {
        std::unique_lock<std::mutex> lock(stateMutex_);
        const auto timeout = std::chrono::milliseconds(std::max(1, iec_.t1Ms));
        bool disconnected = false;
        const auto accepted = stateChanged_.wait_for(lock, timeout, [&] {
            // Abort the wait immediately if the link dropped, so the window
            // wait cannot deadlock when the background receiver has stopped
            // updating remoteReceiveSequence_.
            if (!connected_.load()) {
                disconnected = true;
                return true;
            }
            // Sequence numbers live in a 15-bit space (0..32767); compute the
            // outstanding count modulo 2^15 so the window stays correct across
            // a sequence wrap.
            const auto outstanding = static_cast<std::uint16_t>(
                (sendSequence_ - remoteReceiveSequence_) & 0x7FFF);
            return outstanding < static_cast<std::uint16_t>(std::max(1, iec_.kWindow));
        });
        if (disconnected) {
            throw std::runtime_error("IEC104 link disconnected while waiting for send window");
        }
        if (!accepted) {
            lock.unlock();
            // t1 expired without acknowledgement: the link is dead (IEC 60870-5-104 5.2).
            disconnect();
            throw std::runtime_error("IEC104 send window timeout");
        }
        const auto sequence = sendSequence_;
        sendSequence_ = static_cast<std::uint16_t>((sendSequence_ + 1) & 0x7FFF);
        patchIec104Sequences(frame, sequence, receiveSequence_);
        unackedIFrames_.emplace_back(sequence, nowMs());
        // The I-frame carries N(R), which acknowledges everything received so far.
        unacknowledgedReceived_ = 0;
        lastAckMs_ = nowMs();
    }
    sendAll(frame);
}

void IecTcpClient::sendAll(const std::vector<std::uint8_t>& bytes) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    std::uint64_t socketGeneration = 0;
    const auto socketHandle = static_cast<SocketHandle>(duplicateConnectedSocket(socketGeneration));
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("IEC TCP socket is not connected");
    }
    SocketGuard socketGuard(socketHandle);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(socketHandle, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(socketHandle, bytes.data() + sent, bytes.size() - sent, kSendFlags);
#endif
        if (rc <= 0) {
            disconnect(socketGeneration);
            throw std::runtime_error("IEC TCP send failed");
        }
        traceBytes(true, bytes.data() + sent, static_cast<std::size_t>(rc));
        sent += static_cast<std::size_t>(rc);
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastSendMs_ = nowMs();
    }
}

std::vector<std::uint8_t> IecTcpClient::readSome(int timeoutMs) {
    std::uint64_t socketGeneration = 0;
    const auto socketHandle = static_cast<SocketHandle>(duplicateConnectedSocket(socketGeneration));
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("IEC TCP socket is not connected");
    }
    SocketGuard socketGuard(socketHandle);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);
    const auto boundedTimeoutMs = std::max(0, timeoutMs);
    timeval timeout{};
    timeout.tv_sec = boundedTimeoutMs / 1000;
    timeout.tv_usec = (boundedTimeoutMs % 1000) * 1000;
#ifdef _WIN32
    const auto ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const auto ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    if (ready == 0) {
        return {};
    }
    if (ready < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEINTR) {
            return {};
        }
#else
        if (errno == EINTR) {
            return {};
        }
#endif
        disconnect(socketGeneration);
        throw std::runtime_error("IEC TCP receive wait failed");
    }

    std::vector<std::uint8_t> bytes(512);
#ifdef _WIN32
        const auto rc = recv(socketHandle, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
#else
        const auto rc = recv(socketHandle, bytes.data(), bytes.size(), 0);
#endif
    if (rc == 0) {
        disconnect(socketGeneration);
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
        disconnect(socketGeneration);
        throw std::runtime_error("IEC TCP receive failed");
    }
    if (rc <= 0) {
        return {};
    }
    bytes.resize(static_cast<std::size_t>(rc));
    traceBytes(false, bytes.data(), bytes.size());
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
    const auto rejectAcknowledge = [this](std::uint16_t receiveSequence) {
        std::uint16_t sendSequence = 0;
        std::uint16_t acknowledged = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            sendSequence = sendSequence_;
            acknowledged = remoteReceiveSequence_;
        }
        disconnect();
        throw std::runtime_error(
            "IEC104 protocol error: received N(R)=" + std::to_string(receiveSequence) +
            " outside [" + std::to_string(acknowledged) + ", V(S)=" + std::to_string(sendSequence) +
            "]; connection closed");
    };
    if (IecCodec::isIec104TestFrCon(frame) || IecCodec::isIec104StopDtCon(frame) || IecCodec::isIec104SFrame(frame)) {
        bool acknowledgeValid = true;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (IecCodec::isIec104TestFrCon(frame)) {
                testFrPendingSinceMs_ = 0;
            }
            if (IecCodec::isIec104SFrame(frame)) {
                acknowledgeValid = noteRemoteAcknowledgeLocked(IecCodec::iec104ReceiveSequence(frame));
            }
        }
        if (!acknowledgeValid) {
            rejectAcknowledge(IecCodec::iec104ReceiveSequence(frame));
        }
        stateChanged_.notify_all();
        return;
    }
    if (!IecCodec::isIec104IFrame(frame)) {
        return;
    }

    std::uint16_t ackSequence = 0;
    bool shouldAck = false;
    bool acknowledgeValid = true;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        acknowledgeValid = noteRemoteAcknowledgeLocked(IecCodec::iec104ReceiveSequence(frame));
    }
    if (!acknowledgeValid) {
        rejectAcknowledge(IecCodec::iec104ReceiveSequence(frame));
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        receiveSequence_ = static_cast<std::uint16_t>((IecCodec::iec104SendSequence(frame) + 1) & 0x7FFF);
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

bool IecTcpClient::waitForControlConfirmation(
    const PointDefinition& point,
    double value,
    int expectedCause,
    int timeoutMs,
    int expectedSelect
) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    std::unique_lock<std::mutex> lock(stateMutex_);
    while (true) {
        for (auto it = controlValues_.begin(); it != controlValues_.end(); ++it) {
            if (!controlAddressMatches(point, *it) || it->cause != expectedCause) {
                continue;
            }
            if (it->negative) {
                controlValues_.erase(it);
                throw std::runtime_error(
                    "IEC104 negative confirmation from controlled station (COT=" +
                    std::to_string(expectedCause) + ", P/N=1)");
            }
            if (!controlEchoMatches(point, value, *it, expectedSelect)) {
                continue;
            }
            controlValues_.erase(it);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        stateChanged_.wait_until(lock, deadline);
    }
}

void IecTcpClient::clearControlConfirmations(const PointDefinition& point) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    controlValues_.erase(
        std::remove_if(controlValues_.begin(), controlValues_.end(), [&](const IecDataValue& candidate) {
            return controlAddressMatches(point, candidate);
        }),
        controlValues_.end());
}

bool IecTcpClient::waitForParameterConfirmation(
    int ioa,
    int typeId,
    int expectedCause,
    int timeoutMs,
    IecParameterValue* value
) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    std::unique_lock<std::mutex> lock(stateMutex_);
    while (true) {
        for (auto it = parameterValues_.begin(); it != parameterValues_.end(); ++it) {
            if (it->ioa == ioa && it->typeId == typeId && it->cause == expectedCause) {
                if (it->negative) {
                    parameterValues_.erase(it);
                    throw std::runtime_error(
                        "IEC104 negative parameter confirmation (COT=" + std::to_string(expectedCause) + ", P/N=1)");
                }
                if (value) {
                    *value = *it;
                }
                parameterValues_.erase(it);
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        stateChanged_.wait_until(lock, deadline);
    }
}

void IecTcpClient::clearParameterConfirmations(int ioa, int typeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    parameterValues_.erase(
        std::remove_if(parameterValues_.begin(), parameterValues_.end(), [&](const IecParameterValue& candidate) {
            return candidate.ioa == ioa && candidate.typeId == typeId;
        }),
        parameterValues_.end());
}

bool IecTcpClient::noteRemoteAcknowledgeLocked(std::uint16_t receiveSequence) {
    // Valid N(R): last acknowledged <= N(R) <= V(S) (modulo 2^15).
    const auto advance = static_cast<std::uint16_t>((receiveSequence - remoteReceiveSequence_) & 0x7FFF);
    const auto outstanding = static_cast<std::uint16_t>((sendSequence_ - remoteReceiveSequence_) & 0x7FFF);
    if (advance > outstanding) {
        return false;
    }
    remoteReceiveSequence_ = receiveSequence;
    // Frames with N(S) in [N(R), sendSequence_) are still unacknowledged.
    const auto window = static_cast<std::uint16_t>((sendSequence_ - receiveSequence) & 0x7FFF);
    while (!unackedIFrames_.empty()) {
        const auto distance = static_cast<std::uint16_t>((unackedIFrames_.front().first - receiveSequence) & 0x7FFF);
        if (distance < window) {
            break;
        }
        unackedIFrames_.pop_front();
    }
    return true;
}

bool IecTcpClient::t1SupervisionExpired(std::int64_t nowMsValue) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto t1 = static_cast<std::int64_t>(std::max(1, iec_.t1Ms));
    if (testFrPendingSinceMs_ > 0 && nowMsValue - testFrPendingSinceMs_ >= t1) {
        return true;
    }
    return !unackedIFrames_.empty() && nowMsValue - unackedIFrames_.front().second >= t1;
}

bool IecTcpClient::interrogationDue(std::int64_t nowMsValue) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (iec_.interrogationIntervalMs > 0 && lastInterrogationMs_ > 0 &&
        nowMsValue - lastInterrogationMs_ < iec_.interrogationIntervalMs) {
        return false;
    }
    lastInterrogationMs_ = nowMsValue;
    return true;
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
    std::lock_guard<std::mutex> rxLock(rxMutex_);
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        if (rxResetRequested_.exchange(false)) {
            rxBuffer_.clear();
        }
        auto chunk = readSome(timeoutMs);
        if (rxResetRequested_.exchange(false)) {
            rxBuffer_.clear();
        }
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

std::vector<std::vector<std::uint8_t>> IecTcpClient::drainFt12Frames(int timeoutMs, int maxFrames, bool* sawLinkResponseOut) {
    bool sawLinkResponse = false;
    struct Report {
        bool* out;
        bool& value;
        ~Report() { if (out) *out = value; }
    } report{sawLinkResponseOut, sawLinkResponse};
    std::lock_guard<std::mutex> rxLock(rxMutex_);
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        if (rxResetRequested_.exchange(false)) {
            rxBuffer_.clear();
        }
        auto chunk = readSome(timeoutMs);
        if (rxResetRequested_.exchange(false)) {
            rxBuffer_.clear();
        }
        if (chunk.empty() && rxBuffer_.empty()) {
            break;
        }
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
        if (extractFt12Frames(rxBuffer_, iec_.linkAddressSize, frames, maxFrames, sawLinkResponse)) {
            return frames;
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
        bool sawLinkResponse = false;
        const auto frames = drainFt12Frames(serial_.timeoutMs, iec_.maxPollFrames, &sawLinkResponse);
        if (iec_.pollOnCollect && !sawLinkResponse) {
            throw std::runtime_error("IEC101 no response to interrogation");
        }
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
        bool sawLinkResponse = false;
        const auto frames = drainFt12Frames(serial_.timeoutMs, iec_.maxPollFrames, &sawLinkResponse);
        if (iec_.pollOnCollect && !sawLinkResponse) {
            throw std::runtime_error("IEC103 no response to general interrogation");
        }
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

std::vector<std::vector<std::uint8_t>> IecSerialClient::drainFt12Frames(int timeoutMs, int maxFrames, bool* sawLinkResponseOut) {
    bool sawLinkResponse = false;
    struct Report {
        bool* out;
        bool& value;
        ~Report() { if (out) *out = value; }
    } report{sawLinkResponseOut, sawLinkResponse};
    std::vector<std::vector<std::uint8_t>> frames;
    for (int attempt = 0; attempt < std::max(1, maxFrames); ++attempt) {
        auto chunk = serialPort_->read(512, timeoutMs);
        if (chunk.empty() && rxBuffer_.empty()) {
            break;
        }
        rxBuffer_.insert(rxBuffer_.end(), chunk.begin(), chunk.end());
        if (extractFt12Frames(rxBuffer_, iec_.linkAddressSize, frames, maxFrames, sawLinkResponse)) {
            return frames;
        }
        if (!frames.empty() && chunk.empty()) {
            break;
        }
    }
    return frames;
}

}  // namespace edge_gateway
