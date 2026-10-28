#include "edge_gateway/iec_client.hpp"
#include "edge_gateway/channel_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
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

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();
// A GI/CI without ACT_TERM blocks the next one for at least this long (or one period).
constexpr std::int64_t kUnterminatedRetryMs = 300000;

bool systemCommandDue(const std::int64_t nextDueMs, bool inProgress, std::int64_t sentMs,
    int intervalMs, std::int64_t now) {
    if (now < nextDueMs) {
        return false;
    }
    return !inProgress || now - sentMs >= std::max<std::int64_t>(intervalMs, kUnterminatedRetryMs);
}

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

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

bool isNegativeCause(int cause) {
    return cause >= kIecCauseUnknownType && cause <= kIecCauseUnknownObjectAddress;
}

std::string negativeCauseText(int cause) {
    switch (cause) {
        case kIecCauseUnknownType: return "COT 44 unknown type identification";
        case kIecCauseUnknownCause: return "COT 45 unknown cause of transmission";
        case kIecCauseUnknownCommonAddress: return "COT 46 unknown common address";
        case kIecCauseUnknownObjectAddress: return "COT 47 unknown information object address";
        default: return "COT " + std::to_string(cause);
    }
}

int controlIoaOf(const PointDefinition& point) {
    if (point.write.iec.ioa >= 0) return point.write.iec.ioa;
    if (point.read.iec.ioa >= 0) return point.read.iec.ioa;
    if (point.write.address >= 0) return point.write.address;
    return point.address > 0 ? point.address : -1;
}

// Command confirmations are matched on the command's own type (never the read point's type).
bool controlAddressMatches(const PointDefinition& point, const IecDataValue& value) {
    const int configuredIoa = controlIoaOf(point);
    if (configuredIoa >= 0 && configuredIoa != value.ioa) {
        return false;
    }
    const int configuredCa = point.write.iec.commonAddress > 0
        ? point.write.iec.commonAddress
        : point.read.iec.commonAddress;
    if (configuredCa > 0 && configuredCa != value.commonAddress) {
        return false;
    }
    return IecCodec::commandTypeOf(point) == value.typeId;
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

class SocketGuard {
public:
    explicit SocketGuard(SocketHandle socketHandle) : socketHandle_(socketHandle) {}
    ~SocketGuard() { closeSocket(socketHandle_); }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

private:
    SocketHandle socketHandle_;
};

Iec104Client::Iec104Client(TcpTransportConfig tcp, IecProtocolConfig iec)
    : tcp_(std::move(tcp)),
      iec_(std::move(iec)) {
}

Iec104Client::~Iec104Client() {
    stopReceiveLoop();
    disconnect();
}

std::vector<IecDataValue> Iec104Client::poll() {
    ensureConnected();
    ensureIec104Started();
    sendDueSystemCommands(steadyNowMs());
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

void Iec104Client::sendDueSystemCommands(std::int64_t steadyNow) {
    // GI after STARTDT and every interrogationIntervalMs; counter interrogation once the GI
    // has terminated. A GI/CI without ACT_TERM is not repeated before its next period.
    bool sendInterrogation = false;
    bool sendCounter = false;
    bool interrogationUnfinished = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!iec104Started_) {
            return;
        }
        if (systemCommandDue(interrogation_.nextDueMs, interrogation_.inProgress, interrogation_.sentMs,
                iec_.interrogationIntervalMs, steadyNow)) {
            sendInterrogation = true;
            interrogationUnfinished = interrogation_.inProgress;
            interrogation_.inProgress = true;
            interrogation_.confirmed = false;
            interrogation_.sentMs = steadyNow;
            interrogation_.nextDueMs = iec_.interrogationIntervalMs > 0
                ? steadyNow + iec_.interrogationIntervalMs
                : kNever;
        } else if (!interrogation_.inProgress &&
            systemCommandDue(counterInterrogation_.nextDueMs, counterInterrogation_.inProgress,
                counterInterrogation_.sentMs, iec_.counterInterrogationIntervalMs, steadyNow)) {
            sendCounter = true;
            counterInterrogation_.inProgress = true;
            counterInterrogation_.confirmed = false;
            counterInterrogation_.sentMs = steadyNow;
            counterInterrogation_.nextDueMs = iec_.counterInterrogationIntervalMs > 0
                ? steadyNow + iec_.counterInterrogationIntervalMs
                : kNever;
        }
    }
    if (sendInterrogation) {
        if (interrogationUnfinished) {
            std::cerr << "IEC104 general interrogation to " << tcp_.host << ":" << tcp_.port
                      << " was not terminated (no ACT_TERM); sending the next one" << std::endl;
        }
        sendIec104IFrame(IecCodec::buildIec104InterrogationCommand(iec_, 0, 0));
    } else if (sendCounter) {
        sendIec104IFrame(IecCodec::buildIec104CounterInterrogationCommand(iec_, 0, 0));
    }
}

