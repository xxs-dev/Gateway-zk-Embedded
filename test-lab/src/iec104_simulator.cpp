#include "gateway_test_lab/iec104_simulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "gateway_test_lab/modbus_simulator.hpp"

namespace gateway_test_lab {
namespace {

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

void shutdownSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

bool sendAll(SocketHandle socket, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef _WIN32
        const auto count = send(socket, reinterpret_cast<const char*>(bytes.data() + offset),
                                static_cast<int>(bytes.size() - offset), 0);
#else
        const auto count = send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool receiveExact(SocketHandle socket, std::uint8_t* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const auto count = recv(socket, reinterpret_cast<char*>(bytes + offset),
                                static_cast<int>(size - offset), 0);
#else
        const auto count = recv(socket, bytes + offset, size - offset, 0);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::vector<std::uint8_t> makeIFrame(
    std::uint16_t sendSequence,
    std::uint16_t receiveSequence,
    const std::vector<std::uint8_t>& asdu
) {
    std::vector<std::uint8_t> frame{
        0x68, static_cast<std::uint8_t>(asdu.size() + 4U),
        static_cast<std::uint8_t>((sendSequence << 1U) & 0xFFU),
        static_cast<std::uint8_t>(((sendSequence << 1U) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((receiveSequence << 1U) & 0xFFU),
        static_cast<std::uint8_t>(((receiveSequence << 1U) >> 8U) & 0xFFU)
    };
    frame.insert(frame.end(), asdu.begin(), asdu.end());
    return frame;
}

std::uint16_t sendSequence(const std::vector<std::uint8_t>& frame) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[3]) << 8U | frame[2]) >> 1U
    );
}

void appendIoa(std::vector<std::uint8_t>& bytes, int ioa) {
    bytes.push_back(static_cast<std::uint8_t>(ioa & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((ioa >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((ioa >> 16) & 0xFF));
}

void appendCommonAddress(std::vector<std::uint8_t>& bytes, int commonAddress) {
    bytes.push_back(static_cast<std::uint8_t>(commonAddress & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((commonAddress >> 8) & 0xFF));
}

std::vector<std::uint8_t> singlePointAsdu(int commonAddress, int cause, bool value) {
    std::vector<std::uint8_t> asdu{1, 1, static_cast<std::uint8_t>(cause), 0};
    appendCommonAddress(asdu, commonAddress);
    appendIoa(asdu, 1001);
    asdu.push_back(value ? 1U : 0U);
    return asdu;
}

std::vector<std::uint8_t> floatPointAsdu(int commonAddress, int cause, float value) {
    std::vector<std::uint8_t> asdu{13, 1, static_cast<std::uint8_t>(cause), 0};
    appendCommonAddress(asdu, commonAddress);
    appendIoa(asdu, 2001);
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    asdu.push_back(static_cast<std::uint8_t>(raw & 0xFFU));
    asdu.push_back(static_cast<std::uint8_t>((raw >> 8U) & 0xFFU));
    asdu.push_back(static_cast<std::uint8_t>((raw >> 16U) & 0xFFU));
    asdu.push_back(static_cast<std::uint8_t>((raw >> 24U) & 0xFFU));
    asdu.push_back(0);
    return asdu;
}

}  // namespace

class Iec104Simulator::Impl {
public:
    explicit Impl(Iec104SimulatorOptions options) : options_(std::move(options)) {}
    ~Impl() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        initializeSockets();
        try {
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == kInvalidSocket) throw std::runtime_error("failed to create IEC104 socket");
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
                throw std::runtime_error("invalid IEC104 bind address: " + options_.bindAddress);
            }
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                listen(listener_, 4) != 0) {
                throw std::runtime_error("failed to bind IEC104 simulator");
            }
            socklen_t length = sizeof(address);
            if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                throw std::runtime_error("failed to resolve IEC104 simulator port");
            }
            port_ = ntohs(address.sin_port);
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            running_.store(false);
            closeSocket(listener_);
            listener_ = kInvalidSocket;
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        shutdownSocket(client_);
        shutdownSocket(listener_);
        closeSocket(client_);
        closeSocket(listener_);
        client_ = kInvalidSocket;
        listener_ = kInvalidSocket;
        if (worker_.joinable()) worker_.join();
    }

    bool running() const { return running_.load(); }
    std::uint16_t port() const { return port_; }

    Iec104SimulatorStats stats() const {
        Iec104SimulatorStats result;
        result.acceptedConnections = acceptedConnections_.load();
        result.receivedFrames = receivedFrames_.load();
        result.transmittedFrames = transmittedFrames_.load();
        result.commands = commands_.load();
        result.injectedFaults = injectedFaults_.load();
        result.protocolErrors = protocolErrors_.load();
        return result;
    }

private:
    ControlState state() const {
        try { return StateFile::load(options_.stateFile); } catch (...) { return {}; }
    }

    FaultMode currentFault() const {
        const auto loaded = state();
        const auto it = loaded.faults.find(1);
        return it == loaded.faults.end() ? FaultMode::None : it->second;
    }

    bool sendFrame(const std::vector<std::uint8_t>& frame) {
        if (!sendAll(client_, frame)) return false;
        transmittedFrames_.fetch_add(1);
        return true;
    }

    void sendData(const std::vector<std::uint8_t>& asdu) {
        sendFrame(makeIFrame(serverSendSequence_++, clientReceiveSequence_, asdu));
    }

