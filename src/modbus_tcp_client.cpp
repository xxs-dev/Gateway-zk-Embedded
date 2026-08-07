#include "edge_gateway/modbus_tcp_client.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edge_gateway/modbus_error.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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

thread_local int ModbusTcpClient::priorityContextDepth_ = 0;

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

constexpr int kMaxReadRegisters = 125;
constexpr int kMaxWriteRegisters = 123;
constexpr int kMaxReadBits = 2000;
constexpr std::uint16_t kMaxMbapLength = 254;

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
    const auto nextFlags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(socketHandle, F_SETFL, nextFlags) == 0;
#endif
}

bool waitForSocket(
    SocketHandle socketHandle,
    bool writable,
    std::chrono::steady_clock::time_point deadline
) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remainingUs / 1000000);
        timeout.tv_usec = static_cast<long>(remainingUs % 1000000);

        fd_set readySet;
        FD_ZERO(&readySet);
        FD_SET(socketHandle, &readySet);
        fd_set errorSet;
        FD_ZERO(&errorSet);
        FD_SET(socketHandle, &errorSet);
#ifdef _WIN32
        const auto rc = select(
            0,
            writable ? nullptr : &readySet,
            writable ? &readySet : nullptr,
            &errorSet,
            &timeout
        );
#else
        const auto rc = select(
            socketHandle + 1,
            writable ? nullptr : &readySet,
            writable ? &readySet : nullptr,
            &errorSet,
            &timeout
        );
#endif
        if (rc > 0) {
            return !FD_ISSET(socketHandle, &errorSet);
        }
        if (rc == 0) {
            return false;
        }
        if (!isInterrupted(lastSocketError())) {
            throw ModbusError(ModbusFailureKind::Transport, "modbus tcp socket wait failed");
        }
    }
}

std::vector<std::uint8_t> makeWord(int value) {
    if (value < 0 || value > 0xFFFF) {
        throw std::invalid_argument("modbus word out of range");
    }
    return {
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
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

std::vector<std::uint16_t> decodeRegistersFromReadResponse(
    const std::vector<std::uint8_t>& pdu,
    std::uint8_t function,
    int expectedCount
) {
    if (pdu.size() < 2) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp pdu too short");
    }
    if (pdu[0] != function) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp function mismatch");
    }
    if (pdu.size() != static_cast<std::size_t>(2 + expectedCount * 2)) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp unexpected byte count");
    }

    std::vector<std::uint16_t> registers;
    registers.reserve(static_cast<std::size_t>(expectedCount));
    for (int i = 0; i < expectedCount; ++i) {
        const auto hi = static_cast<std::uint16_t>(pdu[2 + i * 2]);
        const auto lo = static_cast<std::uint16_t>(pdu[3 + i * 2]);
        registers.push_back(static_cast<std::uint16_t>((hi << 8) | lo));
    }
    return registers;
}

std::vector<std::uint16_t> decodeBitsFromReadResponse(
    const std::vector<std::uint8_t>& pdu,
    std::uint8_t function,
    int expectedCount
) {
    if (pdu.size() < 2) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp bit pdu too short");
    }
    if (pdu[0] != function) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp bit function mismatch");
    }
    const auto byteCount = static_cast<std::size_t>(pdu[1]);
    const auto expectedByteCount = static_cast<std::size_t>((expectedCount + 7) / 8);
    if (byteCount != expectedByteCount) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp bit unexpected byte count");
    }
    if (pdu.size() != 2 + byteCount) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp bit unexpected pdu size");
    }

    std::vector<std::uint16_t> bits;
    bits.reserve(static_cast<std::size_t>(expectedCount));
    for (int i = 0; i < expectedCount; ++i) {
        const auto byte = pdu[2 + static_cast<std::size_t>(i / 8)];
        bits.push_back(static_cast<std::uint16_t>((byte >> (i % 8)) & 0x01U));
    }
    return bits;
}

void validateWriteEcho(
    const std::vector<std::uint8_t>& pdu,
    std::uint8_t function,
    int address,
    int countOrValue
) {
    if (pdu.size() != 5) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp write echo size mismatch");
    }
    if (pdu[0] != function) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp write function mismatch");
    }
    const auto echoedAddress = static_cast<int>(
        static_cast<std::uint16_t>(pdu[1] << 8) | pdu[2]
    );
    const auto echoedValue = static_cast<int>(
        static_cast<std::uint16_t>(pdu[3] << 8) | pdu[4]
    );
    if (echoedAddress != address || echoedValue != countOrValue) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp write echo mismatch");
    }
}

class ModbusTcpTransactionScope {
public:
    explicit ModbusTcpTransactionScope(ModbusTcpClient& client) : client_(client) {
        client_.enterTransaction();
    }

