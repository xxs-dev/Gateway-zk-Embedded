#include "edge_gateway/modbus_tcp_client.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

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
        throw std::runtime_error("modbus tcp pdu too short");
    }
    if (pdu[0] != function) {
        throw std::runtime_error("modbus tcp function mismatch");
    }
    if (pdu.size() != static_cast<std::size_t>(2 + expectedCount * 2)) {
        throw std::runtime_error("modbus tcp unexpected byte count");
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
        throw std::runtime_error("modbus tcp bit pdu too short");
    }
    if (pdu[0] != function) {
        throw std::runtime_error("modbus tcp bit function mismatch");
    }
    const auto byteCount = static_cast<std::size_t>(pdu[1]);
    const auto expectedByteCount = static_cast<std::size_t>((expectedCount + 7) / 8);
    if (byteCount != expectedByteCount) {
        throw std::runtime_error("modbus tcp bit unexpected byte count");
    }
    if (pdu.size() != 2 + byteCount) {
        throw std::runtime_error("modbus tcp bit unexpected pdu size");
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
        throw std::runtime_error("modbus tcp write echo size mismatch");
    }
    if (pdu[0] != function) {
        throw std::runtime_error("modbus tcp write function mismatch");
    }
    const auto echoedAddress = static_cast<int>(
        static_cast<std::uint16_t>(pdu[1] << 8) | pdu[2]
    );
    const auto echoedValue = static_cast<int>(
        static_cast<std::uint16_t>(pdu[3] << 8) | pdu[4]
    );
    if (echoedAddress != address || echoedValue != countOrValue) {
        throw std::runtime_error("modbus tcp write echo mismatch");
    }
}

}  // namespace

ModbusTcpClient::ModbusTcpClient(TcpTransportConfig config)
    : config_(std::move(config)) {
}

ModbusTcpClient::~ModbusTcpClient() {
    disconnect();
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
    if (values.empty()) {
        throw std::invalid_argument("writeMultipleRegisters requires at least one value");
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
    if (slave < 0 || slave > 255) {
        throw std::invalid_argument("unit id must be in range 0..255");
    }
    ensureConnected();

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

    sendAll(frame);

    const auto header = readExact(7);
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
        throw std::runtime_error("modbus tcp transaction id mismatch");
    }
    if (protocolId != 0) {
        throw std::runtime_error("modbus tcp protocol id mismatch");
    }
    if (unitId != static_cast<std::uint8_t>(slave & 0xFF)) {
        throw std::runtime_error("modbus tcp unit id mismatch");
    }
    if (lengthField < 2) {
        throw std::runtime_error("modbus tcp length too short");
    }

    const auto body = readExact(static_cast<std::size_t>(lengthField - 1));
    if (body.empty()) {
        throw std::runtime_error("modbus tcp empty pdu");
    }
    if (body[0] == static_cast<std::uint8_t>(function | 0x80U)) {
        if (body.size() < 2) {
            throw std::runtime_error("modbus tcp exception without code");
        }
        throw std::runtime_error("modbus tcp exception code " + std::to_string(body[1]));
    }
    if (body[0] != function) {
        throw std::runtime_error("modbus tcp function mismatch");
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
        throw std::runtime_error("modbus tcp getaddrinfo failed");
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
        throw std::runtime_error("modbus tcp connect failed");
    }

    socket_ = static_cast<std::intptr_t>(connected);
    configureSocketTimeouts();
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
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout;
    timeout.tv_sec = config_.timeoutMs / 1000;
    timeout.tv_usec = (config_.timeoutMs % 1000) * 1000;
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

std::vector<std::uint8_t> ModbusTcpClient::readExact(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto rc = recv(static_cast<SocketHandle>(socket_), reinterpret_cast<char*>(bytes.data() + received), static_cast<int>(size - received), 0);
#else
        const auto rc = recv(static_cast<SocketHandle>(socket_), bytes.data() + received, size - received, 0);
#endif
        if (rc <= 0) {
            disconnect();
            throw std::runtime_error("modbus tcp recv failed");
        }
        received += static_cast<std::size_t>(rc);
    }
    return bytes;
}

void ModbusTcpClient::sendAll(const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(static_cast<SocketHandle>(socket_), reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(static_cast<SocketHandle>(socket_), bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (rc <= 0) {
            disconnect();
            throw std::runtime_error("modbus tcp send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

}  // namespace edge_gateway
