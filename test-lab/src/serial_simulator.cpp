#include "gateway_test_lab/serial_simulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace gateway_test_lab {
namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes.at(offset)) << 8U) |
        static_cast<std::uint16_t>(bytes.at(offset + 1U))
    );
}

std::uint16_t modbusCrc(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t crc = 0xFFFF;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const bool lowBit = (crc & 0x0001U) != 0;
            crc >>= 1U;
            if (lowBit) crc ^= 0xA001U;
        }
    }
    return crc;
}

bool validModbusCrc(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 4U) return false;
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto actual = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(frame[frame.size() - 1U]) << 8U |
        frame[frame.size() - 2U]
    );
    return modbusCrc(payload) == actual;
}

void appendModbusCrc(std::vector<std::uint8_t>& frame) {
    const auto crc = modbusCrc(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
}

std::uint8_t dltChecksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const auto byte : bytes) sum += byte;
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

std::string dltAddress(const std::vector<std::uint8_t>& frame) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (int index = 6; index >= 1; --index) {
        output << std::setw(2) << static_cast<int>(frame[static_cast<std::size_t>(index)]);
    }
    return output.str();
}

std::string dltDataId(const std::vector<std::uint8_t>& decodedData) {
    if (decodedData.size() < 4U) return {};
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (int index = 3; index >= 0; --index) {
        output << std::setw(2) << static_cast<int>(decodedData[static_cast<std::size_t>(index)]);
    }
    return output.str();
}

std::vector<std::uint8_t> encodeBcd(std::uint64_t value, std::size_t byteCount) {
    std::vector<std::uint8_t> result;
    result.reserve(byteCount);
    for (std::size_t index = 0; index < byteCount; ++index) {
        const auto pair = static_cast<int>(value % 100U);
        result.push_back(static_cast<std::uint8_t>(((pair / 10) << 4U) | (pair % 10)));
        value /= 100U;
    }
    return result;
}

std::vector<std::uint8_t> defaultDltValue(
    const std::string& dataId,
    int meter,
    const std::string& scenario,
    std::int64_t elapsedMs
) {
    std::uint64_t drift = 0;
    if (scenario == "ramp") drift = static_cast<std::uint64_t>((elapsedMs / 1000) % 100);
    if (scenario == "boundary") drift = (elapsedMs / 2000) % 2 == 0 ? 0U : 99U;
    if (scenario == "random") {
        auto seed = static_cast<std::uint64_t>(elapsedMs / 500) ^
            static_cast<std::uint64_t>(meter * 0x9E37U);
        seed ^= seed >> 7U;
        drift = seed % 100U;
    }
    if (dataId == "02010100") return encodeBcd(2200U + meter * 5U + drift % 10U, 2);
    if (dataId == "02010200") return encodeBcd(2210U + meter * 5U + drift % 10U, 2);
    if (dataId == "02010300") return encodeBcd(2190U + meter * 5U + drift % 10U, 2);
    if (dataId == "02020100") return encodeBcd(12345U + meter * 100U + drift, 3);
    if (dataId == "02020200") return encodeBcd(11345U + meter * 100U + drift, 3);
    if (dataId == "02020300") return encodeBcd(10345U + meter * 100U + drift, 3);
    if (dataId == "02030000") return encodeBcd(15234U + meter * 100U + drift, 3);
    if (dataId == "02040000") return encodeBcd(6234U + meter * 100U + drift, 3);
    if (dataId == "00010000") return encodeBcd(1234567U + meter * 1000U + drift, 4);
    if (dataId == "00020000") return encodeBcd(2234567U + meter * 1000U + drift, 4);
    return encodeBcd(1234U + static_cast<std::uint64_t>(meter) * 100U + drift, 4);
}

class SerialEndpoint {
public:
    explicit SerialEndpoint(const SerialSimulatorOptions& options) : options_(options) {}
    ~SerialEndpoint() { close(); }

