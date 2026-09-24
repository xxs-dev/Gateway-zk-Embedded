#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_gateway {

namespace {

#ifndef _WIN32

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kAuthTagSize = 32;
constexpr std::size_t kMaxFrameSize = 64 * 1024;
constexpr std::size_t kReadBytesPerMember = 16 * 1024;
constexpr std::size_t kFramesPerMember = 4;
constexpr int kSendDeadlineMs = 20;
constexpr int kConnectDeadlineMs = 5;
constexpr int kPartialDeadlineMs = 1000;
constexpr int kPeerIdleMs = 5000;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

struct PeerEndpoint {
    std::string address;
    int tcpPort = 0;
    Deadline lastSeen;
};

struct TcpConnection {
    int fd = -1;
    bool outbound = false;
    std::string peerNodeId;
    std::string sourceAddress;
    std::vector<std::uint8_t> input;
    bool identified = false;
    Deadline lastFrame = Clock::now();
    Deadline partialSince = Clock::now();
};

struct ReadBudget {
    std::size_t bytes;
    std::size_t frames;
    std::size_t calls;
};

void closeFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

void setNonBlocking(int fd) {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("failed to set cluster socket non-blocking");
    }
}

void resetStream(int& fd) {
    // Teardown must not leave queued control frames retransmitting after expiry/stop.
    linger reset{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    closeFd(fd);
}

void configureStream(int fd) {
    setNonBlocking(fd);
    const int receiveSize = static_cast<int>(kMaxFrameSize);
    const int sendSize = static_cast<int>(kReadBytesPerMember);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveSize, sizeof(receiveSize)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendSize, sizeof(sendSize)) != 0) {
        throw std::runtime_error("failed to bound cluster socket buffers");
    }
#ifdef TCP_USER_TIMEOUT
    const unsigned int unacknowledgedMs = kPartialDeadlineMs;
    if (setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &unacknowledgedMs, sizeof(unacknowledgedMs)) != 0) {
        throw std::runtime_error("failed to bound cluster TCP retransmission time");
    }
#endif
}

bool waitWritable(int fd, Deadline deadline) {
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, POLLOUT, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (Clock::now() >= deadline) return false;
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 && (descriptor.revents & POLLOUT) != 0 &&
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
    }
    return false;
}

void bindToInterface(int fd, const std::string& interfaceName) {
#ifdef SO_BINDTODEVICE
    if (!interfaceName.empty() && interfaceName != "lo" &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interfaceName.c_str(), interfaceName.size() + 1) != 0) {
        throw std::runtime_error("failed to bind cluster socket to interface " + interfaceName + ": " + std::strerror(errno));
    }
#else
    (void)fd;
    (void)interfaceName;
#endif
}

std::string socketAddress(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr ? std::string() : buffer;
}

std::string interfaceAddress(const std::string& name) {
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return {};
    std::string fallback;
    std::string selected;
    for (auto* item = addresses; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET || name != item->ifa_name) continue;
        const auto address = socketAddress(*reinterpret_cast<sockaddr_in*>(item->ifa_addr));
        if (address.rfind("169.254.", 0) == 0) selected = address;
        if (fallback.empty()) fallback = address;
    }
    freeifaddrs(addresses);
    return selected.empty() ? fallback : selected;
}

bool parseEndpoint(const std::string& raw, int defaultPort, sockaddr_in* result) {
    auto host = raw;
    auto port = defaultPort;
    const auto colon = raw.rfind(':');
    if (colon != std::string::npos && raw.find(':') == colon) {
        host = raw.substr(0, colon);
        try {
            std::size_t used = 0;
            const auto value = raw.substr(colon + 1);
            port = std::stoi(value, &used);
            if (used != value.size()) return false;
        } catch (...) { return false; }
    }
    if (port <= 0 || port > 65535) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0 || resolved == nullptr) return false;
    *result = *reinterpret_cast<sockaddr_in*>(resolved->ai_addr);
    result->sin_port = htons(static_cast<std::uint16_t>(port));
    freeaddrinfo(resolved);
    return true;
}