std::vector<IecDataValue> Iec104Client::drainBufferedValues() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return drainBufferedValuesLocked();
}

CommandResult Iec104Client::writeByPoint(
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

    const auto fail = [&result](const std::string& message) {
        result.success = false;
        result.outcomeUnknown = false;
        result.message = message;
        return result;
    };
    const auto unknown = [&result](const std::string& message) {
        result.success = false;
        result.outcomeUnknown = true;
        result.message = message;
        return result;
    };

    std::vector<std::uint8_t> selectFrame;
    std::vector<std::uint8_t> executeFrame;
    try {
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
        // Build both frames first: an unencodable value fails before anything is sent.
        if (point.write.iec.selectBeforeExecute) {
            selectFrame = IecCodec::buildIec104ControlCommand(iec_, point, value, 0, 0, true, false, nowMs());
        }
        executeFrame = IecCodec::buildIec104ControlCommand(iec_, point, value, 0, 0, false, false, nowMs());
        ensureConnected();
        ensureIec104Started();
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }

    const auto timeoutMs = point.write.iec.timeoutMs > 0 ? point.write.iec.timeoutMs : iec_.t1Ms;
    // Late confirmations of an earlier (timed-out) command must never confirm this one.
    clearControlConfirmations(point);
    std::string detail;
    bool selectionMayBeActive = false;
    if (!selectFrame.empty()) {
        bool selectSent = false;
        try {
            sendIec104IFrame(selectFrame, &selectSent);
        } catch (const std::exception& ex) {
            // A select alone never operates the equipment.
            if (selectSent) {
                sendDeselect(point, value);
            }
            return fail(std::string("IEC104 select not sent: ") + ex.what());
        }
        selectionMayBeActive = true;
        const auto selected = waitForControlConfirmation(point, value, kIecCauseActivationConfirmation, timeoutMs, 1, &detail);
        if (selected != Confirmation::Confirmed) {
            if (selected == Confirmation::Timeout) {
                sendDeselect(point, value);
            }
            return fail(selected == Confirmation::Timeout
                ? "IEC104 select confirmation timeout (deselect sent)"
                : "IEC104 select rejected: " + detail);
        }
        clearControlConfirmations(point);
    }

    bool executeSent = false;
    try {
        sendIec104IFrame(executeFrame, &executeSent);
    } catch (const std::exception& ex) {
        if (!executeSent) {
            if (selectionMayBeActive) {
                sendDeselect(point, value);
            }
            return fail(std::string("IEC104 execute not sent: ") + ex.what());
        }
        return unknown(std::string("IEC104 execute send interrupted: ") + ex.what());
    }
    const auto executed = waitForControlConfirmation(point, value, kIecCauseActivationConfirmation, timeoutMs, 0, &detail);
    if (executed == Confirmation::Negative) {
        if (selectionMayBeActive) {
            sendDeselect(point, value);
        }
        return fail("IEC104 execute rejected: " + detail);
    }
    if (executed == Confirmation::Timeout) {
        if (selectionMayBeActive) {
            sendDeselect(point, value);
        }
        return unknown("IEC104 execute confirmation timeout: command sent, outcome unknown");
    }
    if (point.write.iec.waitActivationTermination) {
        const auto terminated = waitForControlConfirmation(
            point, value, iec_.activationTerminationCot, timeoutMs, 0, &detail);
        if (terminated == Confirmation::Timeout) {
            return unknown("IEC104 activation termination timeout: command confirmed, completion unknown");
        }
        if (terminated == Confirmation::Negative) {
            return unknown("IEC104 negative activation termination after ACT_CON: " + detail);
        }
    }
    result.success = true;
    result.outcomeUnknown = false;
    result.message = "ok";
    return result;
}

void Iec104Client::sendDeselect(const PointDefinition& point, double value) {
    // S/E=1 with COT 8 (deactivation) releases a selection held by the station.
    try {
        sendIec104IFrame(IecCodec::buildIec104ControlCommand(iec_, point, value, 0, 0, true, true, nowMs()));
    } catch (const std::exception& ex) {
        std::cerr << "IEC104 deselect failed for point " << point.pointCode << ": " << ex.what() << std::endl;
    }
}

