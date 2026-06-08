#include "edge_gateway/modbus_northbound_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
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

constexpr std::uint8_t kIllegalFunction = 0x01;
constexpr std::uint8_t kIllegalDataAddress = 0x02;
constexpr std::uint8_t kIllegalDataValue = 0x03;
constexpr std::uint8_t kServerDeviceFailure = 0x04;

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

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::uint16_t readWord(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset] << 8) | bytes[offset + 1]
    );
}

void appendWord(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::size_t dataTypeRegisterLength(const std::string& dataType, int fallback) {
    if (dataType == "uint32" || dataType == "int32" || dataType == "float32") {
        return 2;
    }
    if (dataType == "uint64" || dataType == "int64" || dataType == "float64" || dataType == "double") {
        return 4;
    }
    return static_cast<std::size_t>(std::max(1, fallback));
}

std::vector<std::uint8_t> writeUint16BigEndian(std::uint16_t value) {
    return {
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::vector<std::uint8_t> writeUint32BigEndian(std::uint32_t value) {
    return {
        static_cast<std::uint8_t>((value >> 24) & 0xFF),
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::vector<std::uint8_t> writeUint64BigEndian(std::uint64_t value) {
    return {
        static_cast<std::uint8_t>((value >> 56) & 0xFF),
        static_cast<std::uint8_t>((value >> 48) & 0xFF),
        static_cast<std::uint8_t>((value >> 40) & 0xFF),
        static_cast<std::uint8_t>((value >> 32) & 0xFF),
        static_cast<std::uint8_t>((value >> 24) & 0xFF),
        static_cast<std::uint8_t>((value >> 16) & 0xFF),
        static_cast<std::uint8_t>((value >> 8) & 0xFF),
        static_cast<std::uint8_t>(value & 0xFF)
    };
}

std::vector<std::uint8_t> reorderBytesForEncode(
    const std::vector<std::uint8_t>& canonicalBytes,
    const std::string& byteOrder
) {
    if (byteOrder.empty()) {
        return canonicalBytes;
    }
    if (byteOrder.size() != canonicalBytes.size()) {
        throw std::invalid_argument("northbound byteOrder size does not match data width");
    }
    std::vector<std::uint8_t> reordered(byteOrder.size());
    for (std::size_t i = 0; i < byteOrder.size(); ++i) {
        const auto token = byteOrder[i];
        if (token < 'A' || static_cast<std::size_t>(token - 'A') >= canonicalBytes.size()) {
            throw std::invalid_argument("invalid northbound byteOrder token");
        }
        reordered[static_cast<std::size_t>(token - 'A')] = canonicalBytes[i];
    }
    return reordered;
}

std::vector<std::uint16_t> bytesToRegisters(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() % 2 != 0) {
        throw std::invalid_argument("northbound register bytes must be even");
    }
    std::vector<std::uint16_t> registers;
    registers.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        registers.push_back(static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[i] << 8) | bytes[i + 1]
        ));
    }
    return registers;
}

template <typename T>
std::vector<std::uint8_t> writeBigEndian(T value);

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint16_t>(std::uint16_t value) {
    return writeUint16BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int16_t>(std::int16_t value) {
    std::uint16_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint16BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint32_t>(std::uint32_t value) {
    return writeUint32BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int32_t>(std::int32_t value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint32BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::uint64_t>(std::uint64_t value) {
    return writeUint64BigEndian(value);
}

template <>
std::vector<std::uint8_t> writeBigEndian<std::int64_t>(std::int64_t value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint64BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<float>(float value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint32BigEndian(raw);
}

template <>
std::vector<std::uint8_t> writeBigEndian<double>(double value) {
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return writeUint64BigEndian(raw);
}

double removeScale(double businessValue, const ModbusNorthboundMappingConfig& mapping) {
    if (std::abs(mapping.scale) < std::numeric_limits<double>::epsilon()) {
        throw std::invalid_argument("northbound scale must not be zero");
    }
    return (businessValue - mapping.offset) / mapping.scale;
}

std::vector<std::uint16_t> encodeNumericRegisters(
    double businessValue,
    const ModbusNorthboundMappingConfig& mapping
) {
    const auto rawValue = removeScale(businessValue, mapping);
    std::vector<std::uint8_t> canonicalBytes;
    if (mapping.dataType == "bit" || mapping.dataType == "bool") {
        canonicalBytes = writeBigEndian(static_cast<std::uint16_t>(std::abs(rawValue) > 0.0 ? 1 : 0));
    } else if (mapping.dataType == "uint16" || mapping.dataType.empty()) {
        canonicalBytes = writeBigEndian(static_cast<std::uint16_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "int16") {
        canonicalBytes = writeBigEndian(static_cast<std::int16_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "uint32") {
        canonicalBytes = writeBigEndian(static_cast<std::uint32_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "int32") {
        canonicalBytes = writeBigEndian(static_cast<std::int32_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "uint64") {
        canonicalBytes = writeBigEndian(static_cast<std::uint64_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "int64") {
        canonicalBytes = writeBigEndian(static_cast<std::int64_t>(std::llround(rawValue)));
    } else if (mapping.dataType == "float32") {
        canonicalBytes = writeBigEndian(static_cast<float>(rawValue));
    } else if (mapping.dataType == "float64" || mapping.dataType == "double") {
        canonicalBytes = writeBigEndian(static_cast<double>(rawValue));
    } else {
        throw std::invalid_argument("unsupported northbound dataType: " + mapping.dataType);
    }

    const auto deviceBytes = reorderBytesForEncode(canonicalBytes, mapping.byteOrder);
    return bytesToRegisters(deviceBytes);
}

std::vector<std::uint8_t> makeException(std::uint8_t function, std::uint8_t code) {
    return {
        static_cast<std::uint8_t>(function | 0x80U),
        code
    };
}

bool isReadFunction(std::uint8_t function) {
    return function == 1 || function == 2 || function == 3 || function == 4;
}

bool parseIpv4(const std::string& value, std::uint32_t& out) {
    std::istringstream stream(value);
    std::string part;
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::getline(stream, part, '.')) {
            return false;
        }
        if (part.empty()) {
            return false;
        }
        char* end = nullptr;
        const auto octet = std::strtol(part.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || octet < 0 || octet > 255) {
            return false;
        }
        result = (result << 8) | static_cast<std::uint32_t>(octet);
    }
    if (std::getline(stream, part, '.')) {
        return false;
    }
    out = result;
    return true;
}

bool ipv4MatchesCidr(const std::string& address, const std::string& cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::uint32_t addressValue = 0;
    std::uint32_t networkValue = 0;
    if (!parseIpv4(address, addressValue) || !parseIpv4(cidr.substr(0, slash), networkValue)) {
        return false;
    }
    char* end = nullptr;
    const auto prefix = std::strtol(cidr.substr(slash + 1).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || prefix < 0 || prefix > 32) {
        return false;
    }
    const auto mask = prefix == 0
        ? 0U
        : static_cast<std::uint32_t>(0xFFFFFFFFU << (32 - prefix));
    return (addressValue & mask) == (networkValue & mask);
}

}  // namespace

std::size_t ModbusNorthboundServer::AddressKeyHash::operator()(const AddressKey& key) const {
    std::size_t value = static_cast<std::size_t>(key.unitId);
    value = value * 131U + static_cast<std::size_t>(key.function);
    value = value * 131U + static_cast<std::size_t>(key.address);
    return value;
}

bool ModbusNorthboundServer::AddressKeyEqual::operator()(
    const AddressKey& lhs,
    const AddressKey& rhs
) const {
    return lhs.unitId == rhs.unitId &&
           lhs.function == rhs.function &&
           lhs.address == rhs.address;
}

ModbusNorthboundServer::ModbusNorthboundServer(DeviceConfig config, MemoryPointStore& store)
    : config_(std::move(config)),
      store_(store) {
    buildMappings();
}

ModbusNorthboundServer::~ModbusNorthboundServer() {
    stop();
}

void ModbusNorthboundServer::start() {
    if (!config_.northboundServer.enabled) {
        return;
    }
    if (config_.northboundServer.protocol != "modbus_tcp") {
        throw std::invalid_argument("northboundServer currently supports protocol=modbus_tcp only");
    }
    if (config_.northboundServer.mode != "mapped" && config_.northboundServer.mode != "hybrid") {
        throw std::invalid_argument("northboundServer mode passthrough is reserved and must not be enabled in this build");
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    acceptThread_ = std::thread(&ModbusNorthboundServer::acceptLoop, this);
}

void ModbusNorthboundServer::stop() {
    running_.store(false);
    closeListenSocket();
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& thread : clientThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        clientThreads_.clear();
    }
}

bool ModbusNorthboundServer::isRunning() const {
    return running_.load();
}

int ModbusNorthboundServer::boundPort() const {
    return boundPort_.load();
}

void ModbusNorthboundServer::buildMappings() {
    const auto addPoints = [this](const std::vector<PointDefinition>& points) {
        for (const auto& point : points) {
            addPointMapping(point);
        }
    };
    addPoints(config_.points);
    for (const auto& meter : config_.meters) {
        addPoints(meter.points);
    }
}

void ModbusNorthboundServer::addPointMapping(const PointDefinition& point) {
    const auto& mapping = point.northbound;
    if (!point.enabled || !mapping.enabled) {
        return;
    }
    if (!isReadFunction(static_cast<std::uint8_t>(mapping.readFunction))) {
        throw std::invalid_argument("northbound readFunction must be 1, 2, 3, or 4");
    }
    if (mapping.address < 0) {
        throw std::invalid_argument("northbound mapping address must be configured for point " + point.pointCode);
    }
    const auto span = mapping.readFunction == 1 || mapping.readFunction == 2
        ? 1
        : static_cast<int>(dataTypeRegisterLength(mapping.dataType, mapping.length));
    for (int offset = 0; offset < span; ++offset) {
        AddressKey key{mapping.unitId, mapping.readFunction, mapping.address + offset};
        const auto inserted = addressMappings_.emplace(key, Mapping{point, mapping});
        if (!inserted.second) {
            throw std::invalid_argument(
                "duplicate northbound mapping unit/function/address: " +
                std::to_string(mapping.unitId) + "/" +
                std::to_string(mapping.readFunction) + "/" +
                std::to_string(mapping.address + offset)
            );
        }
    }
}

void ModbusNorthboundServer::acceptLoop() {
    try {
        ensureSocketRuntime();
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* result = nullptr;
        const auto port = std::to_string(config_.northboundServer.port);
        const char* host = config_.northboundServer.bindHost.empty()
            ? nullptr
            : config_.northboundServer.bindHost.c_str();
        if (getaddrinfo(host, port.c_str(), &hints, &result) != 0) {
            throw std::runtime_error("northbound getaddrinfo failed");
        }

        SocketHandle listenSocket = kInvalidSocket;
        for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
            listenSocket = static_cast<SocketHandle>(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
            if (listenSocket == kInvalidSocket) {
                continue;
            }
            int reuse = 1;
            setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            if (bind(listenSocket, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0 &&
                listen(listenSocket, std::max(1, config_.northboundServer.maxClients)) == 0) {
                break;
            }
            closeSocket(listenSocket);
            listenSocket = kInvalidSocket;
        }
        freeaddrinfo(result);

        if (listenSocket == kInvalidSocket) {
            throw std::runtime_error("northbound listen failed");
        }

        listenSocket_ = static_cast<std::intptr_t>(listenSocket);
        int effectivePort = config_.northboundServer.port;
        if (effectivePort == 0) {
            sockaddr_storage boundAddr;
            std::memset(&boundAddr, 0, sizeof(boundAddr));
            socklen_t boundLen = sizeof(boundAddr);
            if (getsockname(listenSocket, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) == 0) {
                if (boundAddr.ss_family == AF_INET) {
                    effectivePort = ntohs(reinterpret_cast<sockaddr_in*>(&boundAddr)->sin_port);
                } else if (boundAddr.ss_family == AF_INET6) {
                    effectivePort = ntohs(reinterpret_cast<sockaddr_in6*>(&boundAddr)->sin6_port);
                }
            }
        }
        boundPort_.store(effectivePort);
        std::cerr << "northbound modbus tcp started"
                  << " host=" << config_.northboundServer.bindHost
                  << " port=" << effectivePort
                  << " mappings=" << addressMappings_.size()
                  << std::endl;

        while (running_.load()) {
            sockaddr_storage clientAddr;
            std::memset(&clientAddr, 0, sizeof(clientAddr));
            socklen_t clientLen = sizeof(clientAddr);
            const auto client = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (client == kInvalidSocket) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }

            char hostBuffer[NI_MAXHOST] = {};
            if (getnameinfo(
                    reinterpret_cast<sockaddr*>(&clientAddr),
                    clientLen,
                    hostBuffer,
                    sizeof(hostBuffer),
                    nullptr,
                    0,
                    NI_NUMERICHOST) != 0) {
                std::strncpy(hostBuffer, "unknown", sizeof(hostBuffer) - 1);
            }
            const std::string clientAddress(hostBuffer);
            if (!isClientAllowed(clientAddress)) {
                closeSocket(client);
                continue;
            }
            if (activeClients_.load() >= config_.northboundServer.maxClients) {
                closeSocket(client);
                continue;
            }
            activeClients_.fetch_add(1);
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clientThreads_.emplace_back(
                &ModbusNorthboundServer::handleClient,
                this,
                static_cast<std::intptr_t>(client),
                clientAddress
            );
        }
    } catch (const std::exception& ex) {
        if (running_.load()) {
            std::cerr << "northbound modbus tcp stopped by error: " << ex.what() << std::endl;
            running_.store(false);
        }
    }
}

void ModbusNorthboundServer::handleClient(std::intptr_t clientSocket, std::string clientAddress) {
    (void)clientAddress;
    const auto socketHandle = static_cast<SocketHandle>(clientSocket);
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(config_.northboundServer.requestTimeoutMs);
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout;
    timeout.tv_sec = config_.northboundServer.requestTimeoutMs / 1000;
    timeout.tv_usec = (config_.northboundServer.requestTimeoutMs % 1000) * 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    try {
        while (running_.load()) {
            std::vector<std::uint8_t> header;
            if (!readExact(clientSocket, header, 7)) {
                break;
            }
            const auto transactionId = readWord(header, 0);
            const auto protocolId = readWord(header, 2);
            const auto length = readWord(header, 4);
            const auto unitId = header[6];
            if (protocolId != 0 || length < 2 || length > 260) {
                break;
            }
            std::vector<std::uint8_t> pdu;
            if (!readExact(clientSocket, pdu, static_cast<std::size_t>(length - 1))) {
                break;
            }

            auto responsePdu = handleRequest(unitId, pdu);
            std::vector<std::uint8_t> response;
            response.reserve(7 + responsePdu.size());
            appendWord(response, transactionId);
            appendWord(response, 0);
            appendWord(response, static_cast<std::uint16_t>(responsePdu.size() + 1));
            response.push_back(unitId);
            response.insert(response.end(), responsePdu.begin(), responsePdu.end());
            sendAll(clientSocket, response);
        }
    } catch (...) {
    }
    closeSocket(socketHandle);
    activeClients_.fetch_sub(1);
}

std::vector<std::uint8_t> ModbusNorthboundServer::handleRequest(
    std::uint8_t unitId,
    const std::vector<std::uint8_t>& pdu
) {
    if (pdu.size() < 1) {
        return makeException(0, kIllegalDataValue);
    }
    const auto function = pdu[0];
    if (function == 1 || function == 2 || function == 3 || function == 4) {
        if (pdu.size() != 5) {
            return makeException(function, kIllegalDataValue);
        }
        const auto start = static_cast<int>(readWord(pdu, 1));
        const auto count = static_cast<int>(readWord(pdu, 3));
        if (function == 1 || function == 2) {
            return handleBitRead(unitId, function, start, count);
        }
        return handleRegisterRead(unitId, function, start, count);
    }
    if (function == 5 || function == 6 || function == 15 || function == 16) {
        return makeException(function, config_.northboundServer.writesEnabled
            ? kIllegalDataAddress
            : kIllegalFunction);
    }
    return makeException(function, kIllegalFunction);
}

std::vector<std::uint8_t> ModbusNorthboundServer::handleRegisterRead(
    std::uint8_t unitId,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0 || count > config_.northboundServer.maxReadRegisters) {
        return makeException(function, kIllegalDataValue);
    }
    std::vector<std::uint16_t> registers(static_cast<std::size_t>(count), 0);
    std::vector<std::uint8_t> filled(static_cast<std::size_t>(count), 0);
    const auto now = currentTimeMs();

    for (int offset = 0; offset < count; ++offset) {
        AddressKey key{unitId, function, start + offset};
        const auto it = addressMappings_.find(key);
        if (it == addressMappings_.end()) {
            return makeException(function, kIllegalDataAddress);
        }
        std::vector<std::uint16_t> encoded;
        try {
            encoded = encodeRegistersForMapping(it->second, now);
        } catch (const std::exception& ex) {
            std::cerr << "northbound register read failed"
                      << " unit=" << static_cast<int>(unitId)
                      << " function=" << static_cast<int>(function)
                      << " address=" << (start + offset)
                      << " point=" << it->second.point.pointCode
                      << " error=" << ex.what()
                      << std::endl;
            return makeException(function, kServerDeviceFailure);
        }
        const auto mappingAddress = it->second.config.address;
        const auto relative = start + offset - mappingAddress;
        if (relative < 0 || static_cast<std::size_t>(relative) >= encoded.size()) {
            return makeException(function, kIllegalDataAddress);
        }
        registers[static_cast<std::size_t>(offset)] = encoded[static_cast<std::size_t>(relative)];
        filled[static_cast<std::size_t>(offset)] = 1;
    }

    if (std::find(filled.begin(), filled.end(), 0) != filled.end()) {
        return makeException(function, kIllegalDataAddress);
    }

    std::vector<std::uint8_t> response;
    response.reserve(2 + registers.size() * 2);
    response.push_back(function);
    response.push_back(static_cast<std::uint8_t>(registers.size() * 2));
    for (const auto value : registers) {
        appendWord(response, value);
    }
    return response;
}

std::vector<std::uint8_t> ModbusNorthboundServer::handleBitRead(
    std::uint8_t unitId,
    std::uint8_t function,
    int start,
    int count
) {
    if (count <= 0 || count > config_.northboundServer.maxReadBits) {
        return makeException(function, kIllegalDataValue);
    }
    std::vector<std::uint8_t> response;
    const auto byteCount = static_cast<std::size_t>((count + 7) / 8);
    response.assign(2 + byteCount, 0);
    response[0] = function;
    response[1] = static_cast<std::uint8_t>(byteCount);
    const auto now = currentTimeMs();
    for (int offset = 0; offset < count; ++offset) {
        AddressKey key{unitId, function, start + offset};
        const auto it = addressMappings_.find(key);
        if (it == addressMappings_.end()) {
            return makeException(function, kIllegalDataAddress);
        }
        std::uint16_t bit = 0;
        try {
            bit = encodeBitForMapping(it->second, now);
        } catch (const std::exception& ex) {
            std::cerr << "northbound bit read failed"
                      << " unit=" << static_cast<int>(unitId)
                      << " function=" << static_cast<int>(function)
                      << " address=" << (start + offset)
                      << " point=" << it->second.point.pointCode
                      << " error=" << ex.what()
                      << std::endl;
            return makeException(function, kServerDeviceFailure);
        }
        if (bit != 0) {
            response[2 + static_cast<std::size_t>(offset / 8)] |=
                static_cast<std::uint8_t>(1U << (offset % 8));
        }
    }
    return response;
}

std::vector<std::uint16_t> ModbusNorthboundServer::encodeRegistersForMapping(
    const Mapping& mapping,
    std::int64_t nowMs
) const {
    const auto latest = store_.getLatestByIndex(mapping.point.index, nowMs);
    if (!latest || latest->quality != 1 || latest->stale) {
        if (mapping.config.stalePolicy == "zero") {
            return zeroRegistersForMapping(mapping);
        }
        if (mapping.config.stalePolicy == "last_value" && latest) {
            return encodeNumericRegisters(latest->value, mapping.config);
        }
        throw std::runtime_error("northbound mapped point is stale or missing");
    }
    return encodeNumericRegisters(latest->value, mapping.config);
}

std::vector<std::uint16_t> ModbusNorthboundServer::zeroRegistersForMapping(const Mapping& mapping) const {
    const auto length = dataTypeRegisterLength(mapping.config.dataType, mapping.config.length);
    return std::vector<std::uint16_t>(length, 0);
}

std::uint16_t ModbusNorthboundServer::encodeBitForMapping(
    const Mapping& mapping,
    std::int64_t nowMs
) const {
    const auto latest = store_.getLatestByIndex(mapping.point.index, nowMs);
    if (!latest || latest->quality != 1 || latest->stale) {
        if (mapping.config.stalePolicy == "zero") {
            return 0;
        }
        if (mapping.config.stalePolicy == "last_value" && latest) {
            return std::abs(latest->value) > 0.0 ? 1 : 0;
        }
        throw std::runtime_error("northbound mapped bit is stale or missing");
    }
    return std::abs(latest->value) > 0.0 ? 1 : 0;
}

void ModbusNorthboundServer::sendAll(std::intptr_t socket, const std::vector<std::uint8_t>& bytes) const {
    const auto socketHandle = static_cast<SocketHandle>(socket);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(socketHandle, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(socketHandle, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (rc <= 0) {
            throw std::runtime_error("northbound send failed");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

bool ModbusNorthboundServer::readExact(
    std::intptr_t socket,
    std::vector<std::uint8_t>& bytes,
    std::size_t size
) const {
    bytes.assign(size, 0);
    const auto socketHandle = static_cast<SocketHandle>(socket);
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto rc = recv(socketHandle, reinterpret_cast<char*>(bytes.data() + received), static_cast<int>(size - received), 0);
#else
        const auto rc = recv(socketHandle, bytes.data() + received, size - received, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(rc);
    }
    return true;
}

bool ModbusNorthboundServer::isClientAllowed(const std::string& clientAddress) const {
    if (config_.northboundServer.allowedClientCidrs.empty()) {
        return true;
    }
    for (const auto& item : config_.northboundServer.allowedClientCidrs) {
        if (item == clientAddress) {
            return true;
        }
        if (ipv4MatchesCidr(clientAddress, item)) {
            return true;
        }
    }
    return false;
}

void ModbusNorthboundServer::closeListenSocket() {
    const auto socket = static_cast<SocketHandle>(listenSocket_);
    listenSocket_ = static_cast<std::intptr_t>(kInvalidSocket);
    if (socket != kInvalidSocket) {
#ifdef _WIN32
        shutdown(socket, SD_BOTH);
#else
        shutdown(socket, SHUT_RDWR);
#endif
    }
    closeSocket(socket);
}

}  // namespace edge_gateway