    ~ModbusTcpTransactionScope() {
        client_.leaveTransaction();
    }

    ModbusTcpTransactionScope(const ModbusTcpTransactionScope&) = delete;
    ModbusTcpTransactionScope& operator=(const ModbusTcpTransactionScope&) = delete;

private:
    ModbusTcpClient& client_;
};

class ModbusTcpPriorityWriteScope {
public:
    explicit ModbusTcpPriorityWriteScope(ModbusTcpClient& client) : client_(client) {
        client_.beginPriorityWrite();
    }

    ~ModbusTcpPriorityWriteScope() {
        client_.endPriorityWrite();
    }

    ModbusTcpPriorityWriteScope(const ModbusTcpPriorityWriteScope&) = delete;
    ModbusTcpPriorityWriteScope& operator=(const ModbusTcpPriorityWriteScope&) = delete;

private:
    ModbusTcpClient& client_;
};

}  // namespace

ModbusTcpClient::ModbusTcpClient(TcpTransportConfig config, int maxRequestRegisters)
    : config_(std::move(config)),
      maxRequestRegisters_(maxRequestRegisters > 0 ? maxRequestRegisters : kMaxReadRegisters) {
    if (config_.connectTimeoutMs <= 0) {
        throw std::invalid_argument("modbus tcp connectTimeoutMs must be positive");
    }
    if (config_.timeoutMs <= 0) {
        throw std::invalid_argument("modbus tcp timeoutMs must be positive");
    }
}

ModbusTcpClient::~ModbusTcpClient() {
    disconnect();
}

void ModbusTcpClient::beginPriorityWrite() {
    ++priorityContextDepth_;
    if (priorityContextDepth_ == 1) {
        std::lock_guard<std::mutex> lock(transactionMutex_);
        ++pendingPriorityWrites_;
        transactionCv_.notify_all();
    }
}

void ModbusTcpClient::endPriorityWrite() {
    if (priorityContextDepth_ <= 0) {
        return;
    }
    --priorityContextDepth_;
    if (priorityContextDepth_ == 0) {
        {
            std::lock_guard<std::mutex> lock(transactionMutex_);
            if (pendingPriorityWrites_ > 0) {
                --pendingPriorityWrites_;
            }
        }
        transactionCv_.notify_all();
    }
}

void ModbusTcpClient::enterTransaction() {
    std::unique_lock<std::mutex> lock(transactionMutex_);
    const bool priorityContext = priorityContextDepth_ > 0;
    transactionCv_.wait(lock, [&] {
        if (activeTransactions_ != 0) {
            return false;
        }
        return priorityContext || pendingPriorityWrites_ == 0;
    });
    activeTransactions_ = 1;
}

void ModbusTcpClient::leaveTransaction() {
    {
        std::lock_guard<std::mutex> lock(transactionMutex_);
        activeTransactions_ = 0;
    }
    transactionCv_.notify_all();
}

std::vector<std::uint16_t> ModbusTcpClient::readCoils(int slave, int start, int count) {
    return executeBitRead(slave, 0x01, start, count);
}

std::vector<std::uint16_t> ModbusTcpClient::readDiscreteInputs(int slave, int start, int count) {
    return executeBitRead(slave, 0x02, start, count);
}

std::vector<std::uint16_t> ModbusTcpClient::readHoldingRegisters(int slave, int start, int count) {
    return executeRegisterRead(slave, 0x03, start, count);
}

std::vector<std::uint16_t> ModbusTcpClient::readInputRegisters(int slave, int start, int count) {
    return executeRegisterRead(slave, 0x04, start, count);
}

void ModbusTcpClient::writeSingleCoil(int slave, int address, bool value) {
    ModbusTcpPriorityWriteScope priority(*this);
    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    const auto coilValue = makeWord(value ? 0xFF00 : 0x0000);
    pdu.push_back(0x05);
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.insert(pdu.end(), coilValue.begin(), coilValue.end());

    const auto response = transact(slave, 0x05, pdu);
    validateWriteEcho(response, 0x05, address, value ? 0xFF00 : 0x0000);
}

void ModbusTcpClient::writeSingleRegister(int slave, int address, std::uint16_t value) {
    ModbusTcpPriorityWriteScope priority(*this);
    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    pdu.push_back(0x06);
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    pdu.push_back(static_cast<std::uint8_t>(value & 0xFF));

    const auto response = transact(slave, 0x06, pdu);
    validateWriteEcho(response, 0x06, address, value);
}