void Iec104Client::synchronizeClock(std::int64_t nowMsValue) {
    ensureConnected();
    ensureIec104Started();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        clockSyncReply_ = 0;
    }
    sendIec104IFrame(IecCodec::buildIec104ClockSyncCommand(iec_, 0, 0, nowMsValue));
    std::unique_lock<std::mutex> lock(stateMutex_);
    const auto replied = stateChanged_.wait_for(lock, std::chrono::milliseconds(std::max(1, iec_.t1Ms)), [&] {
        return clockSyncReply_ != 0 || !connected_.load();
    });
    if (!replied || clockSyncReply_ == 0) {
        throw std::runtime_error("IEC104 clock sync confirmation timeout");
    }
    if (clockSyncReply_ < 0) {
        throw std::runtime_error("IEC104 clock sync rejected by controlled station");
    }
}

IecParameterValue Iec104Client::readParameter(int ioa, int typeId, std::uint8_t qualifier, int timeoutMs) {
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

CommandResult Iec104Client::writeParameter(int ioa, int typeId, double value, std::uint8_t qualifier, int timeoutMs) {
    CommandResult result;
    result.ts = nowMs();
    result.requestedValue = value;
    try {
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

CommandResult Iec104Client::activateParameter(int ioa, std::uint8_t qualifier, int timeoutMs) {
    CommandResult result;
    result.ts = nowMs();
    try {
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

std::vector<IecFileSegment> Iec104Client::callFile(
    int ioa,
    int nameOfFile,
    int nameOfSection,
    std::uint8_t qualifier,
    int timeoutMs
) {
    ensureConnected();
    ensureIec104Started();
    sendIec104IFrame(IecCodec::buildIec104FileCallCommand(
        iec_, ioa, nameOfFile, nameOfSection, qualifier, 0, 0));
    return waitForFileSegments(nameOfFile, timeoutMs);
}

void Iec104Client::ensureConnected() {
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
        interrogation_ = SystemCommandState();
        counterInterrogation_ = SystemCommandState();
        interrogation_.nextDueMs = kNever;
        counterInterrogation_.nextDueMs = kNever;
    }
    if (iec_.backgroundReceive) {
        startReceiveLoop();
    }
}

void Iec104Client::ensureIec104Started() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (iec104Started_) {
            return;
        }
    }
    sendAll(IecCodec::buildIec104StartDtAct());
    // STARTDT is a U-format APDU: its confirmation is supervised by t1 (IEC 60870-5-104 6.9).
    const int timeoutMs = std::max(1, iec_.t1Ms);
    if (iec_.backgroundReceive) {
        if (!waitForIec104Start(timeoutMs)) {
            disconnect();
            throw std::runtime_error("IEC104 STARTDT confirmation timeout; connection closed");
        }
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        const auto frames = drainIec104Frames(std::max(1, std::min(iec_.pollTimeoutMs, remaining)), iec_.maxPollFrames);
        for (const auto& frame : frames) {
            handleIec104Frame(frame);
        }
        if (waitForIec104Start(1)) {
            return;
        }
    }
    disconnect();
    throw std::runtime_error("IEC104 STARTDT confirmation timeout; connection closed");
}

void Iec104Client::startReceiveLoop() {
    bool expected = false;
    if (!receiveRunning_.compare_exchange_strong(expected, true)) {
        return;
    }
    receiveThread_ = std::thread(&Iec104Client::receiveLoop, this);
}

void Iec104Client::stopReceiveLoop() {
    receiveRunning_.store(false);
    disconnect();
    stateChanged_.notify_all();
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

void Iec104Client::receiveLoop() {
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

void Iec104Client::disconnect(std::uint64_t expectedGeneration) {
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
            interrogation_.inProgress = false;
            counterInterrogation_.inProgress = false;
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

std::intptr_t Iec104Client::duplicateConnectedSocket(std::uint64_t& generation) const {
    std::lock_guard<std::mutex> socketLock(socketMutex_);
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        generation = 0;
        return static_cast<std::intptr_t>(kInvalidSocket);
    }
    generation = socketGeneration_;
    return static_cast<std::intptr_t>(duplicateSocket(static_cast<SocketHandle>(socket_)));
}

void Iec104Client::configureSocketTimeouts(std::intptr_t socketValue) const {
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

void Iec104Client::sendIec104IFrame(const std::vector<std::uint8_t>& bytes, bool* handedToSocket) {
    if (handedToSocket) {
        *handedToSocket = false;
    }
    if (!IecCodec::isIec104IFrame(bytes)) {
        throw std::invalid_argument("IEC104 I-frame expected");
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
    if (handedToSocket) {
        *handedToSocket = true;
    }
    sendAll(frame);
}

void Iec104Client::sendAll(const std::vector<std::uint8_t>& bytes) {
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

std::vector<std::uint8_t> Iec104Client::readSome(int timeoutMs) {
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
    bytes.resize(static_cast<std::size_t>(rc));
    traceBytes(false, bytes.data(), bytes.size());
    return bytes;
}

void Iec104Client::handleIec104Frame(const std::vector<std::uint8_t>& frame) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastReceiveMs_ = nowMs();
    }
    if (IecCodec::isIec104StartDtCon(frame)) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!iec104Started_) {
                const auto now = steadyNowMs();
                interrogation_.nextDueMs = iec_.pollOnCollect ? now : kNever;
                counterInterrogation_.nextDueMs = iec_.counterInterrogation ? now : kNever;
            }
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
    const auto receivedSequence = IecCodec::iec104SendSequence(frame);
    bool sequenceValid = true;
    std::uint16_t expectedSequence = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        // N(S) must equal V(R): a gap means lost I-frames, a smaller value a duplicate.
        expectedSequence = receiveSequence_;
        sequenceValid = receivedSequence == receiveSequence_;
        if (sequenceValid) {
            receiveSequence_ = static_cast<std::uint16_t>((receivedSequence + 1) & 0x7FFF);
            ackSequence = receiveSequence_;
            ++unacknowledgedReceived_;
            shouldAck = iec_.sendSFrameAck && unacknowledgedReceived_ >= static_cast<std::uint16_t>(std::max(1, iec_.wAck));
            if (shouldAck) {
                unacknowledgedReceived_ = 0;
                lastAckMs_ = nowMs();
            }
        }
    }
    if (!sequenceValid) {
        disconnect();
        throw std::runtime_error(
            "IEC104 protocol error: received N(S)=" + std::to_string(receivedSequence) +
            " but V(R)=" + std::to_string(expectedSequence) + "; connection closed");
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
        if (decoded.typeId == 100 || decoded.typeId == 101 || decoded.typeId == 103) {
            handleSystemCommandReply(decoded);
            return;
        }
        if (!decoded.values.empty()) {
            if (IecCodec::isCommandType(decoded.typeId)) {
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
                values.push_back(IecCodec::protectionEventValue(event));
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

void Iec104Client::handleSystemCommandReply(const IecAsdu& asdu) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const bool rejected = isNegativeCause(asdu.cause) ||
        (asdu.cause == kIecCauseActivationConfirmation && asdu.negative);
    if (asdu.typeId == 103) {
        if (asdu.cause == kIecCauseActivationConfirmation || isNegativeCause(asdu.cause)) {
            clockSyncReply_ = rejected ? -1 : 1;
            stateChanged_.notify_all();
        }
        return;
    }
    auto& state = asdu.typeId == 100 ? interrogation_ : counterInterrogation_;
    const char* what = asdu.typeId == 100 ? "general interrogation" : "counter interrogation";
    if (rejected) {
        state.inProgress = false;
        std::cerr << "IEC104 " << what << " rejected by " << tcp_.host << ":" << tcp_.port
                  << (isNegativeCause(asdu.cause) ? " (" + negativeCauseText(asdu.cause) + ")" : " (negative ACT_CON)")
                  << std::endl;
    } else if (asdu.cause == kIecCauseActivationConfirmation) {
        state.confirmed = true;
    } else if (asdu.cause == kIecCauseActivationTermination) {
        state.inProgress = false;
    }
    stateChanged_.notify_all();
}

void Iec104Client::bufferValues(const std::vector<IecDataValue>& values) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    bufferedValues_.insert(bufferedValues_.end(), values.begin(), values.end());
    if (bufferedValues_.size() > 4096) {
        bufferedValues_.erase(bufferedValues_.begin(), bufferedValues_.begin() + static_cast<std::ptrdiff_t>(bufferedValues_.size() - 4096));
    }
    stateChanged_.notify_all();
}

std::vector<IecDataValue> Iec104Client::drainBufferedValuesLocked() {
    std::vector<IecDataValue> values;
    values.swap(bufferedValues_);
    return values;
}

bool Iec104Client::waitForIec104Start(int timeoutMs) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    return stateChanged_.wait_for(lock, std::chrono::milliseconds(std::max(1, timeoutMs)), [&] {
        return iec104Started_;
    });
}

Iec104Client::Confirmation Iec104Client::waitForControlConfirmation(
    const PointDefinition& point,
    double value,
    int expectedCause,
    int timeoutMs,
    int expectedSelect,
    std::string* detail
) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    std::unique_lock<std::mutex> lock(stateMutex_);
    while (true) {
        for (auto it = controlValues_.begin(); it != controlValues_.end(); ++it) {
            if (!controlAddressMatches(point, *it)) {
                continue;
            }
            if (isNegativeCause(it->cause)) {
                if (detail) {
                    *detail = negativeCauseText(it->cause);
                }
                controlValues_.erase(it);
                return Confirmation::Negative;
            }
            if (it->cause != expectedCause) {
                continue;
            }
            if (it->negative) {
                if (detail) {
                    *detail = "negative confirmation (COT " + std::to_string(expectedCause) + ", P/N=1)";
                }
                controlValues_.erase(it);
                return Confirmation::Negative;
            }
            if (!controlEchoMatches(point, value, *it, expectedSelect)) {
                continue;
            }
            controlValues_.erase(it);
            return Confirmation::Confirmed;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return Confirmation::Timeout;
        }
        stateChanged_.wait_until(lock, deadline);
    }
}

void Iec104Client::clearControlConfirmations(const PointDefinition& point) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    controlValues_.erase(
        std::remove_if(controlValues_.begin(), controlValues_.end(), [&](const IecDataValue& candidate) {
            return controlAddressMatches(point, candidate);
        }),
        controlValues_.end());
}

bool Iec104Client::waitForParameterConfirmation(
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
            if (it->ioa != ioa || it->typeId != typeId) {
                continue;
            }
            if (isNegativeCause(it->cause)) {
                const auto cause = it->cause;
                parameterValues_.erase(it);
                throw std::runtime_error("IEC104 parameter rejected (" + negativeCauseText(cause) + ")");
            }
            if (it->cause == expectedCause) {
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

void Iec104Client::clearParameterConfirmations(int ioa, int typeId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    parameterValues_.erase(
        std::remove_if(parameterValues_.begin(), parameterValues_.end(), [&](const IecParameterValue& candidate) {
            return candidate.ioa == ioa && candidate.typeId == typeId;
        }),
        parameterValues_.end());
}

bool Iec104Client::noteRemoteAcknowledgeLocked(std::uint16_t receiveSequence) {
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

bool Iec104Client::t1SupervisionExpired(std::int64_t nowMsValue) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto t1 = static_cast<std::int64_t>(std::max(1, iec_.t1Ms));
    if (testFrPendingSinceMs_ > 0 && nowMsValue - testFrPendingSinceMs_ >= t1) {
        return true;
    }
    return !unackedIFrames_.empty() && nowMsValue - unackedIFrames_.front().second >= t1;
}

std::vector<IecFileSegment> Iec104Client::waitForFileSegments(int nameOfFile, int timeoutMs) {
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

std::vector<std::vector<std::uint8_t>> Iec104Client::drainIec104Frames(int timeoutMs, int maxFrames) {
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
            // A TCP byte stream from a conforming station only contains APDUs: a wrong start
            // octet or an APCI length outside 4..253 means framing is lost (IEC 60870-5-104 5.1).
            if (rxBuffer_[0] != 0x68 || rxBuffer_[1] < 4 || rxBuffer_[1] > 253) {
                const auto start = static_cast<int>(rxBuffer_[0]);
                const auto length = static_cast<int>(rxBuffer_[1]);
                rxBuffer_.clear();
                disconnect();
                throw std::runtime_error(
                    "IEC104 protocol error: invalid APCI (start=" + std::to_string(start) +
                    ", length=" + std::to_string(length) + "); connection closed");
            }
            const auto frameSize = static_cast<std::size_t>(rxBuffer_[1]) + 2;
            if (rxBuffer_.size() < frameSize) {
                break;
            }
            std::vector<std::uint8_t> frame(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
            frames.push_back(std::move(frame));
            if (static_cast<int>(frames.size()) >= maxFrames) {
                return frames;
            }
        }
        if (!frames.empty() && chunk.empty()) {
            break;
        }
    }
    return frames;
}

}  // namespace edge_gateway