    void sendPeriodic() {
        const auto loaded = state();
        const auto fault = loaded.faults.find(1);
        if (fault != loaded.faults.end() && fault->second != FaultMode::None) {
            injectedFaults_.fetch_add(1);
            if (fault->second == FaultMode::Disconnect) shutdownSocket(client_);
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt_
        ).count();
        bool signal = (elapsed / 1000) % 2 != 0;
        float value = 50.0F;
        if (loaded.scenario == "ramp") value += static_cast<float>((elapsed / 100) % 100) / 10.0F;
        if (loaded.scenario == "boundary") value = signal ? 1000.0F : -1000.0F;
        if (loaded.scenario == "random") value = static_cast<float>((elapsed * 2654435761ULL) % 10000U) / 100.0F;
        sendData(singlePointAsdu(options_.commonAddress, 3, signal));
        sendData(floatPointAsdu(options_.commonAddress, 3, value));
    }

    bool receiveFrame(std::vector<std::uint8_t>& frame, int timeoutMs) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(client_, &readSet);
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
        const auto ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const auto ready = select(client_ + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) return false;
        std::uint8_t header[2]{};
        if (!receiveExact(client_, header, 2U) || header[0] != 0x68U || header[1] < 4U) return false;
        frame.assign(header, header + 2);
        const auto oldSize = frame.size();
        frame.resize(oldSize + header[1]);
        return receiveExact(client_, frame.data() + oldSize, header[1]);
    }

    void handleFrame(const std::vector<std::uint8_t>& frame) {
        receivedFrames_.fetch_add(1);
        const auto fault = currentFault();
        if (fault == FaultMode::Timeout || fault == FaultMode::Exception) {
            injectedFaults_.fetch_add(1);
            return;
        }
        if (frame == std::vector<std::uint8_t>{0x68, 0x04, 0x07, 0x00, 0x00, 0x00}) {
            sendFrame({0x68, 0x04, 0x0B, 0x00, 0x00, 0x00});
            dataTransferStarted_ = true;
            return;
        }
        if (frame == std::vector<std::uint8_t>{0x68, 0x04, 0x43, 0x00, 0x00, 0x00}) {
            sendFrame({0x68, 0x04, 0x83, 0x00, 0x00, 0x00});
            return;
        }
        if (frame.size() < 7U || (frame[2] & 0x01U) != 0U) return;
        clientReceiveSequence_ = static_cast<std::uint16_t>(sendSequence(frame) + 1U);
        std::vector<std::uint8_t> asdu(frame.begin() + 6, frame.end());
        if (asdu.empty()) return;
        const auto type = asdu[0];
        if (type == 100U) {
            sendData(singlePointAsdu(options_.commonAddress, 20, true));
            sendData(floatPointAsdu(options_.commonAddress, 20, 50.0F));
            auto termination = asdu;
            if (termination.size() > 2U) termination[2] = 10U;
            sendData(termination);
            return;
        }
        if (type == 45U || type == 46U || type == 48U || type == 49U || type == 50U ||
            type == 103U || (type >= 110U && type <= 113U)) {
            commands_.fetch_add(1);
            auto confirmation = asdu;
            if (confirmation.size() > 2U) confirmation[2] = 7U;
            sendData(confirmation);
            if (type >= 45U && type <= 50U) {
                auto termination = asdu;
                if (termination.size() > 2U) termination[2] = 10U;
                sendData(termination);
            }
        }
    }

    bool acceptClient() {
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
        if (ready <= 0) return false;
        client_ = accept(listener_, nullptr, nullptr);
        if (client_ == kInvalidSocket) return false;
        acceptedConnections_.fetch_add(1);
        dataTransferStarted_ = false;
        serverSendSequence_ = 0;
        clientReceiveSequence_ = 0;
        return true;
    }

    void run() {
        while (running_.load()) {
            if (client_ == kInvalidSocket && !acceptClient()) continue;
            const auto nextPeriodic = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max(20, options_.periodMs));
            while (running_.load() && client_ != kInvalidSocket) {
                std::vector<std::uint8_t> frame;
                if (receiveFrame(frame, 20)) handleFrame(frame);
                if (dataTransferStarted_ && std::chrono::steady_clock::now() >= nextPeriodic_) {
                    sendPeriodic();
                    nextPeriodic_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max(20, options_.periodMs));
                }
                if (currentFault() == FaultMode::Disconnect) break;
            }
            closeSocket(client_);
            client_ = kInvalidSocket;
        }
    }

    Iec104SimulatorOptions options_;
    SocketHandle listener_ = kInvalidSocket;
    SocketHandle client_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::uint16_t serverSendSequence_ = 0;
    std::uint16_t clientReceiveSequence_ = 0;
    bool dataTransferStarted_ = false;
    std::chrono::steady_clock::time_point startedAt_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point nextPeriodic_ = std::chrono::steady_clock::now();
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<std::uint64_t> acceptedConnections_{0};
    std::atomic<std::uint64_t> receivedFrames_{0};
    std::atomic<std::uint64_t> transmittedFrames_{0};
    std::atomic<std::uint64_t> commands_{0};
    std::atomic<std::uint64_t> injectedFaults_{0};
    std::atomic<std::uint64_t> protocolErrors_{0};
};

Iec104Simulator::Iec104Simulator(Iec104SimulatorOptions options) : impl_(new Impl(std::move(options))) {}
Iec104Simulator::~Iec104Simulator() = default;
void Iec104Simulator::start() { impl_->start(); }
void Iec104Simulator::stop() { impl_->stop(); }
bool Iec104Simulator::running() const { return impl_->running(); }
std::uint16_t Iec104Simulator::port() const { return impl_->port(); }
Iec104SimulatorStats Iec104Simulator::stats() const { return impl_->stats(); }

}  // namespace gateway_test_lab
