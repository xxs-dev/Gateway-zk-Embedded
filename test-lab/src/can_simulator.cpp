#include "gateway_test_lab/can_simulator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "edge_gateway/virtual_can_datagram.hpp"
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

bool usesUdpTestTransport(const CanSimulatorOptions& options) {
    return options.transportMode == "udp_test";
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

void closeSocketHandle(SocketHandle socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

}  // namespace

class CanSimulator::Impl {
public:
    explicit Impl(CanSimulatorOptions options) : options_(std::move(options)) {}
    ~Impl() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        try {
            initializeSockets();
            if (usesUdpTestTransport(options_)) {
                openUdpSocket();
            } else {
                openSocketCanSocket();
            }
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            running_.store(false);
            closeSocket();
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        closeSocket();
        if (worker_.joinable()) worker_.join();
    }

    bool running() const { return running_.load(); }

    CanSimulatorStats stats() const {
        CanSimulatorStats result;
        result.transmittedFrames = transmittedFrames_.load();
        result.receivedFrames = receivedFrames_.load();
        result.injectedFaults = injectedFaults_.load();
        result.protocolErrors = protocolErrors_.load();
        return result;
    }

private:
    void openUdpSocket() {
        if (options_.udpListenPort <= 0 || options_.udpListenPort > 65535 ||
            options_.udpPeerPort <= 0 || options_.udpPeerPort > 65535) {
            throw std::invalid_argument("virtual CAN UDP ports must be between 1 and 65535");
        }
        socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd_ == kInvalidSocket) {
            throw std::runtime_error("failed to create virtual CAN UDP socket");
        }
        int reuse = 1;
#ifdef _WIN32
        setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(options_.udpListenPort));
        if (inet_pton(AF_INET, options_.udpBindAddress.c_str(), &address.sin_addr) != 1 ||
            bind(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error(
                "failed to bind virtual CAN UDP socket: " + options_.udpBindAddress + ":" +
                std::to_string(options_.udpListenPort)
            );
        }
    }

    void openSocketCanSocket() {
#ifdef _WIN32
        throw std::runtime_error("SocketCAN simulator is only supported on Linux; use --transport udp_test");
#else
        socketFd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (socketFd_ < 0) throw std::runtime_error("failed to create test-lab CAN socket");
        ifreq request{};
        std::strncpy(request.ifr_name, options_.interfaceName.c_str(), IFNAMSIZ - 1);
        if (ioctl(socketFd_, SIOCGIFINDEX, &request) != 0) {
            throw std::runtime_error("CAN interface not found: " + options_.interfaceName);
        }
        sockaddr_can address{};
        address.can_family = AF_CAN;
        address.can_ifindex = request.ifr_ifindex;
        if (bind(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("failed to bind test-lab CAN socket");
        }
#endif
    }

    void closeSocket() {
        const auto socket = socketFd_;
        socketFd_ = kInvalidSocket;
        closeSocketHandle(socket);
    }

    ControlState loadState() const {
        try {
            return StateFile::load(options_.stateFile);
        } catch (const std::exception&) {
            return {};
        }
    }

    void sendFrame(
        std::uint32_t frameId,
        bool extended,
        const std::uint8_t* data,
        std::size_t size
    ) {
        bool sent = false;
        if (usesUdpTestTransport(options_)) {
            edge_gateway::VirtualCanFrame frame;
            frame.frameId = frameId;
            frame.extended = extended;
            frame.fd = size > 8U;
            frame.payload.assign(data, data + size);
            const auto bytes = edge_gateway::encodeVirtualCanDatagram(frame);
            sockaddr_in peer{};
            peer.sin_family = AF_INET;
            peer.sin_port = htons(static_cast<std::uint16_t>(options_.udpPeerPort));
            if (!bytes.empty() && inet_pton(AF_INET, options_.udpPeerAddress.c_str(), &peer.sin_addr) == 1) {
#ifdef _WIN32
                const auto count = sendto(
                    socketFd_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                    reinterpret_cast<sockaddr*>(&peer), sizeof(peer)
                );
#else
                const auto count = sendto(
                    socketFd_, bytes.data(), bytes.size(), 0,
                    reinterpret_cast<sockaddr*>(&peer), sizeof(peer)
                );
#endif
                sent = count == static_cast<decltype(count)>(bytes.size());
            }
        } else {
#ifndef _WIN32
            if (size <= CAN_MAX_DLEN) {
                can_frame frame{};
                frame.can_id = frameId | (extended ? CAN_EFF_FLAG : 0U);
                frame.can_dlc = static_cast<__u8>(size);
                std::memcpy(frame.data, data, size);
                sent = ::write(socketFd_, &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame));
            }
#endif
        }
        if (sent) {
            transmittedFrames_.fetch_add(1);
        } else if (running_.load()) {
            protocolErrors_.fetch_add(1);
        }
    }

    void transmit(const ControlState& state, std::int64_t tick) {
        const auto fault = state.faults.find(1);
        if (fault != state.faults.end() && fault->second != FaultMode::None) {
            injectedFaults_.fetch_add(1);
            return;
        }
        std::uint16_t speed = 12000U;
        std::int16_t temperature = 250;
        std::uint8_t status = 1;
        if (state.scenario == "ramp") {
            speed = static_cast<std::uint16_t>(10000 + tick % 4000);
            temperature = static_cast<std::int16_t>(200 + tick % 200);
        } else if (state.scenario == "random") {
            const auto mixed = static_cast<std::uint32_t>(tick * 2654435761ULL);
            speed = static_cast<std::uint16_t>(8000U + mixed % 8000U);
            temperature = static_cast<std::int16_t>(150 + mixed % 300U);
        } else if (state.scenario == "boundary") {
            speed = tick % 2 == 0 ? 0U : 65535U;
            temperature = tick % 2 == 0 ? -400 : 1250;
            status = static_cast<std::uint8_t>(tick % 2);
        }
        const std::uint8_t telemetry[8] = {
            static_cast<std::uint8_t>(speed & 0xFFU),
            static_cast<std::uint8_t>((speed >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(static_cast<std::uint16_t>(temperature) & 0xFFU),
            static_cast<std::uint8_t>((static_cast<std::uint16_t>(temperature) >> 8U) & 0xFFU),
            status, 0, 0, 0
        };
        sendFrame(0x18FF50E5U, true, telemetry, sizeof(telemetry));
    }

    void receiveOnce() {
        const auto socket = socketFd_;
        if (socket == kInvalidSocket) return;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);
        timeval timeout{};
        timeout.tv_usec = 1000;
#ifdef _WIN32
        const auto ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const auto ready = select(socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) return;
        bool received = false;
        if (usesUdpTestTransport(options_)) {
            std::uint8_t bytes[edge_gateway::kVirtualCanHeaderSize + edge_gateway::kVirtualCanMaxPayloadSize]{};
#ifdef _WIN32
            const auto count = recv(socket, reinterpret_cast<char*>(bytes), static_cast<int>(sizeof(bytes)), 0);
#else
            const auto count = recv(socket, bytes, sizeof(bytes), 0);
#endif
            edge_gateway::VirtualCanFrame frame;
            received = count > 0 && edge_gateway::decodeVirtualCanDatagram(
                bytes, static_cast<std::size_t>(count), frame
            );
        } else {
#ifndef _WIN32
            can_frame frame{};
            received = ::read(socket, &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame));
#endif
        }
        if (received) {
            receivedFrames_.fetch_add(1);
        } else if (running_.load()) {
            protocolErrors_.fetch_add(1);
        }
    }

    void run() {
        auto nextTransmit = std::chrono::steady_clock::now();
        std::int64_t tick = 0;
        while (running_.load()) {
            receiveOnce();
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextTransmit) {
                transmit(loadState(), tick++);
                nextTransmit = now + std::chrono::milliseconds(std::max(10, options_.periodMs));
            }
        }
    }

    CanSimulatorOptions options_;
    std::atomic<bool> running_{false};
    SocketHandle socketFd_ = kInvalidSocket;
    std::thread worker_;
    std::atomic<std::uint64_t> transmittedFrames_{0};
    std::atomic<std::uint64_t> receivedFrames_{0};
    std::atomic<std::uint64_t> injectedFaults_{0};
    std::atomic<std::uint64_t> protocolErrors_{0};
};

CanSimulator::CanSimulator(CanSimulatorOptions options) : impl_(new Impl(std::move(options))) {}
CanSimulator::~CanSimulator() = default;
void CanSimulator::start() { impl_->start(); }
void CanSimulator::stop() { impl_->stop(); }
bool CanSimulator::running() const { return impl_->running(); }
CanSimulatorStats CanSimulator::stats() const { return impl_->stats(); }

}  // namespace gateway_test_lab
