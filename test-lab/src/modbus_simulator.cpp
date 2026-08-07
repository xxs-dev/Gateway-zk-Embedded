#include "gateway_test_lab/modbus_simulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
namespace test_lab_fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace test_lab_fs = std::filesystem;
#endif
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace gateway_test_lab {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int parseInt(const std::string& text, const std::string& field) {
    std::size_t used = 0;
    long value = 0;
    try {
        value = std::stol(trim(text), &used, 10);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid integer for " + field + ": " + text);
    }
    if (used != trim(text).size() || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        throw std::runtime_error("invalid integer for " + field + ": " + text);
    }
    return static_cast<int>(value);
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> values;
    std::istringstream input(text);
    std::string value;
    while (std::getline(input, value, delimiter)) {
        values.push_back(trim(value));
    }
    return values;
}

void initializeSockets() {
#ifdef _WIN32
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    });
#endif
}

void closeSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void shutdownSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

bool retryableSocketError() {
#ifdef _WIN32
    const auto error = WSAGetLastError();
    return error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes.at(offset)) << 8) |
        static_cast<std::uint16_t>(bytes.at(offset + 1))
    );
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::string normalizeArea(std::string area) {
    area = lower(trim(std::move(area)));
    if (area == "holding" || area == "input" || area == "coil" || area == "discrete") {
        return area;
    }
    throw std::runtime_error("unsupported Modbus area: " + area);
}

bool isSupportedScenario(const std::string& scenario) {
    return scenario == "normal" || scenario == "ramp" || scenario == "random" ||
           scenario == "boundary";
}

}  // namespace

bool ValueKey::operator<(const ValueKey& other) const {
    return std::tie(slave, area, address) < std::tie(other.slave, other.area, other.address);
}

std::string faultModeName(FaultMode mode) {
    switch (mode) {
        case FaultMode::None: return "none";
        case FaultMode::Timeout: return "timeout";
        case FaultMode::Disconnect: return "disconnect";
        case FaultMode::Exception: return "exception";
        case FaultMode::WriteTimeout: return "write-timeout";
        case FaultMode::WriteVerifyFailed: return "write-verify-failed";
    }
    return "none";
}

FaultMode parseFaultMode(const std::string& text) {
    const auto mode = lower(trim(text));
    if (mode.empty() || mode == "none" || mode == "normal") return FaultMode::None;
    if (mode == "timeout") return FaultMode::Timeout;
    if (mode == "disconnect") return FaultMode::Disconnect;
    if (mode == "exception") return FaultMode::Exception;
    if (mode == "write-timeout") return FaultMode::WriteTimeout;
    if (mode == "write-verify-failed") return FaultMode::WriteVerifyFailed;
    throw std::runtime_error("unsupported fault mode: " + text);
}

ControlState StateFile::load(const std::string& path) {
    ControlState state;
    if (path.empty()) {
        return state;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open test-lab state file: " + path);
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("invalid state line " + std::to_string(lineNumber));
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == "version") {
            if (value != "1") {
                throw std::runtime_error("unsupported test-lab state version: " + value);
            }
        } else if (key == "scenario") {
            const auto scenario = lower(value);
            if (!isSupportedScenario(scenario)) {
                throw std::runtime_error("unsupported scenario: " + value);
            }
            state.scenario = scenario;
        } else if (key == "slaves") {
            state.slaves.clear();
            for (const auto& item : split(value, ',')) {
                if (item.empty()) continue;
                const auto slave = parseInt(item, "slaves");
                if (slave < 1 || slave > 247) {
                    throw std::runtime_error("slave must be between 1 and 247");
                }
                state.slaves.push_back(slave);
            }
            std::sort(state.slaves.begin(), state.slaves.end());
            state.slaves.erase(std::unique(state.slaves.begin(), state.slaves.end()), state.slaves.end());
            if (state.slaves.empty()) {
                throw std::runtime_error("state file must contain at least one slave");
            }
        } else if (key.rfind("fault.", 0) == 0) {
            const auto slave = parseInt(key.substr(6), "fault slave");
            state.faults[slave] = parseFaultMode(value);
        } else if (key.rfind("value.", 0) == 0) {
            const auto parts = split(key, '.');
            if (parts.size() != 4) {
                throw std::runtime_error("invalid value key: " + key);
            }
            ValueKey valueKey;
            valueKey.slave = parseInt(parts[1], "value slave");
            valueKey.area = normalizeArea(parts[2]);
            valueKey.address = parseInt(parts[3], "value address");
            const auto raw = parseInt(value, "value");
            if (valueKey.address < 0 || valueKey.address > 65535 || raw < 0 || raw > 65535) {
                throw std::runtime_error("Modbus address and value must be between 0 and 65535");
            }
            state.values[valueKey] = static_cast<std::uint16_t>(raw);
        }
    }
    return state;
}

