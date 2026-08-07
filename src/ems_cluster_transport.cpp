#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
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

struct PeerEndpoint {
    std::string address;
    int tcpPort = 0;
};

struct TcpConnection {
    int fd = -1;
    bool outbound = false;
    std::string peerNodeId;
    std::string sourceAddress;
    std::vector<std::uint8_t> input;
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
        try { port = std::stoi(raw.substr(colon + 1)); } catch (...) { return false; }
    }
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
    const auto total = kHeaderSize + static_cast<std::size_t>(payload) + ((flags & 1U) != 0 ? kAuthTagSize : 0U);
    return total <= kMaxFrameSize ? total : kMaxFrameSize + 1;
}

class EthernetClusterTransport final : public IClusterTransport {
public:
    EthernetClusterTransport(EmsClusterConfig config, std::string nodeId)
        : config_(std::move(config)), nodeId_(std::move(nodeId)) {}

    ~EthernetClusterTransport() override { stop(); }

    void start() override {
        if (running_) return;
        const auto localAddress = interfaceAddress(config_.clusterInterface);
        if (localAddress.empty()) throw std::runtime_error("cluster interface has no IPv4 address");

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
        bindToInterface(tcpListenFd_, config_.clusterInterface);
        sockaddr_in tcpBind{};
        tcpBind.sin_family = AF_INET;
        tcpBind.sin_port = htons(static_cast<std::uint16_t>(config_.tcpPort));
        inet_pton(AF_INET, localAddress.c_str(), &tcpBind.sin_addr);
        if (bind(tcpListenFd_, reinterpret_cast<sockaddr*>(&tcpBind), sizeof(tcpBind)) != 0 ||
            listen(tcpListenFd_, config_.maxMembers * 2) != 0) {
            const auto message = std::string("failed to bind cluster TCP listener: ") + std::strerror(errno);
            stop();
            throw std::runtime_error(message);
        }
        setNonBlocking(tcpListenFd_);
        running_ = true;
    }

    void stop() override {
        running_ = false;
        for (auto& entry : connections_) closeFd(entry.second.fd);
        connections_.clear();
        connectionByPeer_.clear();
        closeFd(tcpListenFd_);
        closeFd(udpFd_);
    }

    std::vector<EmsClusterInbound> poll(int timeoutMs) override {
        std::vector<EmsClusterInbound> received;
        if (!running_) return received;
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
        for (const auto fd : readable) receiveTcp(fd, received);
        return received;
    }

    bool send(const EmsClusterOutbound& outbound) override {
        if (!running_) return false;
        const auto frame = EmsClusterProtocol::encode(outbound.message, config_);
        if (outbound.discovery) return sendDiscovery(frame);
        if (!outbound.targetNodeId.empty()) return sendToPeer(outbound.targetNodeId, frame);
        bool allSent = true;
        bool attempted = false;
        std::vector<std::string> peers;
        for (const auto& entry : endpoints_) peers.push_back(entry.first);
        for (const auto& peer : peers) {
            if (peer == nodeId_) continue;
            attempted = true;
            allSent = sendToPeer(peer, frame) && allSent;
        }
        return !attempted || allSent;
    }

private:
    bool sendDiscovery(const std::vector<std::uint8_t>& frame) {
        bool sent = false;
        sockaddr_in multicast{};
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(static_cast<std::uint16_t>(config_.discoveryPort));
        if (inet_pton(AF_INET, config_.multicastAddress.c_str(), &multicast.sin_addr) == 1) {
            sent = sendto(udpFd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast)) ==
                static_cast<ssize_t>(frame.size());
        }
        for (const auto& seed : config_.seedPeers) {
            sockaddr_in address{};
            if (!parseEndpoint(seed, config_.discoveryPort, &address)) continue;
            sent = sendto(udpFd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
                static_cast<ssize_t>(frame.size()) || sent;
        }
        return sent;
    }