void ModbusTcpClient::writeMultipleRegisters(
    int slave,
    int address,
    const std::vector<std::uint16_t>& values
) {
    ModbusTcpPriorityWriteScope priority(*this);
    if (values.empty()) {
        throw std::invalid_argument("writeMultipleRegisters requires at least one value");
    }
    const auto writeLimit = std::min(maxRequestRegisters_, kMaxWriteRegisters);
    if (values.size() > static_cast<std::size_t>(writeLimit)) {
        throw std::invalid_argument(
            "modbus tcp write register count exceeds maxRequestRegisters: " + std::to_string(values.size()) +
            " > " + std::to_string(writeLimit)
        );
    }
    if (address < 0 || address > 0xFFFF ||
        static_cast<int>(values.size()) - 1 > 0xFFFF - address) {
        throw std::invalid_argument("modbus tcp write address range is invalid");
    }

    std::vector<std::uint8_t> pdu;
    const auto addr = makeWord(address);
    const auto quantity = makeWord(static_cast<int>(values.size()));
    pdu.push_back(0x10);
    pdu.insert(pdu.end(), addr.begin(), addr.end());
    pdu.insert(pdu.end(), quantity.begin(), quantity.end());
    pdu.push_back(static_cast<std::uint8_t>(values.size() * 2));
    for (std::size_t i = 0; i < values.size(); ++i) {
        pdu.push_back(static_cast<std::uint8_t>((values[i] >> 8) & 0xFF));
        pdu.push_back(static_cast<std::uint8_t>(values[i] & 0xFF));
    }

    const auto response = transact(slave, 0x10, pdu);
    validateWriteEcho(response, 0x10, address, static_cast<int>(values.size()));
}

std::vector<std::uint8_t> ModbusTcpClient::transact(
    int slave,
    std::uint8_t function,
    const std::vector<std::uint8_t>& pdu
) {
    ModbusTcpTransactionScope transaction(*this);
    if (slave < 0 || slave > 255) {
        throw std::invalid_argument("unit id must be in range 0..255");
    }
    ensureConnected();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.timeoutMs);

    ++transactionId_;
    if (transactionId_ == 0) {
        ++transactionId_;
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(7 + pdu.size());
    frame.push_back(static_cast<std::uint8_t>((transactionId_ >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(transactionId_ & 0xFF));
    frame.push_back(0x00);
    frame.push_back(0x00);
    const std::uint16_t length = static_cast<std::uint16_t>(1 + pdu.size());
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(length & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(slave & 0xFF));
    frame.insert(frame.end(), pdu.begin(), pdu.end());

    sendAll(frame, deadline);

    const auto header = readExact(7, deadline);
    const auto responseTransactionId = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(header[0] << 8) | header[1]
    );
    const auto protocolId = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(header[2] << 8) | header[3]
    );
    const auto lengthField = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(header[4] << 8) | header[5]
    );
    const auto unitId = header[6];

    if (responseTransactionId != transactionId_) {
        disconnect();
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp transaction id mismatch");
    }
    if (protocolId != 0) {
        disconnect();
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp protocol id mismatch");
    }
    if (unitId != static_cast<std::uint8_t>(slave & 0xFF)) {
        disconnect();
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp unit id mismatch");
    }
    if (lengthField < 2 || lengthField > kMaxMbapLength) {
        disconnect();
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp invalid length");
    }

    const auto body = readExact(static_cast<std::size_t>(lengthField - 1), deadline);
    if (body.empty()) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp empty pdu");
    }
    if (body[0] == static_cast<std::uint8_t>(function | 0x80U)) {
        if (body.size() < 2) {
            throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp exception without code");
        }
        throw ModbusError(
            ModbusFailureKind::ProtocolException,
            "modbus tcp exception code " + std::to_string(body[1])
        );
    }
    if (body[0] != function) {
        throw ModbusError(ModbusFailureKind::MalformedFrame, "modbus tcp function mismatch");
    }
    return body;
}

std::vector<std::uint16_t> ModbusTcpClient::executeRegisterRead(
    int slave,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0) {
        throw std::invalid_argument("read count must be positive");
    }
    const auto readLimit = std::min(maxRequestRegisters_, kMaxReadRegisters);
    if (count > readLimit) {
        throw std::invalid_argument(
            "modbus tcp read count exceeds maxRequestRegisters: " + std::to_string(count) +
            " > " + std::to_string(readLimit)
        );
    }
    if (start < 0 || start > 0xFFFF || count - 1 > 0xFFFF - start) {
        throw std::invalid_argument("modbus tcp read address range is invalid");
    }

    std::vector<std::uint8_t> pdu;
    const auto startWord = makeWord(start);
    const auto countWord = makeWord(count);
    pdu.push_back(function);
    pdu.insert(pdu.end(), startWord.begin(), startWord.end());
    pdu.insert(pdu.end(), countWord.begin(), countWord.end());

    const auto response = transact(slave, function, pdu);
    return decodeRegistersFromReadResponse(response, function, count);
}