void StateFile::save(const std::string& path, const ControlState& state) {
    if (path.empty()) {
        throw std::runtime_error("state file path cannot be empty");
    }
    const test_lab_fs::path target(path);
    if (!target.parent_path().empty()) {
        test_lab_fs::create_directories(target.parent_path());
    }
    const auto temporary = target.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("failed to write test-lab state file: " + temporary);
        }
        output << "version=1\n";
        output << "scenario=" << state.scenario << "\n";
        output << "slaves=";
        for (std::size_t i = 0; i < state.slaves.size(); ++i) {
            if (i > 0) output << ',';
            output << state.slaves[i];
        }
        output << "\n";
        for (const auto& fault : state.faults) {
            if (fault.second != FaultMode::None) {
                output << "fault." << fault.first << '=' << faultModeName(fault.second) << "\n";
            }
        }
        for (const auto& item : state.values) {
            output << "value." << item.first.slave << '.' << item.first.area << '.'
                   << item.first.address << '=' << item.second << "\n";
        }
        if (!output.good()) {
            throw std::runtime_error("failed to flush test-lab state file: " + temporary);
        }
    }
    std::error_code error;
    test_lab_fs::remove(target, error);
    error.clear();
    test_lab_fs::rename(temporary, target, error);
    if (error) {
        throw std::runtime_error("failed to replace test-lab state file: " + error.message());
    }
}

class ModbusTcpSimulator::Impl {
public:
    explicit Impl(SimulatorOptions options)
        : options_(std::move(options)), startedAt_(Clock::now()) {}

    ~Impl() {
        stop();
    }

    void start() {
        if (running_.exchange(true)) {
            return;
        }
        initializeSockets();
        try {
            loadState(true);
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == kInvalidSocket) {
                throw std::runtime_error("failed to create Modbus TCP simulator socket");
            }
            int reuse = 1;
#ifdef _WIN32
            setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
            setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(options_.port);
            if (inet_pton(AF_INET, options_.bindAddress.c_str(), &address.sin_addr) != 1) {
                throw std::runtime_error("invalid simulator bind address: " + options_.bindAddress);
            }
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                throw std::runtime_error("failed to bind Modbus TCP simulator");
            }
            if (listen(listener_, 16) != 0) {
                throw std::runtime_error("failed to listen for Modbus TCP simulator clients");
            }
            socklen_t length = sizeof(address);
            if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                throw std::runtime_error("failed to resolve simulator listen port");
            }
            actualPort_ = ntohs(address.sin_port);
            stateThread_ = std::thread([this] { stateLoop(); });
            acceptThread_ = std::thread([this] { acceptLoop(); });
        } catch (...) {
            running_.store(false);
            closeSocket(listener_);
            listener_ = kInvalidSocket;
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        shutdownSocket(listener_);
        closeSocket(listener_);
        listener_ = kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (const auto client : clients_) {
                shutdownSocket(client);
            }
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        if (stateThread_.joinable()) {
            stateThread_.join();
        }
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            threads.swap(clientThreads_);
        }
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (const auto client : clients_) closeSocket(client);
            clients_.clear();
        }
    }

    bool running() const { return running_.load(); }
    std::uint16_t port() const { return actualPort_; }

    SimulatorStats stats() const {
        SimulatorStats result;
        result.acceptedConnections = acceptedConnections_.load();
        result.requests = requests_.load();
        result.responses = responses_.load();
        result.reads = reads_.load();
        result.writes = writes_.load();
        result.injectedFaults = injectedFaults_.load();
        result.protocolErrors = protocolErrors_.load();
        return result;
    }

private:
    void acceptLoop() {
        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener_, &readSet);
            timeval timeout{};
            timeout.tv_usec = 200000;
