#include "edge_gateway/modbus_northbound_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <thread>

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

bool isBitFunction(int function) {
    return function == 1 || function == 2;
}

// Registers occupied by one value of a register-area dataType; 0 for unsupported types.
int registerSpanOf(const std::string& dataType) {
    if (dataType.empty() || dataType == "uint16" || dataType == "int16" ||
        dataType == "bool" || dataType == "bit") {
        return 1;
    }
    if (dataType == "uint32" || dataType == "int32" || dataType == "float32") {
        return 2;
    }
    if (dataType == "uint64" || dataType == "int64" || dataType == "float64" || dataType == "double") {
        return 4;
    }
    return 0;
}

std::vector<std::uint8_t> bigEndianBytes(std::uint64_t value, std::size_t width) {
    std::vector<std::uint8_t> bytes(width);
    for (std::size_t i = 0; i < width; ++i) {
        bytes[width - 1 - i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return bytes;
}

// Empty when byteOrder is usable for a value of width bytes: either empty (big-endian) or a
// permutation of the first width letters (A = most significant canonical byte).
std::string byteOrderProblem(const std::string& byteOrder, std::size_t width) {
    if (byteOrder.empty()) {
        return {};
    }
    if (byteOrder.size() != width) {
        return "byteOrder \"" + byteOrder + "\" has " + std::to_string(byteOrder.size()) +
            " letters but the dataType is " + std::to_string(width) + " bytes";
    }
    std::vector<bool> seen(width, false);
    for (const auto token : byteOrder) {
        const auto position = static_cast<int>(token) - 'A';
        if (position < 0 || static_cast<std::size_t>(position) >= width) {
            return "byteOrder \"" + byteOrder + "\" uses a letter outside A.." +
                std::string(1, static_cast<char>('A' + width - 1));
        }
        if (seen[static_cast<std::size_t>(position)]) {
            return "byteOrder \"" + byteOrder + "\" repeats a letter";
        }
        seen[static_cast<std::size_t>(position)] = true;
    }
    return {};
}

std::vector<std::uint8_t> reorderBytesForEncode(
    const std::vector<std::uint8_t>& canonicalBytes,
    const std::string& byteOrder
) {
    if (byteOrder.empty()) {
        return canonicalBytes;
    }
    std::vector<std::uint8_t> reordered(byteOrder.size());
    for (std::size_t i = 0; i < byteOrder.size(); ++i) {
        reordered[static_cast<std::size_t>(byteOrder[i] - 'A')] = canonicalBytes[i];
    }
    return reordered;
}

std::vector<std::uint16_t> bytesToRegisters(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint16_t> registers;
    registers.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        registers.push_back(static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[i] << 8) | bytes[i + 1]
        ));
    }
    return registers;
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Rounds rawValue to T, refusing values T cannot represent instead of letting
// the cast wrap (e.g. -5 or 70000 into a uint16 register).
template <typename T>
bool checkedIntegral(double rawValue, T* out) {
    if (!std::isfinite(rawValue)) {
        return false;
    }
    const auto rounded = static_cast<long double>(std::round(rawValue));
    if (rounded < static_cast<long double>(std::numeric_limits<T>::min()) ||
        rounded > static_cast<long double>(std::numeric_limits<T>::max())) {
        return false;
    }
    *out = static_cast<T>(std::llround(rawValue));
    return true;
}