    void open() {
        if (isOpen()) return;
#ifdef _WIN32
        if (lower(options_.device) == "auto") {
            throw std::runtime_error("--serial-device auto is only supported on Linux");
        }
        std::string path = options_.device;
        if (path.rfind("\\\\.\\", 0) != 0) path = "\\\\.\\" + path;
        handle_ = CreateFileA(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        );
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open serial device: " + options_.device);
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            close();
            throw std::runtime_error("GetCommState failed");
        }
        dcb.BaudRate = static_cast<DWORD>(options_.baudRate);
        dcb.ByteSize = static_cast<BYTE>(options_.dataBits);
        dcb.StopBits = options_.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
        dcb.Parity = NOPARITY;
        dcb.fParity = FALSE;
        if (options_.parity == "E") { dcb.Parity = EVENPARITY; dcb.fParity = TRUE; }
        if (options_.parity == "O") { dcb.Parity = ODDPARITY; dcb.fParity = TRUE; }
        if (!SetCommState(handle_, &dcb)) {
            close();
            throw std::runtime_error("SetCommState failed");
        }
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 10;
        timeouts.ReadTotalTimeoutConstant = 20;
        timeouts.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(handle_, &timeouts);
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        peerDevice_ = options_.device;
#else
        if (lower(options_.device) == "auto") {
            fd_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd_ < 0 || grantpt(fd_) != 0 || unlockpt(fd_) != 0) {
                close();
                throw std::runtime_error("failed to create test-lab pseudo terminal");
            }
            char* name = ptsname(fd_);
            if (name == nullptr) {
                close();
                throw std::runtime_error("failed to resolve test-lab pseudo terminal peer");
            }
            peerDevice_ = name;
        } else {
            fd_ = ::open(options_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd_ < 0) throw std::runtime_error("failed to open serial device: " + options_.device);
            peerDevice_ = options_.device;
            configureTermios(fd_);
        }
#endif
    }

    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    bool isOpen() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    std::vector<std::uint8_t> read(int timeoutMs) {
        std::vector<std::uint8_t> bytes(512);
#ifdef _WIN32
        if (!isOpen()) return {};
        DWORD count = 0;
        if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            throw std::runtime_error("serial simulator read failed");
        }
        bytes.resize(static_cast<std::size_t>(count));
        if (bytes.empty() && timeoutMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
#else
        if (!isOpen()) return {};
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd_, &readSet);
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        const auto ready = select(fd_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0) return {};
        const auto count = ::read(fd_, bytes.data(), bytes.size());
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO) return {};
            throw std::runtime_error("serial simulator read failed");
        }
        bytes.resize(static_cast<std::size_t>(count));
#endif
        return bytes;
    }

    void write(const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(handle_, bytes.data() + offset,
                           static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || count == 0) {
                throw std::runtime_error("serial simulator write failed");
            }
            offset += static_cast<std::size_t>(count);
#else
            const auto count = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("serial simulator write failed");
            }
            if (count == 0) throw std::runtime_error("serial simulator write returned zero bytes");
            offset += static_cast<std::size_t>(count);
#endif
        }
    }

    const std::string& peerDevice() const { return peerDevice_; }

private:
#ifndef _WIN32
    static speed_t baudConstant(int baudRate) {
        switch (baudRate) {
            case 1200: return B1200;
            case 2400: return B2400;
            case 4800: return B4800;
            case 9600: return B9600;
            case 19200: return B19200;
            case 38400: return B38400;
            case 57600: return B57600;
            case 115200: return B115200;
            default: throw std::runtime_error("unsupported serial simulator baud rate");
        }
    }

    void configureTermios(int fd) const {
        termios tty{};
        if (tcgetattr(fd, &tty) != 0) throw std::runtime_error("serial simulator tcgetattr failed");
        cfmakeraw(&tty);
        const auto speed = baudConstant(options_.baudRate);
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= options_.dataBits == 7 ? CS7 : CS8;
        if (options_.stopBits == 2) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~(PARENB | PARODD);
        if (options_.parity == "E") tty.c_cflag |= PARENB;
        if (options_.parity == "O") tty.c_cflag |= PARENB | PARODD;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tty) != 0) throw std::runtime_error("serial simulator tcsetattr failed");
    }