std::vector<std::uint16_t> ModbusTcpClient::executeBitRead(
    int slave,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0) {
        throw std::invalid_argument("read count must be positive");
    }
    const auto readLimit = std::min(maxRequestRegisters_, kMaxReadBits);
    if (count > readLimit) {
        throw std::invalid_argument(
            "modbus tcp bit read count exceeds maxRequestRegisters: " + std::to_string(count) +
            " > " + std::to_string(readLimit)
        );
    }
    if (start < 0 || start > 0xFFFF || count - 1 > 0xFFFF - start) {
        throw std::invalid_argument("modbus tcp bit read address range is invalid");
    }

    std::vector<std::uint8_t> pdu;
    const auto startWord = makeWord(start);
    const auto countWord = makeWord(count);
    pdu.push_back(function);
    pdu.insert(pdu.end(), startWord.begin(), startWord.end());
    pdu.insert(pdu.end(), countWord.begin(), countWord.end());

    const auto response = transact(slave, function, pdu);
    return decodeBitsFromReadResponse(response, function, count);
}

void ModbusTcpClient::ensureConnected() {
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
    const auto port = std::to_string(config_.port);
    if (getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result) != 0) {
        throw ModbusError(ModbusFailureKind::Transport, "modbus tcp getaddrinfo failed");
    }

    SocketHandle connected = kInvalidSocket;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(config_.connectTimeoutMs);
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
            if (!isConnectInProgress(error) || !waitForSocket(connected, true, deadline)) {
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
        throw ModbusError(ModbusFailureKind::Transport, "modbus tcp connect failed");
    }

    socket_ = static_cast<std::intptr_t>(connected);
    try {
        configureSocketTimeouts();
    } catch (...) {
        disconnect();
        throw;
    }
}

void ModbusTcpClient::disconnect() {
    if (socket_ == static_cast<std::intptr_t>(kInvalidSocket)) {
        return;
    }
    closeSocket(static_cast<SocketHandle>(socket_));
    socket_ = static_cast<std::intptr_t>(kInvalidSocket);
}

void ModbusTcpClient::configureSocketTimeouts() const {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(config_.timeoutMs);
    if (setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
        setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        throw ModbusError(ModbusFailureKind::Transport, "modbus tcp configure socket timeout failed");
    }
#else
    timeval timeout;
    timeout.tv_sec = config_.timeoutMs / 1000;
    timeout.tv_usec = (config_.timeoutMs % 1000) * 1000;
    if (setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw ModbusError(ModbusFailureKind::Transport, "modbus tcp configure socket timeout failed");
    }
#endif
}

std::vector<std::uint8_t> ModbusTcpClient::readExact(
    std::size_t size,
    std::chrono::steady_clock::time_point deadline
) {
    std::vector<std::uint8_t> bytes(size);
    std::size_t received = 0;
    while (received < size) {
        if (!waitForSocket(static_cast<SocketHandle>(socket_), false, deadline)) {
            disconnect();
            throw ModbusError(ModbusFailureKind::Timeout, "modbus tcp recv timeout");
        }
#ifdef _WIN32
        const auto rc = recv(static_cast<SocketHandle>(socket_), reinterpret_cast<char*>(bytes.data() + received), static_cast<int>(size - received), 0);
#else
        const auto rc = recv(static_cast<SocketHandle>(socket_), bytes.data() + received, size - received, 0);
#endif
        if (rc < 0 && isInterrupted(lastSocketError())) {
            continue;
        }
        if (rc <= 0) {
            disconnect();
            throw ModbusError(ModbusFailureKind::Transport, "modbus tcp recv failed");
        }
        received += static_cast<std::size_t>(rc);
    }
    return bytes;
}

void ModbusTcpClient::sendAll(
    const std::vector<std::uint8_t>& bytes,
    std::chrono::steady_clock::time_point deadline
) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        if (!waitForSocket(static_cast<SocketHandle>(socket_), true, deadline)) {
            disconnect();
            throw ModbusError(ModbusFailureKind::Timeout, "modbus tcp send timeout");
        }
#ifdef _WIN32
        const auto rc = send(static_cast<SocketHandle>(socket_), reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(static_cast<SocketHandle>(socket_), bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (rc < 0 && isInterrupted(lastSocketError())) {
            continue;
        }
        if (rc <= 0) {
            disconnect();
            throw ModbusError(ModbusFailureKind::Transport, "modbus tcp send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

}  // namespace edge_gateway