std::size_t frameLength(const std::vector<std::uint8_t>& data) {
    if (data.size() < kHeaderSize) return 0;
    const auto flags = static_cast<std::uint16_t>((data[6] << 8U) | data[7]);
    const auto payload = (static_cast<std::uint32_t>(data[8]) << 24U) |
        (static_cast<std::uint32_t>(data[9]) << 16U) |
        (static_cast<std::uint32_t>(data[10]) << 8U) |
        static_cast<std::uint32_t>(data[11]);
    const auto overhead = kHeaderSize + ((flags & 1U) != 0 ? kAuthTagSize : 0U);
    // Length only: version/magic/authentication remain the shared codec's responsibility.
    if (payload > kMaxFrameSize - overhead) return kMaxFrameSize + 1;
    return overhead + payload;
}

class EthernetClusterTransport final : public IClusterTransport {
public:
    EthernetClusterTransport(EmsClusterConfig config, std::string nodeId)
        : config_(std::move(config)), nodeId_(std::move(nodeId)),
          memberLimit_(static_cast<std::size_t>(std::max(2, std::min(5, config_.maxMembers)))),
          clusterHash_(EmsClusterProtocol::clusterIdHash(config_.clusterId)),
          configHash_(EmsClusterProtocol::configHash(config_)) {}

    ~EthernetClusterTransport() override { stop(); }