#ifdef _WIN32
            const auto ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const auto ready = select(listener_ + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (ready <= 0) {
                if (ready < 0 && !retryableSocketError() && running_.load()) {
                    protocolErrors_.fetch_add(1);
                }
                continue;
            }
            const auto client = accept(listener_, nullptr, nullptr);
            if (client == kInvalidSocket) {
                if (running_.load() && !retryableSocketError()) protocolErrors_.fetch_add(1);
                continue;
            }
            acceptedConnections_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clients_.insert(client);
            }
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            clientThreads_.emplace_back([this, client] { clientLoop(client); });
        }
    }

    void stateLoop() {
        while (running_.load()) {
            try {
                loadState(false);
            } catch (const std::exception&) {
                stateReloadErrors_.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    bool receiveExact(SocketHandle socket, std::uint8_t* output, std::size_t size) {
        std::size_t received = 0;
        while (running_.load() && received < size) {
#ifdef _WIN32
            const auto count = recv(socket, reinterpret_cast<char*>(output + received),
                                    static_cast<int>(size - received), 0);
#else
            const auto count = recv(socket, output + received, size - received, 0);
#endif
            if (count == 0) return false;
            if (count < 0) {
                if (retryableSocketError()) continue;
                return false;
            }
            received += static_cast<std::size_t>(count);
        }
        return received == size;
    }

    bool sendAll(SocketHandle socket, const std::vector<std::uint8_t>& bytes) {
        std::size_t sent = 0;
        while (running_.load() && sent < bytes.size()) {
#ifdef _WIN32
            const auto count = send(socket, reinterpret_cast<const char*>(bytes.data() + sent),
                                    static_cast<int>(bytes.size() - sent), 0);
#else
            const auto count = send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
            if (count <= 0) {
                if (count < 0 && retryableSocketError()) continue;
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return sent == bytes.size();
    }

    void clientLoop(SocketHandle client) {
        while (running_.load()) {
            std::vector<std::uint8_t> header(7);
            if (!receiveExact(client, header.data(), header.size())) break;
            const auto protocol = readU16(header, 2);
            const auto length = readU16(header, 4);
            if (protocol != 0 || length < 2 || length > 254) {
                protocolErrors_.fetch_add(1);
                break;
            }
            std::vector<std::uint8_t> pdu(length - 1);
            if (!receiveExact(client, pdu.data(), pdu.size())) break;
            requests_.fetch_add(1);
            if (!handleRequest(client, header, pdu)) break;
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.erase(client);
        }
        closeSocket(client);
    }

    bool handleRequest(
        SocketHandle client,
        const std::vector<std::uint8_t>& header,
        const std::vector<std::uint8_t>& request
    ) {
        const int slave = header[6];
        const auto function = request.empty() ? 0 : request[0];
        const bool write = function == 5 || function == 6 || function == 15 || function == 16;
        FaultMode fault = FaultMode::None;
        ControlState state;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state = state_;
            const auto item = state.faults.find(slave);
            if (item != state.faults.end()) fault = item->second;
        }
        if (std::find(state.slaves.begin(), state.slaves.end(), slave) == state.slaves.end()) {
            return sendResponse(client, header, {static_cast<std::uint8_t>(function | 0x80), 0x0B});
        }
        if (fault == FaultMode::Timeout || (write && fault == FaultMode::WriteTimeout)) {
            injectedFaults_.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, options_.faultDelayMs)));
            return true;
        }
        if (fault == FaultMode::Disconnect) {
            injectedFaults_.fetch_add(1);
            shutdownSocket(client);
            return false;
        }
        if (fault == FaultMode::Exception) {
            injectedFaults_.fetch_add(1);
            return sendResponse(client, header, {static_cast<std::uint8_t>(function | 0x80), 0x04});
        }
        if (write && fault == FaultMode::WriteVerifyFailed) {
            injectedFaults_.fetch_add(1);
        }

        try {
            std::vector<std::uint8_t> response;
            switch (function) {
                case 1:
                case 2:
                    response = readBits(state, slave, function, request);
                    reads_.fetch_add(1);
                    break;
                case 3:
                case 4:
                    response = readRegisters(state, slave, function, request);
                    reads_.fetch_add(1);
                    break;
                case 5:
                case 6:
                case 15:
                case 16:
                    response = writeValues(state, slave, function, request,
                                           fault == FaultMode::WriteVerifyFailed);
                    writes_.fetch_add(1);
                    break;
                default:
                    response = {static_cast<std::uint8_t>(function | 0x80), 0x01};
                    protocolErrors_.fetch_add(1);
                    break;
            }
            return sendResponse(client, header, response);
        } catch (const std::exception&) {
            protocolErrors_.fetch_add(1);
            return sendResponse(client, header, {static_cast<std::uint8_t>(function | 0x80), 0x03});
        }
    }

    std::vector<std::uint8_t> readRegisters(
        const ControlState& state,
        int slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& request
    ) {
        if (request.size() != 5) throw std::runtime_error("invalid register read request");
        const auto start = readU16(request, 1);
        const auto count = readU16(request, 3);
        if (count < 1 || count > 125 || static_cast<unsigned int>(start) + count > 65536U) {
            throw std::runtime_error("invalid register read range");
        }
        const std::string area = function == 3 ? "holding" : "input";
        std::vector<std::uint8_t> response{function, static_cast<std::uint8_t>(count * 2)};
        for (std::uint16_t offset = 0; offset < count; ++offset) {
            appendU16(response, valueFor(state, slave, area, start + offset));
        }
        return response;
    }

    std::vector<std::uint8_t> readBits(
        const ControlState& state,
        int slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& request
    ) {
        if (request.size() != 5) throw std::runtime_error("invalid bit read request");
        const auto start = readU16(request, 1);
        const auto count = readU16(request, 3);
        if (count < 1 || count > 2000 || static_cast<unsigned int>(start) + count > 65536U) {
            throw std::runtime_error("invalid bit read range");
        }
        const std::string area = function == 1 ? "coil" : "discrete";
        const auto byteCount = static_cast<std::size_t>((count + 7) / 8);
        std::vector<std::uint8_t> response(2 + byteCount, 0);
        response[0] = function;
        response[1] = static_cast<std::uint8_t>(byteCount);
        for (std::uint16_t offset = 0; offset < count; ++offset) {
            if (valueFor(state, slave, area, start + offset) != 0) {
                response[2 + offset / 8] |= static_cast<std::uint8_t>(1U << (offset % 8));
            }
        }
        return response;
    }

    std::vector<std::uint8_t> writeValues(
        const ControlState&,
        int slave,
        std::uint8_t function,
        const std::vector<std::uint8_t>& request,
        bool ignoreWrite
    ) {
        if ((function == 5 || function == 6) && request.size() == 5) {
            const auto address = readU16(request, 1);
            const auto value = readU16(request, 3);
            if (function == 5 && value != 0x0000 && value != 0xFF00) {
                throw std::runtime_error("invalid coil value");
            }
            if (!ignoreWrite) {
                std::lock_guard<std::mutex> lock(valuesMutex_);
                writtenValues_[{slave, function == 5 ? "coil" : "holding", address}] =
                    function == 5 ? static_cast<std::uint16_t>(value == 0xFF00) : value;
            }
            return request;
        }
        if (function == 15 || function == 16) {
            if (request.size() < 6) throw std::runtime_error("invalid multiple write request");
            const auto start = readU16(request, 1);
            const auto count = readU16(request, 3);
            const auto byteCount = request[5];
            if (count < 1 || static_cast<unsigned int>(start) + count > 65536U ||
                request.size() != static_cast<std::size_t>(6 + byteCount)) {
                throw std::runtime_error("invalid multiple write range");
            }
            if ((function == 16 && (count > 123 || byteCount != count * 2)) ||
                (function == 15 && (count > 1968 || byteCount != (count + 7) / 8))) {
                throw std::runtime_error("invalid multiple write payload");
            }
            if (!ignoreWrite) {
                std::lock_guard<std::mutex> lock(valuesMutex_);
                for (std::uint16_t offset = 0; offset < count; ++offset) {
                    std::uint16_t value = 0;
                    if (function == 16) {
                        value = readU16(request, 6 + offset * 2);
                    } else {
                        value = static_cast<std::uint16_t>((request[6 + offset / 8] >> (offset % 8)) & 0x01);
                    }
                    writtenValues_[{slave, function == 15 ? "coil" : "holding", start + offset}] = value;
                }
            }
            std::vector<std::uint8_t> response{function};
            appendU16(response, start);
            appendU16(response, count);
            return response;
        }
        throw std::runtime_error("invalid write request");
    }

    std::uint16_t valueFor(
        const ControlState& state,
        int slave,
        const std::string& area,
        int address
    ) const {
        const ValueKey key{slave, area, address};
        const auto configured = state.values.find(key);
        if (configured != state.values.end()) return configured->second;
        {
            std::lock_guard<std::mutex> lock(valuesMutex_);
            const auto written = writtenValues_.find(key);
            if (written != writtenValues_.end()) return written->second;
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - startedAt_
        ).count();
        if (area == "coil" || area == "discrete") {
            return static_cast<std::uint16_t>(((elapsedMs / 1000) + slave + address) % 2);
        }
        const std::uint32_t areaOffset = area == "input" ? 500U : 0U;
        const std::uint32_t base = static_cast<std::uint32_t>(slave * 1000 + address) + areaOffset;
        if (state.scenario == "ramp") {
            return static_cast<std::uint16_t>((base + elapsedMs / 100) & 0xFFFF);
        }
        if (state.scenario == "random") {
            std::uint32_t value = static_cast<std::uint32_t>(elapsedMs / 200);
            value ^= static_cast<std::uint32_t>(slave * 0x9E3779B9U);
            value ^= static_cast<std::uint32_t>(address * 0x85EBCA6BU);
            value ^= value >> 16;
            value *= 0x7FEB352DU;
            value ^= value >> 15;
            return static_cast<std::uint16_t>(value & 0xFFFF);
        }
        if (state.scenario == "boundary") {
            return static_cast<std::uint16_t>((elapsedMs / 2000) % 2 == 0 ? 0 : 65535);
        }
        if (address == 0) {
            return static_cast<std::uint16_t>((elapsedMs / 1000) & 0xFFFF);
        }
        if (address == 1) {
            return static_cast<std::uint16_t>(2200 + ((elapsedMs / 500 + slave) % 20));
        }
        if (address == 2) {
            return static_cast<std::uint16_t>(100 + ((elapsedMs / 300 + slave) % 50));
        }
        return static_cast<std::uint16_t>(base & 0xFFFF);
    }

    bool sendResponse(
        SocketHandle client,
        const std::vector<std::uint8_t>& requestHeader,
        const std::vector<std::uint8_t>& pdu
    ) {
        std::vector<std::uint8_t> response;
        response.reserve(7 + pdu.size());
        response.push_back(requestHeader[0]);
        response.push_back(requestHeader[1]);
        response.push_back(0);
        response.push_back(0);
        appendU16(response, static_cast<std::uint16_t>(pdu.size() + 1));
        response.push_back(requestHeader[6]);
        response.insert(response.end(), pdu.begin(), pdu.end());
        if (!sendAll(client, response)) return false;
        responses_.fetch_add(1);
        return true;
    }

    void loadState(bool required) {
        if (options_.stateFile.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = ControlState{};
            return;
        }
        try {
            const auto loaded = StateFile::load(options_.stateFile);
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = loaded;
            stateLoaded_ = true;
        } catch (const std::exception&) {
            if (required) throw;
        }
    }

    SimulatorOptions options_;
    Clock::time_point startedAt_;
    std::atomic<bool> running_{false};
    SocketHandle listener_ = kInvalidSocket;
    std::uint16_t actualPort_ = 0;
    std::thread acceptThread_;
    std::thread stateThread_;
    mutable std::mutex clientsMutex_;
    std::set<SocketHandle> clients_;
    std::mutex clientThreadsMutex_;
    std::vector<std::thread> clientThreads_;
    mutable std::mutex stateMutex_;
    ControlState state_;
    bool stateLoaded_ = false;
    mutable std::mutex valuesMutex_;
    std::map<ValueKey, std::uint16_t> writtenValues_;
    std::atomic<std::uint64_t> acceptedConnections_{0};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> responses_{0};
    std::atomic<std::uint64_t> reads_{0};
    std::atomic<std::uint64_t> writes_{0};
    std::atomic<std::uint64_t> injectedFaults_{0};
    std::atomic<std::uint64_t> protocolErrors_{0};
    std::atomic<std::uint64_t> stateReloadErrors_{0};
};

ModbusTcpSimulator::ModbusTcpSimulator(SimulatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ModbusTcpSimulator::~ModbusTcpSimulator() = default;

void ModbusTcpSimulator::start() { impl_->start(); }
void ModbusTcpSimulator::stop() { impl_->stop(); }
bool ModbusTcpSimulator::running() const { return impl_->running(); }
std::uint16_t ModbusTcpSimulator::port() const { return impl_->port(); }
SimulatorStats ModbusTcpSimulator::stats() const { return impl_->stats(); }

}  // namespace gateway_test_lab