#endif

    SerialSimulatorOptions options_;
    std::string peerDevice_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void initializeSockets() {
#ifdef _WIN32
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    });
#endif
}

void closeSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

bool sendAll(SocketHandle socket, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto count = send(socket, reinterpret_cast<const char*>(bytes.data() + sent),
                                static_cast<int>(bytes.size() - sent), 0);
#else
        const auto count = send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool receiveExact(SocketHandle socket, std::uint8_t* bytes, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        const auto count = recv(socket, reinterpret_cast<char*>(bytes + received),
                                static_cast<int>(size - received), 0);
#else
        const auto count = recv(socket, bytes + received, size - received, 0);
#endif
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}

std::vector<std::uint8_t> exchangeModbusTcp(
    std::uint16_t port,
    const std::vector<std::uint8_t>& rtuFrame,
    int timeoutMs
) {
    initializeSockets();
    SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) return {};
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socketHandle);
        return {};
    }

    static std::atomic<std::uint16_t> transaction{1};
    const auto id = transaction.fetch_add(1);
    const auto pduSize = rtuFrame.size() - 3U;
    std::vector<std::uint8_t> request{
        static_cast<std::uint8_t>((id >> 8U) & 0xFFU), static_cast<std::uint8_t>(id & 0xFFU),
        0, 0,
        static_cast<std::uint8_t>(((pduSize + 1U) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((pduSize + 1U) & 0xFFU),
        rtuFrame[0]
    };
    request.insert(request.end(), rtuFrame.begin() + 1, rtuFrame.end() - 2);
    if (!sendAll(socketHandle, request)) {
        closeSocket(socketHandle);
        return {};
    }
    std::vector<std::uint8_t> header(7);
    if (!receiveExact(socketHandle, header.data(), header.size())) {
        closeSocket(socketHandle);
        return {};
    }
    const auto length = readU16(header, 4);
    if (length < 2U || length > 254U) {
        closeSocket(socketHandle);
        return {};
    }
    std::vector<std::uint8_t> pdu(static_cast<std::size_t>(length - 1U));
    if (!receiveExact(socketHandle, pdu.data(), pdu.size())) {
        closeSocket(socketHandle);
        return {};
    }
    closeSocket(socketHandle);
    std::vector<std::uint8_t> response{header[6]};
    response.insert(response.end(), pdu.begin(), pdu.end());
    appendModbusCrc(response);
    return response;
}

std::size_t modbusRequestSize(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < 2U) return 0;
    const auto function = buffer[1];
    if (function >= 1U && function <= 6U) return 8U;
    if ((function == 15U || function == 16U) && buffer.size() >= 7U) {
        return 9U + static_cast<std::size_t>(buffer[6]);
    }
    return buffer.size() >= 8U ? 8U : 0U;
}

std::vector<std::uint8_t> makeDltFrame(
    const std::vector<std::uint8_t>& request,
    std::uint8_t control,
    const std::vector<std::uint8_t>& decodedData
) {
    std::vector<std::uint8_t> response{0x68};
    response.insert(response.end(), request.begin() + 1, request.begin() + 7);
    response.push_back(0x68);
    response.push_back(control);
    response.push_back(static_cast<std::uint8_t>(decodedData.size()));
    for (const auto byte : decodedData) response.push_back(static_cast<std::uint8_t>(byte + 0x33U));
    response.push_back(dltChecksum(response));
    response.push_back(0x16);
    return response;
}

}  // namespace

SerialProtocol parseSerialProtocol(const std::string& value) {
    const auto normalized = lower(value);
    if (normalized == "modbus-rtu" || normalized == "modbus_rtu") return SerialProtocol::ModbusRtu;
    if (normalized == "dlt645" || normalized == "dlt645-2007" || normalized == "dlt645_2007") {
        return SerialProtocol::Dlt645;
    }
    throw std::runtime_error("unsupported serial simulator protocol: " + value);
}

std::string serialProtocolName(SerialProtocol protocol) {
    return protocol == SerialProtocol::ModbusRtu ? "modbus-rtu" : "dlt645-2007";
}