    void start() override {
        if (running_) return;
        const auto localAddress = interfaceAddress(config_.clusterInterface);
        if (localAddress.empty()) throw std::runtime_error("cluster interface has no IPv4 address");
        // Resolve at startup, never on the deadline-sensitive send path.
        seedEndpoints_.clear();
        for (std::size_t i = 0; i < std::min(memberLimit_, config_.seedPeers.size()); ++i) {
            sockaddr_in address{};
            if (parseEndpoint(config_.seedPeers[i], config_.discoveryPort, &address)) seedEndpoints_.push_back(address);
        }

        udpFd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpFd_ < 0) throw std::runtime_error("failed to create cluster UDP socket");
        int reuse = 1;
        setsockopt(udpFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        bindToInterface(udpFd_, config_.clusterInterface);
        sockaddr_in udpBind{};
        udpBind.sin_family = AF_INET;
        udpBind.sin_port = htons(static_cast<std::uint16_t>(config_.discoveryPort));
        udpBind.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(udpFd_, reinterpret_cast<sockaddr*>(&udpBind), sizeof(udpBind)) != 0) {
            const auto message = std::string("failed to bind cluster UDP port: ") + std::strerror(errno);
            stop();
            throw std::runtime_error(message);
        }
        ip_mreqn membership{};
        if (inet_pton(AF_INET, config_.multicastAddress.c_str(), &membership.imr_multiaddr) != 1) {
            stop();
            throw std::runtime_error("invalid cluster multicast address");
        }
        membership.imr_ifindex = static_cast<int>(if_nametoindex(config_.clusterInterface.c_str()));
        if (setsockopt(udpFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
            const auto message = std::string("failed to join cluster multicast group: ") + std::strerror(errno);
            stop();
            throw std::runtime_error(message);
        }
        in_addr multicastInterface{};
        inet_pton(AF_INET, localAddress.c_str(), &multicastInterface);
        setsockopt(udpFd_, IPPROTO_IP, IP_MULTICAST_IF, &multicastInterface, sizeof(multicastInterface));
        unsigned char ttl = 1;
        setsockopt(udpFd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setNonBlocking(udpFd_);

        tcpListenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcpListenFd_ < 0) {
            stop();
            throw std::runtime_error("failed to create cluster TCP listener");
        }
        setsockopt(tcpListenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        configureStream(tcpListenFd_);
        bindToInterface(tcpListenFd_, config_.clusterInterface);
        sockaddr_in tcpBind{};
        tcpBind.sin_family = AF_INET;
        tcpBind.sin_port = htons(static_cast<std::uint16_t>(config_.tcpPort));
        inet_pton(AF_INET, localAddress.c_str(), &tcpBind.sin_addr);
        if (bind(tcpListenFd_, reinterpret_cast<sockaddr*>(&tcpBind), sizeof(tcpBind)) != 0 ||
            listen(tcpListenFd_, static_cast<int>(memberLimit_ * 2)) != 0) {
            const auto message = std::string("failed to bind cluster TCP listener: ") + std::strerror(errno);
            stop();
            throw std::runtime_error(message);
        }
        setNonBlocking(tcpListenFd_);
        running_ = true;
    }

    void stop() override {
        running_ = false;
        for (auto& entry : connections_) resetStream(entry.second.fd);
        connections_.clear();
        connectionByPeer_.clear();
        endpoints_.clear();
        seedEndpoints_.clear();
        closeFd(tcpListenFd_);
        closeFd(udpFd_);
    }

    std::vector<EmsClusterInbound> poll(int timeoutMs) override {
        std::vector<EmsClusterInbound> received;
        if (!running_) return received;
        expirePeers();
        std::vector<pollfd> descriptors;
        descriptors.push_back({udpFd_, POLLIN, 0});
        descriptors.push_back({tcpListenFd_, POLLIN, 0});
        for (const auto& entry : connections_) descriptors.push_back({entry.first, POLLIN, 0});
        const auto ready = ::poll(descriptors.data(), descriptors.size(), std::max(0, timeoutMs));
        if (ready <= 0) return received;
        if ((descriptors[0].revents & POLLIN) != 0) receiveUdp(received);
        if ((descriptors[1].revents & POLLIN) != 0) acceptConnections();
        std::vector<int> readable;
        for (std::size_t i = 2; i < descriptors.size(); ++i) {
            if ((descriptors[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0) readable.push_back(descriptors[i].fd);
        }
        // Rotate service order so a continuously readable low fd cannot monopolize the budget.
        const auto next = std::upper_bound(readable.begin(), readable.end(), lastReadFd_);
        std::rotate(readable.begin(), next, readable.end());
        ReadBudget budget{kReadBytesPerMember * memberLimit_, kFramesPerMember * memberLimit_, 16 * memberLimit_};
        for (const auto fd : readable) {
            if (budget.bytes == 0 || budget.frames == 0 || budget.calls == 0) break;
            receiveTcp(fd, received, budget);
            lastReadFd_ = fd;
        }
        return received;
    }

    bool send(const EmsClusterOutbound& outbound) override {
        if (!running_) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(kSendDeadlineMs);
        expirePeers();
        const auto frame = EmsClusterProtocol::encode(outbound.message, config_);
        if (frame.size() > kMaxFrameSize || Clock::now() >= deadline) return false;
        if (outbound.discovery) return sendDiscovery(frame, deadline);
        if (!outbound.targetNodeId.empty()) return sendToPeer(outbound.targetNodeId, frame, deadline);
        bool allSent = true;
        bool attempted = false;
        std::vector<std::string> peers;
        for (const auto& entry : endpoints_) peers.push_back(entry.first);
        for (const auto& peer : peers) {
            if (peer == nodeId_) continue;
            attempted = true;
            allSent = sendToPeer(peer, frame, deadline) && allSent;
        }
        return !attempted || allSent;
    }

private:
    bool compatible(const EmsClusterMessage& message) const {
        return !message.senderNodeId.empty() &&
            message.clusterIdHash == clusterHash_ && message.configHash == configHash_;
    }

    bool rememberEndpoint(const EmsClusterMessage& message, const std::string& address) {
        if (message.tcpPort <= 0 || message.tcpPort > 65535 || address.empty()) return false;
        auto existing = endpoints_.find(message.senderNodeId);
        if (existing == endpoints_.end() && endpoints_.size() >= memberLimit_ - 1) return false;
        PeerEndpoint endpoint;
        endpoint.address = address;
        endpoint.tcpPort = message.tcpPort;
        endpoint.lastSeen = Clock::now();
        endpoints_[message.senderNodeId] = std::move(endpoint);
        return true;
    }

    void expirePeers() {
        const auto now = Clock::now();
        for (auto it = connections_.begin(); it != connections_.end();) {
            const auto& connection = it->second;
            const bool pending = !connection.identified || !connection.input.empty();
            if ((pending && now - connection.partialSince >= std::chrono::milliseconds(kPartialDeadlineMs)) ||
                now - connection.lastFrame >= std::chrono::milliseconds(kPeerIdleMs)) {
                const int fd = it->first;
                ++it;
                dropConnection(fd);
            } else ++it;
        }
        for (auto it = endpoints_.begin(); it != endpoints_.end();) {
            if (now - it->second.lastSeen >= std::chrono::milliseconds(kPeerIdleMs)) it = endpoints_.erase(it);
            else ++it;
        }
    }

    bool sendDiscovery(const std::vector<std::uint8_t>& frame, Deadline deadline) {
        bool sent = false;
        sockaddr_in multicast{};
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(static_cast<std::uint16_t>(config_.discoveryPort));
        if (Clock::now() < deadline && inet_pton(AF_INET, config_.multicastAddress.c_str(), &multicast.sin_addr) == 1) {
            sent = sendto(udpFd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast)) ==
                static_cast<ssize_t>(frame.size());
        }
        for (const auto& address : seedEndpoints_) {
            if (Clock::now() >= deadline) return false;
            sent = sendto(udpFd_, frame.data(), frame.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                static_cast<ssize_t>(frame.size()) || sent;
        }
        return sent;
    }

    void receiveUdp(std::vector<EmsClusterInbound>& received) {
        std::size_t bytes = kMaxFrameSize * memberLimit_;
        for (std::size_t attempt = 0; attempt < 2 * memberLimit_ && bytes != 0; ++attempt) {
            std::array<std::uint8_t, kMaxFrameSize> buffer{};
            sockaddr_in source{};
            socklen_t sourceSize = sizeof(source);
            const auto count = recvfrom(
                udpFd_, buffer.data(), std::min(buffer.size(), bytes), MSG_TRUNC,
                reinterpret_cast<sockaddr*>(&source), &sourceSize
            );
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count < 0) return;
            const auto size = static_cast<std::size_t>(count);
            const auto available = std::min(buffer.size(), bytes);
            bytes -= std::min(size, available);
            if (size == 0 || size > available) continue;
            try {
                auto message = EmsClusterProtocol::decode(buffer.data(), static_cast<std::size_t>(count), config_);
                const auto address = socketAddress(source);
                if (!compatible(message)) continue;
                // Core distinguishes self multicast from a duplicate identity/incarnation.
                // Preserve that signal without learning an endpoint back to ourselves.
                if (message.senderNodeId != nodeId_ && !rememberEndpoint(message, address)) continue;
                received.push_back({std::move(message), address});
            } catch (const std::exception&) {
                // Discovery is untrusted input. Invalid datagrams are intentionally ignored.
            }
        }
    }

    void acceptConnections() {
        for (std::size_t attempt = 0; attempt < memberLimit_; ++attempt) {
            sockaddr_in source{};
            socklen_t sourceSize = sizeof(source);
            const auto fd = accept(tcpListenFd_, reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (fd < 0) return;
            const auto pending = std::count_if(connections_.begin(), connections_.end(),
                [](const std::pair<const int, TcpConnection>& entry) { return !entry.second.identified; });
            if (connections_.size() >= 2 * memberLimit_ || static_cast<std::size_t>(pending) >= memberLimit_) {
                close(fd);
                continue;
            }
            try {
                configureStream(fd);
                TcpConnection connection;
                connection.fd = fd;
                connection.outbound = false;
                connection.sourceAddress = socketAddress(source);
                connection.input.reserve(kMaxFrameSize);
                connections_.emplace(fd, std::move(connection));
            } catch (...) {
                close(fd);
            }
        }
    }

    int connectPeer(const std::string& peer, Deadline sendDeadline) {
        if (Clock::now() >= sendDeadline) return -1;
        const auto existing = connectionByPeer_.find(peer);
        if (existing != connectionByPeer_.end() && connections_.count(existing->second) != 0) return existing->second;
        const auto endpoint = endpoints_.find(peer);
        if (endpoint == endpoints_.end() || connections_.size() >= 2 * memberLimit_) return -1;
        const auto deadline = std::min(sendDeadline, Clock::now() + std::chrono::milliseconds(kConnectDeadlineMs));
        const auto fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        try {
            bindToInterface(fd, config_.clusterInterface);
            configureStream(fd);
        } catch (...) {
            close(fd);
            return -1;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(endpoint->second.tcpPort));
        if (inet_pton(AF_INET, endpoint->second.address.c_str(), &address.sin_addr) != 1) {
            close(fd);
            return -1;
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        if (!waitWritable(fd, deadline)) {
            close(fd);
            return -1;
        }
        int socketError = 0;
        socklen_t errorSize = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorSize) != 0 || socketError != 0) {
            close(fd);
            return -1;
        }
        TcpConnection connection;
        connection.fd = fd;
        connection.outbound = true;
        connection.peerNodeId = peer;
        connection.sourceAddress = endpoint->second.address;
        connection.input.reserve(kMaxFrameSize);
        connections_.emplace(fd, std::move(connection));
        connectionByPeer_[peer] = fd;
        return fd;
    }

    bool sendToPeer(const std::string& peer, const std::vector<std::uint8_t>& frame, Deadline deadline) {
        auto fd = connectPeer(peer, deadline);
        if (fd < 0) return false;
        std::size_t offset = 0;
        for (std::size_t attempt = 0; attempt < 128 && offset < frame.size(); ++attempt) {
            if (Clock::now() >= deadline) break;
            const auto count = ::send(fd, frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                if (offset == frame.size() && Clock::now() < deadline) return true;
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (waitWritable(fd, deadline)) continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        // No deferred frame queue/retry; dropConnection discards the failed stream's queued bytes.
        dropConnection(fd);
        return false;
    }

    void identifyConnection(int fd, const std::string& peer) {
        auto current = connections_.find(fd);
        if (current == connections_.end() || peer.empty()) return;
        current->second.peerNodeId = peer;
        current->second.identified = true;
        const auto existing = connectionByPeer_.find(peer);
        if (existing == connectionByPeer_.end() || existing->second == fd || connections_.count(existing->second) == 0) {
            connectionByPeer_[peer] = fd;
            return;
        }
        const auto otherFd = existing->second;
        const bool preferOutbound = nodeId_ < peer;
        const bool keepCurrent = current->second.outbound == preferOutbound;
        if (keepCurrent) {
            dropConnection(otherFd);
            connectionByPeer_[peer] = fd;
        } else {
            dropConnection(fd);
        }
    }

    void receiveTcp(int fd, std::vector<EmsClusterInbound>& received, ReadBudget& budget) {
        auto connection = connections_.find(fd);
        if (connection == connections_.end()) return;
        std::size_t bytes = kReadBytesPerMember;
        std::size_t frames = kFramesPerMember;
        while (bytes != 0 && frames != 0 && budget.bytes != 0 && budget.frames != 0 && budget.calls != 0) {
            if ((!connection->second.identified || !connection->second.input.empty()) &&
                Clock::now() - connection->second.partialSince >= std::chrono::milliseconds(kPartialDeadlineMs)) {
                dropConnection(fd);
                return;
            }
            auto length = frameLength(connection->second.input);
            if (length > kMaxFrameSize) {
                dropConnection(fd);
                return;
            }
            const auto needed = length == 0 ? kHeaderSize : length;
            if (connection->second.input.size() < needed) {
                std::array<std::uint8_t, 8192> buffer{};
                const auto capacity = std::min(std::min(buffer.size(), needed - connection->second.input.size()),
                    std::min(bytes, budget.bytes));
                --budget.calls;
                const auto count = recv(fd, buffer.data(), capacity, 0);
                if (count > 0) {
                    if (connection->second.input.empty() && connection->second.identified) connection->second.partialSince = Clock::now();
                    connection->second.input.insert(connection->second.input.end(), buffer.begin(), buffer.begin() + count);
                    bytes -= static_cast<std::size_t>(count);
                    budget.bytes -= static_cast<std::size_t>(count);
                    // Decode a just-completed frame even at the byte/syscall boundary.
                    length = frameLength(connection->second.input);
                    if (length > kMaxFrameSize) {
                        dropConnection(fd);
                        return;
                    }
                    if (length == 0 || connection->second.input.size() < length) continue;
                } else {
                    if (count < 0 && errno == EINTR) continue;
                    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) dropConnection(fd);
                    return;
                }
            }
            --frames;
            --budget.frames;
            try {
                auto message = EmsClusterProtocol::decode(connection->second.input.data(), length, config_);
                const auto sourceAddress = connection->second.sourceAddress;
                const auto peer = message.senderNodeId;
                if (!compatible(message) ||
                    (!connection->second.peerNodeId.empty() && connection->second.peerNodeId != peer)) {
                    dropConnection(fd);
                    return;
                }
                if (peer == nodeId_) {
                    received.push_back({std::move(message), sourceAddress});
                    dropConnection(fd);
                    return;
                }
                if (!rememberEndpoint(message, sourceAddress)) {
                    dropConnection(fd);
                    return;
                }
                connection->second.input.clear();
                connection->second.lastFrame = Clock::now();
                connection->second.partialSince = connection->second.lastFrame;
                identifyConnection(fd, peer);
                if (connections_.count(fd) == 0) return;
                connection = connections_.find(fd);
                received.push_back({std::move(message), sourceAddress});
            } catch (const std::exception&) {
                dropConnection(fd);
                return;
            }
        }
    }

    void dropConnection(int fd) {
        const auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        if (!it->second.peerNodeId.empty()) {
            const auto mapped = connectionByPeer_.find(it->second.peerNodeId);
            if (mapped != connectionByPeer_.end() && mapped->second == fd) connectionByPeer_.erase(mapped);
        }
        auto descriptor = it->second.fd;
        resetStream(descriptor);
        connections_.erase(it);
    }

    EmsClusterConfig config_;
    std::string nodeId_;
    const std::size_t memberLimit_;
    const std::uint64_t clusterHash_;
    const std::uint64_t configHash_;
    bool running_ = false;
    int udpFd_ = -1;
    int tcpListenFd_ = -1;
    int lastReadFd_ = -1;
    std::vector<sockaddr_in> seedEndpoints_;
    std::map<std::string, PeerEndpoint> endpoints_;
    std::map<int, TcpConnection> connections_;
    std::map<std::string, int> connectionByPeer_;
};

#else

class EthernetClusterTransport final : public IClusterTransport {
public:
    EthernetClusterTransport(EmsClusterConfig, std::string) {}
    void start() override { throw std::runtime_error("Ethernet cluster transport is only supported on Linux"); }
    void stop() override {}
    std::vector<EmsClusterInbound> poll(int) override { return {}; }
    bool send(const EmsClusterOutbound&) override { return false; }
};

#endif

}  // namespace

std::unique_ptr<IClusterTransport> makeEthernetClusterTransport(
    EmsClusterConfig config,
    std::string nodeId
) {
    return std::unique_ptr<IClusterTransport>(new EthernetClusterTransport(std::move(config), std::move(nodeId)));
}

}  // namespace edge_gateway