    void receiveUdp(std::vector<EmsClusterInbound>& received) {
        for (;;) {
            std::array<std::uint8_t, kMaxFrameSize> buffer{};
            sockaddr_in source{};
            socklen_t sourceSize = sizeof(source);
            const auto count = recvfrom(
                udpFd_, buffer.data(), buffer.size(), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceSize
            );
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count <= 0) return;
            try {
                auto message = EmsClusterProtocol::decode(buffer.data(), static_cast<std::size_t>(count), config_);
                const auto address = socketAddress(source);
                if (!message.senderNodeId.empty() && message.senderNodeId != nodeId_ && message.tcpPort > 0) {
                    endpoints_[message.senderNodeId] = {address, message.tcpPort};
                }
                received.push_back({std::move(message), address});
            } catch (const std::exception&) {
                // Discovery is untrusted input. Invalid datagrams are intentionally ignored.
            }
        }
    }

    void acceptConnections() {
        for (;;) {
            sockaddr_in source{};
            socklen_t sourceSize = sizeof(source);
            const auto fd = accept(tcpListenFd_, reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (fd < 0) return;
            try {
                setNonBlocking(fd);
                TcpConnection connection;
                connection.fd = fd;
                connection.outbound = false;
                connection.sourceAddress = socketAddress(source);
                connections_.emplace(fd, std::move(connection));
            } catch (...) {
                close(fd);
            }
        }
    }

    int connectPeer(const std::string& peer) {
        const auto endpoint = endpoints_.find(peer);
        if (endpoint == endpoints_.end()) return -1;
        const auto existing = connectionByPeer_.find(peer);
        if (existing != connectionByPeer_.end() && connections_.count(existing->second) != 0) return existing->second;
        const auto fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        try {
            bindToInterface(fd, config_.clusterInterface);
            setNonBlocking(fd);
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
        pollfd descriptor{fd, POLLOUT, 0};
        if (::poll(&descriptor, 1, 300) <= 0 || (descriptor.revents & POLLOUT) == 0) {
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
        connections_.emplace(fd, std::move(connection));
        connectionByPeer_[peer] = fd;
        return fd;
    }

    bool sendToPeer(const std::string& peer, const std::vector<std::uint8_t>& frame) {
        auto fd = connectPeer(peer);
        if (fd < 0) return false;
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const auto count = ::send(fd, frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd descriptor{fd, POLLOUT, 0};
                if (::poll(&descriptor, 1, 300) > 0) continue;
            }
            dropConnection(fd);
            return false;
        }
        return true;
    }

    void identifyConnection(int fd, const std::string& peer) {
        auto current = connections_.find(fd);
        if (current == connections_.end() || peer.empty()) return;
        current->second.peerNodeId = peer;
        const auto existing = connectionByPeer_.find(peer);
        if (existing == connectionByPeer_.end() || existing->second == fd || connections_.count(existing->second) == 0) {
            connectionByPeer_[peer] = fd;
            return;
        }
        const auto otherFd = existing->second;
        const auto other = connections_.find(otherFd);
        const bool preferOutbound = nodeId_ < peer;
        const bool keepCurrent = current->second.outbound == preferOutbound;
        if (keepCurrent) {
            dropConnection(otherFd);
            connectionByPeer_[peer] = fd;
        } else {
            dropConnection(fd);
        }
    }

    void receiveTcp(int fd, std::vector<EmsClusterInbound>& received) {
        auto connection = connections_.find(fd);
        if (connection == connections_.end()) return;
        bool closed = false;
        for (;;) {
            std::array<std::uint8_t, 8192> buffer{};
            const auto count = recv(fd, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                connection->second.input.insert(connection->second.input.end(), buffer.begin(), buffer.begin() + count);
                continue;
            }
            if (count == 0) closed = true;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) closed = true;
            break;
        }
        if (closed) {
            dropConnection(fd);
            return;
        }
        while (connection != connections_.end()) {
            const auto length = frameLength(connection->second.input);
            if (length == 0) break;
            if (length > kMaxFrameSize) {
                dropConnection(fd);
                return;
            }
            if (connection->second.input.size() < length) break;
            try {
                auto message = EmsClusterProtocol::decode(connection->second.input.data(), length, config_);
                const auto sourceAddress = connection->second.sourceAddress;
                const auto peer = message.senderNodeId;
                connection->second.input.erase(connection->second.input.begin(), connection->second.input.begin() + length);
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
        closeFd(descriptor);
        connections_.erase(it);
    }

    EmsClusterConfig config_;
    std::string nodeId_;
    bool running_ = false;
    int udpFd_ = -1;
    int tcpListenFd_ = -1;
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