class SerialProtocolSimulator::Impl {
public:
    explicit Impl(SerialSimulatorOptions options)
        : options_(std::move(options)), endpoint_(options_), startedAt_(Clock::now()) {}

    ~Impl() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        try {
            state_ = StateFile::load(options_.stateFile);
            endpoint_.open();
            if (options_.protocol == SerialProtocol::ModbusRtu) {
                SimulatorOptions tcpOptions;
                tcpOptions.bindAddress = "127.0.0.1";
                tcpOptions.port = 0;
                tcpOptions.stateFile = options_.stateFile;
                tcpOptions.faultDelayMs = options_.faultDelayMs;
                modbusTcp_.reset(new ModbusTcpSimulator(tcpOptions));
                modbusTcp_->start();
            }
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            running_.store(false);
            endpoint_.close();
            modbusTcp_.reset();
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        endpoint_.close();
        if (worker_.joinable()) worker_.join();
        if (modbusTcp_) modbusTcp_->stop();
        modbusTcp_.reset();
    }

    bool running() const { return running_.load(); }
    std::string peerDevice() const { return endpoint_.peerDevice(); }

    SerialSimulatorStats stats() const {
        SerialSimulatorStats result;
        result.requests = requests_.load();
        result.responses = responses_.load();
        result.reads = reads_.load();
        result.writes = writes_.load();
        result.injectedFaults = injectedFaults_.load();
        result.protocolErrors = protocolErrors_.load();
        return result;
    }

private:
    void run() {
        std::vector<std::uint8_t> buffer;
        while (running_.load()) {
            try {
                auto bytes = endpoint_.read(25);
                if (bytes.empty()) continue;
                buffer.insert(buffer.end(), bytes.begin(), bytes.end());
                if (options_.protocol == SerialProtocol::ModbusRtu) processModbus(buffer);
                else processDlt645(buffer);
                if (buffer.size() > 4096U) buffer.clear();
            } catch (const std::exception&) {
                if (running_.load()) {
                    protocolErrors_.fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }
    }

    void processModbus(std::vector<std::uint8_t>& buffer) {
        while (buffer.size() >= 4U) {
            const auto size = modbusRequestSize(buffer);
            if (size == 0U || buffer.size() < size) return;
            std::vector<std::uint8_t> frame(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            if (!validModbusCrc(frame)) {
                buffer.erase(buffer.begin());
                protocolErrors_.fetch_add(1);
                continue;
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            requests_.fetch_add(1);
            const auto function = frame[1];
            const bool write = function == 5U || function == 6U || function == 15U || function == 16U;
            if (write) writes_.fetch_add(1); else reads_.fetch_add(1);
            auto response = exchangeModbusTcp(
                modbusTcp_->port(), frame, std::max(500, options_.faultDelayMs + 300)
            );
            if (response.empty()) {
                injectedFaults_.fetch_add(1);
                continue;
            }
            endpoint_.write(response);
            responses_.fetch_add(1);
        }
    }

    void reloadState() {
        try {
            auto loaded = StateFile::load(options_.stateFile);
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = std::move(loaded);
        } catch (const std::exception&) {
        }
    }

    void processDlt645(std::vector<std::uint8_t>& buffer) {
        while (!buffer.empty()) {
            const auto start = std::find(buffer.begin(), buffer.end(), static_cast<std::uint8_t>(0x68));
            if (start == buffer.end()) { buffer.clear(); return; }
            if (start != buffer.begin()) buffer.erase(buffer.begin(), start);
            if (buffer.size() < 10U) return;
            if (buffer[7] != 0x68U) { buffer.erase(buffer.begin()); continue; }
            const auto size = static_cast<std::size_t>(buffer[9]) + 12U;
            if (buffer.size() < size) return;
            std::vector<std::uint8_t> frame(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            if (frame.back() != 0x16U || dltChecksum(std::vector<std::uint8_t>(frame.begin(), frame.end() - 2)) != frame[frame.size() - 2U]) {
                protocolErrors_.fetch_add(1);
                continue;
            }
            handleDlt645(frame);
        }
    }

    void handleDlt645(const std::vector<std::uint8_t>& frame) {
        requests_.fetch_add(1);
        reloadState();
        const auto address = dltAddress(frame);
        int meter = 0;
        try { meter = std::stoi(address.substr(address.size() - 3U)); } catch (...) { meter = 0; }
        ControlState state;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state = state_;
        }
        if (std::find(state.slaves.begin(), state.slaves.end(), meter) == state.slaves.end()) return;
        FaultMode fault = FaultMode::None;
        const auto faultIt = state.faults.find(meter);
        if (faultIt != state.faults.end()) fault = faultIt->second;
        const auto control = frame[8];
        const bool write = control == 0x14U;
        if (write) writes_.fetch_add(1); else reads_.fetch_add(1);
        if (fault == FaultMode::Timeout || fault == FaultMode::Disconnect ||
            (write && fault == FaultMode::WriteTimeout)) {
            injectedFaults_.fetch_add(1);
            if (fault != FaultMode::Disconnect) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, options_.faultDelayMs)));
            }
            return;
        }

        std::vector<std::uint8_t> decoded;
        for (std::size_t index = 10; index < frame.size() - 2U; ++index) {
            decoded.push_back(static_cast<std::uint8_t>(frame[index] - 0x33U));
        }
        if (fault == FaultMode::Exception) {
            injectedFaults_.fetch_add(1);
            endpoint_.write(makeDltFrame(frame, write ? 0xD4U : 0xD1U, {0x02U}));
            responses_.fetch_add(1);
            return;
        }
        if (decoded.size() < 4U) {
            protocolErrors_.fetch_add(1);
            return;
        }
        const auto dataId = dltDataId(decoded);
        if (control == 0x11U) {
            std::vector<std::uint8_t> payload(decoded.begin(), decoded.begin() + 4);
            const auto key = address + ":" + dataId;
            const auto written = dltWrittenValues_.find(key);
            const auto value = written == dltWrittenValues_.end()
                ? defaultDltValue(dataId, meter, state.scenario,
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt_).count())
                : written->second;
            payload.insert(payload.end(), value.begin(), value.end());
            endpoint_.write(makeDltFrame(frame, 0x91U, payload));
            responses_.fetch_add(1);
            return;
        }
        if (control == 0x14U) {
            if (fault != FaultMode::WriteVerifyFailed && decoded.size() > 12U) {
                dltWrittenValues_[address + ":" + dataId] =
                    std::vector<std::uint8_t>(decoded.begin() + 12, decoded.end());
            } else if (fault == FaultMode::WriteVerifyFailed) {
                injectedFaults_.fetch_add(1);
            }
            endpoint_.write(makeDltFrame(frame, 0x94U, {}));
            responses_.fetch_add(1);
            return;
        }
        endpoint_.write(makeDltFrame(frame, 0xD1U, {0x04U}));
        responses_.fetch_add(1);
        protocolErrors_.fetch_add(1);
    }

    SerialSimulatorOptions options_;
    SerialEndpoint endpoint_;
    std::unique_ptr<ModbusTcpSimulator> modbusTcp_;
    Clock::time_point startedAt_;
    mutable std::mutex stateMutex_;
    ControlState state_;
    std::map<std::string, std::vector<std::uint8_t>> dltWrittenValues_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> responses_{0};
    std::atomic<std::uint64_t> reads_{0};
    std::atomic<std::uint64_t> writes_{0};
    std::atomic<std::uint64_t> injectedFaults_{0};
    std::atomic<std::uint64_t> protocolErrors_{0};
};

SerialProtocolSimulator::SerialProtocolSimulator(SerialSimulatorOptions options)
    : impl_(new Impl(std::move(options))) {}

SerialProtocolSimulator::~SerialProtocolSimulator() = default;
void SerialProtocolSimulator::start() { impl_->start(); }
void SerialProtocolSimulator::stop() { impl_->stop(); }
bool SerialProtocolSimulator::running() const { return impl_->running(); }
std::string SerialProtocolSimulator::peerDevice() const { return impl_->peerDevice(); }
SerialSimulatorStats SerialProtocolSimulator::stats() const { return impl_->stats(); }

}  // namespace gateway_test_lab