template <>
bool checkedIntegral<std::uint64_t>(double rawValue, std::uint64_t* out) {
    if (!std::isfinite(rawValue)) {
        return false;
    }
    const auto rounded = std::round(rawValue);
    if (rounded < 0.0 || rounded >= 18446744073709551616.0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(rounded);
    return true;
}

template <typename Signed, typename Unsigned>
bool signedBytes(double rawValue, std::vector<std::uint8_t>* canonical) {
    Signed value = 0;
    if (!checkedIntegral<Signed>(rawValue, &value)) {
        return false;
    }
    Unsigned raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    *canonical = bigEndianBytes(raw, sizeof(raw));
    return true;
}

template <typename Unsigned>
bool unsignedBytes(double rawValue, std::vector<std::uint8_t>* canonical) {
    Unsigned value = 0;
    if (!checkedIntegral<Unsigned>(rawValue, &value)) {
        return false;
    }
    *canonical = bigEndianBytes(value, sizeof(value));
    return true;
}

// Canonical (big-endian) bytes of a raw (unscaled) value; false when dataType cannot represent it.
bool canonicalBytesFor(double rawValue, const std::string& dataType, std::vector<std::uint8_t>* canonical) {
    if (dataType == "bit" || dataType == "bool") {
        if (!std::isfinite(rawValue)) {
            return false;
        }
        *canonical = bigEndianBytes(std::abs(rawValue) > 0.0 ? 1U : 0U, 2);
        return true;
    }
    if (dataType.empty() || dataType == "uint16") {
        return unsignedBytes<std::uint16_t>(rawValue, canonical);
    }
    if (dataType == "int16") {
        return signedBytes<std::int16_t, std::uint16_t>(rawValue, canonical);
    }
    if (dataType == "uint32") {
        return unsignedBytes<std::uint32_t>(rawValue, canonical);
    }
    if (dataType == "int32") {
        return signedBytes<std::int32_t, std::uint32_t>(rawValue, canonical);
    }
    if (dataType == "uint64") {
        return unsignedBytes<std::uint64_t>(rawValue, canonical);
    }
    if (dataType == "int64") {
        return signedBytes<std::int64_t, std::uint64_t>(rawValue, canonical);
    }
    if (dataType == "float32") {
        const auto narrowed = static_cast<float>(rawValue);
        if (!std::isfinite(rawValue) || !std::isfinite(narrowed)) {
            return false;
        }
        std::uint32_t raw = 0;
        std::memcpy(&raw, &narrowed, sizeof(raw));
        *canonical = bigEndianBytes(raw, 4);
        return true;
    }
    if (dataType == "float64" || dataType == "double") {
        if (!std::isfinite(rawValue)) {
            return false;
        }
        std::uint64_t raw = 0;
        std::memcpy(&raw, &rawValue, sizeof(raw));
        *canonical = bigEndianBytes(raw, 8);
        return true;
    }
    return false;
}

// The dataType's "not available" marker (SunSpec convention): all ones for unsigned, the minimum
// for signed, quiet NaN for floats.
std::vector<std::uint8_t> defaultSentinelBytes(const std::string& dataType) {
    if (dataType == "int16") return bigEndianBytes(0x8000U, 2);
    if (dataType == "uint32") return bigEndianBytes(0xFFFFFFFFULL, 4);
    if (dataType == "int32") return bigEndianBytes(0x80000000ULL, 4);
    if (dataType == "uint64") return bigEndianBytes(0xFFFFFFFFFFFFFFFFULL, 8);
    if (dataType == "int64") return bigEndianBytes(0x8000000000000000ULL, 8);
    if (dataType == "float32") return bigEndianBytes(0x7FC00000ULL, 4);
    if (dataType == "float64" || dataType == "double") return bigEndianBytes(0x7FF8000000000000ULL, 8);
    return bigEndianBytes(0xFFFFU, 2);
}

std::vector<std::uint8_t> makeException(std::uint8_t function, std::uint8_t code) {
    return {
        static_cast<std::uint8_t>(function | 0x80U),
        code
    };
}

bool parseIpv4(const std::string& value, std::uint32_t& out) {
    std::istringstream stream(value);
    std::string part;
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::getline(stream, part, '.')) {
            return false;
        }
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const auto octet = std::strtol(part.c_str(), nullptr, 10);
        if (octet < 0 || octet > 255) {
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

bool parseCidrPrefix(const std::string& text, long* prefix) {
    if (text.empty() || text.size() > 2 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    *prefix = std::strtol(text.c_str(), nullptr, 10);
    return *prefix >= 0 && *prefix <= 32;
}

bool ipv4MatchesCidr(const std::string& address, const std::string& cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::uint32_t addressValue = 0;
    std::uint32_t networkValue = 0;
    long prefix = 0;
    if (!parseIpv4(address, addressValue) || !parseIpv4(cidr.substr(0, slash), networkValue) ||
        !parseCidrPrefix(cidr.substr(slash + 1), &prefix)) {
        return false;
    }
    const auto mask = prefix == 0
        ? 0U
        : static_cast<std::uint32_t>(0xFFFFFFFFU << (32 - prefix));
    return (addressValue & mask) == (networkValue & mask);
}

bool isIpv6Literal(const std::string& value) {
    in6_addr address;
    return inet_pton(AF_INET6, value.c_str(), &address) == 1;
}

// An accepted IPv4 client of a dual-stack (::) listener is reported as ::ffff:a.b.c.d.
std::string normalizeClientAddress(const std::string& address) {
    static const std::string mappedPrefix = "::ffff:";
    if (address.size() > mappedPrefix.size() && address.compare(0, mappedPrefix.size(), mappedPrefix) == 0) {
        std::uint32_t ignored = 0;
        const auto tail = address.substr(mappedPrefix.size());
        if (parseIpv4(tail, ignored)) {
            return tail;
        }
    }
    return address;
}

bool isWildcardBindHost(const std::string& host) {
    return host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]" || host == "*";
}

std::string mappingLabel(const PointDefinition& point) {
    return "point " + point.pointCode + " (index " + std::to_string(point.index) + ")";
}

}  // namespace

std::vector<std::string> northboundConfigProblems(const DeviceConfig& config) {
    std::vector<std::string> problems;
    const auto& server = config.northboundServer;
    if (!server.enabled) {
        return problems;
    }
    if (server.protocol != "modbus_tcp") {
        problems.push_back("northboundServer.protocol \"" + server.protocol +
            "\" is not supported (only modbus_tcp)");
    }
    if (server.mode != "mapped" && server.mode != "hybrid") {
        problems.push_back("northboundServer.mode \"" + server.mode +
            "\" is reserved (use mapped)");
    }
    for (const auto& item : server.allowedClientCidrs) {
        std::uint32_t ignored = 0;
        const auto slash = item.find('/');
        long prefix = 0;
        const bool ipv4Cidr = slash != std::string::npos &&
            parseIpv4(item.substr(0, slash), ignored) && parseCidrPrefix(item.substr(slash + 1), &prefix);
        if (!ipv4Cidr && !parseIpv4(item, ignored) && !isIpv6Literal(item)) {
            problems.push_back("northboundServer.allowedClientCidrs entry \"" + item +
                "\" is not an IPv4 address, IPv4 CIDR or IPv6 address");
        }
    }
    if (!server.allowUnrestrictedClients && server.allowedClientCidrs.empty() &&
        isWildcardBindHost(server.bindHost)) {
        problems.push_back(
            "bindHost=" + (server.bindHost.empty() ? std::string("<any>") : server.bindHost) +
            " listens on all interfaces with an empty allowedClientCidrs list. Remedy: set"
            " northboundServer.allowedClientCidrs (e.g. [\"192.168.1.0/24\"]), or a specific"
            " northboundServer.bindHost, or explicitly northboundServer.allowUnrestrictedClients=true");
    }

    std::map<std::tuple<int, int, int>, std::string> occupied;
    const auto checkPoint = [&](const PointDefinition& point) {
        const auto& mapping = point.northbound;
        if (!point.enabled || !mapping.enabled) {
            return;
        }
        const auto label = mappingLabel(point) + " northbound";
        const auto before = problems.size();
        if (mapping.readFunction < 1 || mapping.readFunction > 4) {
            problems.push_back(label + ".readFunction must be 1, 2, 3 or 4");
            return;
        }
        if (mapping.unitId < 0 || mapping.unitId > 255) {
            problems.push_back(label + ".unitId must be 0..255");
        }
        if (mapping.onInvalid != "sentinel" && mapping.onInvalid != "hold") {
            problems.push_back(label + ".onInvalid \"" + mapping.onInvalid + "\" must be sentinel or hold");
        }
        int span = 1;
        if (!isBitFunction(mapping.readFunction)) {
            span = registerSpanOf(mapping.dataType);
            if (span == 0) {
                problems.push_back(label + ".dataType \"" + mapping.dataType +
                    "\" is not a register type (uint16/int16/uint32/int32/uint64/int64/float32/float64/bool)");
                return;
            }
            if (mapping.length != span) {
                problems.push_back(label + ".length " + std::to_string(mapping.length) + " does not match dataType " +
                    (mapping.dataType.empty() ? std::string("uint16") : mapping.dataType) + " (" +
                    std::to_string(span) + " register" + (span == 1 ? "" : "s") + ")");
            }
            const auto orderProblem = byteOrderProblem(mapping.byteOrder, static_cast<std::size_t>(span) * 2);
            if (!orderProblem.empty()) {
                problems.push_back(label + "." + orderProblem);
            }
            if (!std::isfinite(mapping.scale) || std::abs(mapping.scale) < std::numeric_limits<double>::epsilon() ||
                !std::isfinite(mapping.offset)) {
                problems.push_back(label + ".scale must be finite and non-zero, offset finite");
            }
            std::vector<std::uint8_t> ignored;
            if (mapping.sentinel && !canonicalBytesFor(*mapping.sentinel, mapping.dataType, &ignored)) {
                problems.push_back(label + ".sentinel is not representable as " + mapping.dataType);
            }
        }
        if (mapping.address < 0 || mapping.address + span - 1 > 65535) {
            problems.push_back(label + ".address must be configured within 0.." + std::to_string(65536 - span));
        }
        if (problems.size() != before) {
            return;
        }
        for (int offset = 0; offset < span; ++offset) {
            const auto key = std::make_tuple(mapping.unitId, mapping.readFunction, mapping.address + offset);
            const auto inserted = occupied.emplace(key, mappingLabel(point));
            if (!inserted.second) {
                problems.push_back(label + " overlaps " + inserted.first->second + " at unit/function/address " +
                    std::to_string(mapping.unitId) + "/" + std::to_string(mapping.readFunction) + "/" +
                    std::to_string(mapping.address + offset));
                return;
            }
        }
    };
    for (const auto& point : config.points) {
        checkPoint(point);
    }
    for (const auto& meter : config.meters) {
        for (const auto& point : meter.points) {
            checkPoint(point);
        }
    }
    return problems;
}

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

ModbusNorthboundServer::ModbusNorthboundServer(
    DeviceConfig config,
    MemoryPointStore& store,
    StatusSink statusSink
) : config_(std::move(config)),
    store_(store),
    statusSink_(std::move(statusSink)),
    configProblems_(northboundConfigProblems(config_)) {
    if (configProblems_.empty()) {
        buildMappings();
    }
}

ModbusNorthboundServer::~ModbusNorthboundServer() {
    stop();
}

void ModbusNorthboundServer::start() {
    if (!config_.northboundServer.enabled) {
        return;
    }
    if (!configProblems_.empty()) {
        // A bad northbound definition disables only this server; collection keeps running.
        std::string message = "northbound modbus tcp REFUSED to start:";
        for (const auto& problem : configProblems_) {
            message += " " + problem + ";";
        }
        setStartError(message);
        std::cerr << "ERROR " << message << std::endl;
        report("northbound-refused", message);
        return;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    try {
        acceptThread_ = std::thread(&ModbusNorthboundServer::acceptLoop, this);
    } catch (const std::exception& ex) {
        running_.store(false);
        setStartError(std::string("northbound listener thread start failed: ") + ex.what());
        report("northbound-refused", startError());
    }
}

void ModbusNorthboundServer::stop() {
    running_.store(false);
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& client : clientThreads_) {
        if (client.thread.joinable()) {
            client.thread.join();
        }
    }
    clientThreads_.clear();
}

void ModbusNorthboundServer::reapFinishedClientsLocked() {
    for (auto it = clientThreads_.begin(); it != clientThreads_.end();) {
        if (it->done && it->done->load()) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = clientThreads_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string ModbusNorthboundServer::startError() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return startError_;
}

void ModbusNorthboundServer::setStartError(const std::string& message) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    startError_ = message;
}

void ModbusNorthboundServer::report(const std::string& event, const std::string& message) const {
    if (!statusSink_) {
        return;
    }
    try {
        statusSink_(event, message);
    } catch (...) {
    }
}

bool ModbusNorthboundServer::isRunning() const {
    return running_.load();
}

int ModbusNorthboundServer::boundPort() const {
    return boundPort_.load();
}

void ModbusNorthboundServer::buildMappings() {
    for (const auto& point : config_.points) {
        addPointMapping(point);
    }
    for (const auto& meter : config_.meters) {
        for (const auto& point : meter.points) {
            addPointMapping(point);
        }
    }
    lastGood_.resize(mappings_.size());
}

void ModbusNorthboundServer::addPointMapping(const PointDefinition& point) {
    const auto& config = point.northbound;
    if (!point.enabled || !config.enabled) {
        return;
    }
    // Validated by northboundConfigProblems(): known dataType, matching length/byteOrder, no overlap.
    Mapping mapping;
    mapping.point = point;
    mapping.config = config;
    if (isBitFunction(config.readFunction)) {
        mapping.span = 1;
        mapping.invalidRegisters = {
            static_cast<std::uint16_t>(config.sentinel && *config.sentinel != 0.0 ? 1 : 0)
        };
    } else {
        mapping.span = registerSpanOf(config.dataType);
        std::vector<std::uint8_t> canonical;
        if (!config.sentinel || !canonicalBytesFor(*config.sentinel, config.dataType, &canonical)) {
            canonical = defaultSentinelBytes(config.dataType);
        }
        mapping.invalidRegisters = bytesToRegisters(reorderBytesForEncode(canonical, config.byteOrder));
    }
    const auto slot = mappings_.size();
    mappings_.push_back(std::move(mapping));
    for (int offset = 0; offset < mappings_.back().span; ++offset) {
        addressMappings_.emplace(AddressKey{config.unitId, config.readFunction, config.address + offset}, slot);
    }
    if (std::find(unitIds_.begin(), unitIds_.end(), config.unitId) == unitIds_.end()) {
        unitIds_.push_back(config.unitId);
    }
}

void ModbusNorthboundServer::acceptLoop() {
    bool failureReported = false;
    const auto retryMs = std::max(100, config_.northboundServer.bindRetryIntervalMs);
    while (running_.load()) {
        std::string error;
        SocketHandle listenSocket = kInvalidSocket;
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
            const auto& bindHost = config_.northboundServer.bindHost;
            const char* host = bindHost.empty() || bindHost == "*" ? nullptr
                : (bindHost == "[::]" ? "::" : bindHost.c_str());
            const auto rc = getaddrinfo(host, port.c_str(), &hints, &result);
            if (rc != 0) {
                error = std::string("getaddrinfo failed: ") + gai_strerror(rc);
            }
            for (auto* addr = result; addr != nullptr && listenSocket == kInvalidSocket; addr = addr->ai_next) {
                listenSocket = static_cast<SocketHandle>(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
                if (listenSocket == kInvalidSocket) {
                    error = std::string("socket failed: ") + std::strerror(errno);
                    continue;
                }
                int reuse = 1;
                setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
                if (addr->ai_family == AF_INET6) {
                    // "::" serves IPv4 clients too (as ::ffff:a.b.c.d), matching the IPv4 allowlist.
                    int v6only = 0;
                    setsockopt(listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
                }
                if (bind(listenSocket, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) != 0 ||
                    listen(listenSocket, std::max(1, config_.northboundServer.maxClients)) != 0) {
                    error = std::string("bind/listen failed: ") + std::strerror(errno);
                    closeSocket(listenSocket);
                    listenSocket = kInvalidSocket;
                }
            }
            if (result != nullptr) {
                freeaddrinfo(result);
            }
        } catch (const std::exception& ex) {
            error = ex.what();
        }

        if (listenSocket == kInvalidSocket) {
            const auto message = "northbound modbus tcp cannot listen on " +
                (config_.northboundServer.bindHost.empty() ? std::string("<any>") : config_.northboundServer.bindHost) +
                ":" + std::to_string(config_.northboundServer.port) + " (" + error + "); retrying every " +
                std::to_string(retryMs) + " ms";
            setStartError(message);
            if (!failureReported) {
                failureReported = true;
                std::cerr << "ERROR " << message << std::endl;
                report("northbound-bind-failed", message);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(retryMs);
            while (running_.load() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        int effectivePort = config_.northboundServer.port;
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
        setStartError(std::string());
        boundPort_.store(effectivePort);
        const auto started = "northbound modbus tcp listening host=" + config_.northboundServer.bindHost +
            " port=" + std::to_string(effectivePort) + " mappings=" + std::to_string(mappings_.size());
        std::cerr << started << std::endl;
        if (failureReported) {
            failureReported = false;
            report("northbound-listening", started);
        }
        serveListener(static_cast<std::intptr_t>(listenSocket));
        closeSocket(listenSocket);
    }
}

void ModbusNorthboundServer::serveListener(std::intptr_t listenHandle) {
    const auto listenSocket = static_cast<SocketHandle>(listenHandle);
    while (running_.load()) {
        // Wait in short slices so stop() is honoured promptly; the listener is owned by this thread.
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 200000;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
#ifdef _WIN32
        const auto ready = select(0, &readSet, nullptr, nullptr, &wait);
#else
        const auto ready = select(listenSocket + 1, &readSet, nullptr, nullptr, &wait);
#endif
        if (ready <= 0) {
            continue;
        }
        sockaddr_storage clientAddr;
        std::memset(&clientAddr, 0, sizeof(clientAddr));
        socklen_t clientLen = sizeof(clientAddr);
        const auto client = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client == kInvalidSocket) {
            // e.g. EMFILE: back off instead of spinning on the listener.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
        if (!northboundClientAllowed(config_.northboundServer, clientAddress)) {
            closeSocket(client);
            continue;
        }
        if (activeClients_.load() >= config_.northboundServer.maxClients) {
            closeSocket(client);
            continue;
        }
        activeClients_.fetch_add(1);
        std::lock_guard<std::mutex> lock(clientsMutex_);
        // Join handlers of closed connections so reconnecting masters do
        // not accumulate unjoined threads (and their stacks) forever.
        reapFinishedClientsLocked();
        try {
            auto done = std::make_shared<std::atomic<bool>>(false);
            ClientThread entry;
            entry.done = done;
            entry.thread = std::thread(
                &ModbusNorthboundServer::handleClient,
                this,
                static_cast<std::intptr_t>(client),
                clientAddress,
                done
            );
            clientThreads_.push_back(std::move(entry));
        } catch (const std::exception& ex) {
            std::cerr << "northbound client thread start failed: " << ex.what() << std::endl;
            closeSocket(client);
            activeClients_.fetch_sub(1);
        }
    }
}

void ModbusNorthboundServer::handleClient(
    std::intptr_t clientSocket,
    std::string clientAddress,
    std::shared_ptr<std::atomic<bool>> done
) {
    (void)clientAddress;
    const auto socketHandle = static_cast<SocketHandle>(clientSocket);
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(config_.northboundServer.requestTimeoutMs);
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout;
    timeout.tv_sec = config_.northboundServer.requestTimeoutMs / 1000;
    timeout.tv_usec = (config_.northboundServer.requestTimeoutMs % 1000) * 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    try {
        while (running_.load()) {
            // Idle phase: a master may stay silent up to idleTimeoutMs between
            // requests. Once the first byte arrives the whole ADU must complete
            // within requestTimeoutMs, so a byte-dribbling client cannot hold a
            // connection slot indefinitely.
            std::vector<std::uint8_t> first;
            const auto idleDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max(1, config_.northboundServer.idleTimeoutMs));
            if (!readExact(clientSocket, first, 1, idleDeadline)) {
                break;
            }
            const auto requestDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max(1, config_.northboundServer.requestTimeoutMs));
            std::vector<std::uint8_t> rest;
            if (!readExact(clientSocket, rest, 6, requestDeadline)) {
                break;
            }
            std::vector<std::uint8_t> header;
            header.reserve(7);
            header.push_back(first.front());
            header.insert(header.end(), rest.begin(), rest.end());
            const auto transactionId = readWord(header, 0);
            const auto protocolId = readWord(header, 2);
            const auto length = readWord(header, 4);
            const auto unitId = header[6];
            if (protocolId != 0 || length < 2 || length > 260) {
                break;
            }
            std::vector<std::uint8_t> pdu;
            if (!readExact(clientSocket, pdu, static_cast<std::size_t>(length - 1), requestDeadline)) {
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
    if (done) {
        done->store(true);
    }
}

// Modbus TCP convention: unit 255 (and 0) address "this server". They alias the only configured
// unit; with several units configured they must be mapped explicitly.
int ModbusNorthboundServer::resolveUnit(std::uint8_t unitId) const {
    const int unit = static_cast<int>(unitId);
    if (std::find(unitIds_.begin(), unitIds_.end(), unit) != unitIds_.end()) {
        return unit;
    }
    if ((unit == 0 || unit == 255) && unitIds_.size() == 1) {
        return unitIds_.front();
    }
    return -1;
}

std::vector<std::uint8_t> ModbusNorthboundServer::handleRequest(
    std::uint8_t unitId,
    const std::vector<std::uint8_t>& pdu
) {
    if (pdu.empty()) {
        return makeException(0, kIllegalDataValue);
    }
    const auto function = pdu[0];
    if (function == 1 || function == 2 || function == 3 || function == 4) {
        if (pdu.size() != 5) {
            return makeException(function, kIllegalDataValue);
        }
        const auto start = static_cast<int>(readWord(pdu, 1));
        const auto count = static_cast<int>(readWord(pdu, 3));
        const auto unit = resolveUnit(unitId);
        if (isBitFunction(function)) {
            if (count <= 0 || count > config_.northboundServer.maxReadBits) {
                return makeException(function, kIllegalDataValue);
            }
            return unit < 0 ? makeException(function, kIllegalDataAddress)
                            : handleBitRead(unit, function, start, count);
        }
        if (count <= 0 || count > config_.northboundServer.maxReadRegisters) {
            return makeException(function, kIllegalDataValue);
        }
        return unit < 0 ? makeException(function, kIllegalDataAddress)
                        : handleRegisterRead(unit, function, start, count);
    }
    if (function == 5 || function == 6 || function == 15 || function == 16) {
        return makeException(function, config_.northboundServer.writesEnabled
            ? kIllegalDataAddress
            : kIllegalFunction);
    }
    return makeException(function, kIllegalFunction);
}

std::vector<std::uint8_t> ModbusNorthboundServer::handleRegisterRead(
    int unitId,
    std::uint8_t function,
    int start,
    int count
) {
    std::vector<std::uint16_t> registers;
    registers.reserve(static_cast<std::size_t>(count));
    const auto now = currentTimeMs();
    // Walk value by value. A request must cover whole values: starting or ending inside a
    // multi-register value would return half of one snapshot and half of another (02).
    int offset = 0;
    while (offset < count) {
        const auto it = addressMappings_.find(AddressKey{unitId, function, start + offset});
        if (it == addressMappings_.end()) {
            return makeException(function, kIllegalDataAddress);
        }
        const auto& mapping = mappings_[it->second];
        if (mapping.config.address != start + offset || offset + mapping.span > count) {
            return makeException(function, kIllegalDataAddress);
        }
        const auto encoded = readMapping(it->second, now);
        registers.insert(registers.end(), encoded.begin(), encoded.end());
        offset += mapping.span;
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
    int unitId,
    std::uint8_t function,
    int start,
    int count
) {
    std::vector<std::uint8_t> response;
    const auto byteCount = static_cast<std::size_t>((count + 7) / 8);
    response.assign(2 + byteCount, 0);
    response[0] = function;
    response[1] = static_cast<std::uint8_t>(byteCount);
    const auto now = currentTimeMs();
    for (int offset = 0; offset < count; ++offset) {
        const auto it = addressMappings_.find(AddressKey{unitId, function, start + offset});
        if (it == addressMappings_.end()) {
            return makeException(function, kIllegalDataAddress);
        }
        if (readMapping(it->second, now).front() != 0) {
            response[2 + static_cast<std::size_t>(offset / 8)] |=
                static_cast<std::uint8_t>(1U << (offset % 8));
        }
    }
    return response;
}

std::vector<std::uint16_t> ModbusNorthboundServer::readMapping(std::size_t slot, std::int64_t nowMs) {
    const auto& mapping = mappings_[slot];
    const bool hold = mapping.config.onInvalid == "hold";
    std::vector<std::uint16_t> encoded;
    bool valid = false;
    const auto latest = store_.getLatestByIndex(mapping.point.index, nowMs);
    if (latest && latest->quality == 1 && !latest->stale && std::isfinite(latest->value)) {
        if (isBitFunction(mapping.config.readFunction)) {
            encoded.assign(1, static_cast<std::uint16_t>(latest->value != 0.0 ? 1 : 0));
            valid = true;
        } else {
            const auto raw = (latest->value - mapping.config.offset) / mapping.config.scale;
            std::vector<std::uint8_t> canonical;
            if (canonicalBytesFor(raw, mapping.config.dataType, &canonical)) {
                encoded = bytesToRegisters(reorderBytesForEncode(canonical, mapping.config.byteOrder));
                valid = true;
            }
        }
    }
    if (hold) {
        std::lock_guard<std::mutex> lock(holdMutex_);
        if (valid) {
            lastGood_[slot] = encoded;
            return encoded;
        }
        return lastGood_[slot].empty() ? mapping.invalidRegisters : lastGood_[slot];
    }
    return valid ? encoded : mapping.invalidRegisters;
}

void ModbusNorthboundServer::sendAll(std::intptr_t socket, const std::vector<std::uint8_t>& bytes) const {
    const auto socketHandle = static_cast<SocketHandle>(socket);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto rc = send(socketHandle, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
#else
        const auto rc = send(socketHandle, bytes.data() + sent, bytes.size() - sent, kSendFlags);
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
    std::size_t size,
    std::chrono::steady_clock::time_point deadline
) const {
    bytes.assign(size, 0);
    const auto socketHandle = static_cast<SocketHandle>(socket);
    std::size_t received = 0;
    while (received < size) {
        if (!running_.load()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        // Wait in short slices so stop() is honoured promptly.
        const auto remainingUs = std::min<long long>(
            200000,
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count());
        timeval wait{};
        wait.tv_sec = static_cast<long>(remainingUs / 1000000);
        wait.tv_usec = static_cast<long>(remainingUs % 1000000);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketHandle, &readSet);
#ifdef _WIN32
        const auto ready = select(0, &readSet, nullptr, nullptr, &wait);
#else
        const auto ready = select(socketHandle + 1, &readSet, nullptr, nullptr, &wait);
#endif
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            return false;
        }
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

// Empty allowedClientCidrs admits every client: northboundConfigProblems() already refuses that
// combination on a wildcard bindHost unless allowUnrestrictedClients is true.
bool northboundClientAllowed(const NorthboundServerConfig& server, const std::string& clientAddress) {
    if (server.allowedClientCidrs.empty()) {
        return true;
    }
    const auto address = normalizeClientAddress(clientAddress);
    for (const auto& item : server.allowedClientCidrs) {
        if (item == address || item == clientAddress || ipv4MatchesCidr(address, item)) {
            return true;
        }
    }
    return false;
}

}  // namespace edge_gateway
