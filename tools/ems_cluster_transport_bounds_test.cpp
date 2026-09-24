#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <dirent.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
bool measure = false;
int accepts = 0, reads = 0, datagrams = 0, writeCalls = 0, maxWait = 0;
std::size_t readBytes = 0;
enum class WriteMode { Real, Trickle, ConnectTimeout };
WriteMode writeMode = WriteMode::Real;
void resetCounters() { accepts = reads = datagrams = writeCalls = maxWait = 0; readBytes = 0; }
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
}

// Wrappers observe real sockets except for two bounded, deterministic fault sequences.
extern "C" {
int __real_accept(int, sockaddr*, socklen_t*);
ssize_t __real_recv(int, void*, size_t, int);
ssize_t __real_recvfrom(int, void*, size_t, int, sockaddr*, socklen_t*);
ssize_t __real_send(int, const void*, size_t, int);
int __real_poll(pollfd*, nfds_t, int);
int __wrap_accept(int fd, sockaddr* address, socklen_t* size) {
    if (measure) ++accepts;
    return __real_accept(fd, address, size);
}
ssize_t __wrap_recv(int fd, void* buffer, size_t size, int flags) {
    const auto result = __real_recv(fd, buffer, size, flags);
    if (measure) { ++reads; if (result > 0) readBytes += result; }
    return result;
}
ssize_t __wrap_recvfrom(int fd, void* buffer, size_t size, int flags, sockaddr* address, socklen_t* length) {
    if (measure) ++datagrams;
    return __real_recvfrom(fd, buffer, size, flags, address, length);
}
ssize_t __wrap_send(int fd, const void* buffer, size_t size, int flags) {
    if (writeMode == WriteMode::Trickle) {
        ++writeCalls;
        if (writeCalls % 2 == 0) { errno = EAGAIN; return -1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::min(size, size_t(1));
    }
    return __real_send(fd, buffer, size, flags);
}
int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (writeMode != WriteMode::Real && count == 1 && fds[0].events == POLLOUT) {
        maxWait = std::max(maxWait, timeout);
        if (writeMode == WriteMode::ConnectTimeout) return 0;
        fds[0].revents = POLLOUT;
        return 1;
    }
    return __real_poll(fds, count, timeout);
}
}

namespace {
using namespace edge_gateway;
struct Socket {
    int fd;
    explicit Socket(int type) : fd(socket(AF_INET, type, 0)) { require(fd >= 0, "socket"); }
    ~Socket() { close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
sockaddr_in address(int port) {
    sockaddr_in value{};
    value.sin_family = AF_INET;
    value.sin_port = htons(static_cast<uint16_t>(port));
    value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return value;
}
int freePort(int type) {
    Socket socket(type);
    auto value = address(0);
    require(bind(socket.fd, reinterpret_cast<sockaddr*>(&value), sizeof(value)) == 0, "bind ephemeral");
    socklen_t size = sizeof(value);
    require(getsockname(socket.fd, reinterpret_cast<sockaddr*>(&value), &size) == 0, "getsockname");
    return ntohs(value.sin_port);
}
EmsClusterConfig configFor(int members = 2) {
    EmsClusterConfig config;
    config.enabled = true;
    config.clusterId = "transport-bounds";
    config.clusterInterface = "lo";
    config.securityMode = "psk";
    config.psk = "isolated-transport-test-key";
    config.expectedMembers = members;
    config.maxMembers = members;
    config.discoveryPort = freePort(SOCK_DGRAM);
    config.tcpPort = freePort(SOCK_STREAM);
    return config;
}
EmsClusterMessage messageFor(const EmsClusterConfig& config, const std::string& peer = "peer") {
    EmsClusterMessage message;
    message.clusterIdHash = EmsClusterProtocol::clusterIdHash(config.clusterId);
    message.configHash = EmsClusterProtocol::configHash(config);
    message.senderNodeId = peer;
    message.senderBootId = "transport-test-process";
    message.sequence = 1;
    message.tcpPort = config.tcpPort;
    return message;
}
void tcpConnect(const Socket& socket, int port) {
    auto value = address(port);
    require(connect(socket.fd, reinterpret_cast<sockaddr*>(&value), sizeof(value)) == 0, "connect loopback");
}
void writeAll(const Socket& socket, const std::vector<uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = __real_send(socket.fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        require(count > 0, "fixture write");
        offset += count;
    }
}
void udpWrite(const Socket& socket, int port, const std::vector<uint8_t>& frame) {
    auto value = address(port);
    require(sendto(socket.fd, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&value), sizeof(value)) ==
            static_cast<ssize_t>(frame.size()), "fixture datagram");
}
int fdCount() {
    auto* directory = opendir("/proc/self/fd");
    require(directory != nullptr, "fd directory");
    int count = 0;
    while (readdir(directory) != nullptr) ++count;
    closedir(directory);
    return count;
}
std::vector<EmsClusterInbound> measuredPoll(IClusterTransport& transport) {
    resetCounters();
    measure = true;
    auto result = transport.poll(0);
    measure = false;
    return result;
}
void udpBudget() {
    for (int members : {2, 5}) {
        auto config = configFor(members);
        auto transport = makeEthernetClusterTransport(config, "local");
        transport->start();
        Socket socket(SOCK_DGRAM);
        const auto frame = EmsClusterProtocol::encode(messageFor(config), config);
        for (int i = 0; i < 32; ++i) udpWrite(socket, config.discoveryPort, frame);
        const auto received = measuredPoll(*transport);
        require(datagrams <= 2 * members, "UDP per-poll datagram budget exceeded");
        require(!received.empty(), "UDP valid frame not delivered");
    }
}
void acceptAndPendingBudget() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    std::vector<std::unique_ptr<Socket>> clients;
    for (int i = 0; i < 4; ++i) {
        clients.emplace_back(new Socket(SOCK_STREAM));
        tcpConnect(*clients.back(), config.tcpPort);
    }
    const int before = fdCount();
    measuredPoll(*transport);
    require(accepts <= config.maxMembers, "TCP per-poll accept budget exceeded");
    transport->poll(0);
    require(fdCount() <= before + config.maxMembers, "too many unauthenticated connections retained");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    transport->poll(0);
    require(fdCount() == before, "unauthenticated connections did not expire");
}
void tcpBudgetAndPartial() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket client(SOCK_STREAM);
    tcpConnect(client, config.tcpPort);
    transport->poll(0);
    const auto frame = EmsClusterProtocol::encode(messageFor(config), config);
    writeAll(client, std::vector<uint8_t>(frame.begin(), frame.begin() + 9));
    require(transport->poll(0).empty(), "partial header delivered");
    std::vector<uint8_t> flood(frame.begin() + 9, frame.end());
    for (int i = 1; i < 64; ++i) flood.insert(flood.end(), frame.begin(), frame.end());
    writeAll(client, flood);
    auto result = measuredPoll(*transport);
    require(!result.empty() && result.size() <= 4U * config.maxMembers, "TCP frame budget exceeded");
    require(readBytes <= 16384U * config.maxMembers && reads <= 16 * config.maxMembers, "TCP byte/syscall budget exceeded");
    std::size_t delivered = result.size();
    for (int i = 0; i < 100 && delivered < 64; ++i) delivered += transport->poll(0).size();
    require(delivered == 64, "buffered flood did not drain over bounded polls");
}
void malformedAndVersion() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    for (int variant = 0; variant < 3; ++variant) {
        Socket client(SOCK_STREAM);
        tcpConnect(client, config.tcpPort);
        transport->poll(0);
        auto frame = EmsClusterProtocol::encode(messageFor(config), config);
        if (variant == 0) { frame[8] = 0xff; frame[9] = 0xff; frame[10] = 0xff; frame[11] = 0xff; }
        if (variant == 1) frame[4] ^= 0x40; // Incompatible with whichever version the shared codec emits.
        if (variant == 2) frame.back() ^= 1;
        writeAll(client, frame);
        const auto before = fdCount();
        require(transport->poll(0).empty(), "malformed/version/auth frame delivered");
        require(fdCount() == before - 1, "malformed/version/auth connection retained");
    }
}
void incompatibleIdentity() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket udp(SOCK_DGRAM);
    for (int variant = 0; variant < 4; ++variant) {
        auto message = messageFor(config);
        if (variant == 0) ++message.clusterIdHash;
        if (variant == 1) ++message.configHash;
        if (variant == 2) message.senderNodeId.clear();
        if (variant == 3) message.senderNodeId = "local";
        const auto frame = EmsClusterProtocol::encode(message, config);
        udpWrite(udp, config.discoveryPort, frame);
        require(transport->poll(0).empty(), "incompatible UDP identity accepted");
        Socket tcp(SOCK_STREAM);
        tcpConnect(tcp, config.tcpPort);
        transport->poll(0);
        writeAll(tcp, frame);
        require(transport->poll(0).empty(), "incompatible TCP identity accepted");
    }
}
void writerDeadlines() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket udp(SOCK_DGRAM);
    udpWrite(udp, config.discoveryPort, EmsClusterProtocol::encode(messageFor(config), config));
    transport->poll(0);
    Socket client(SOCK_STREAM);
    tcpConnect(client, config.tcpPort);
    transport->poll(0);
    writeAll(client, EmsClusterProtocol::encode(messageFor(config), config));
    require(transport->poll(0).size() == 1, "identify writer peer");
    EmsClusterOutbound outbound;
    outbound.message = messageFor(config, "local");
    outbound.targetNodeId = "peer";
    resetCounters();
    const int before = fdCount();
    writeMode = WriteMode::Trickle;
    const bool sent = transport->send(outbound);
    writeMode = WriteMode::Real;
    require(!sent, "repeated writable progress renewed send deadline");
    require(writeCalls < 100 && maxWait <= 20, "send work/wait budget exceeded");
    require(fdCount() == before - 1, "timed-out partial writer retained");
    udpWrite(udp, config.discoveryPort, EmsClusterProtocol::encode(messageFor(config), config));
    transport->poll(0);
    resetCounters();
    writeMode = WriteMode::ConnectTimeout;
    const bool connected = transport->send(outbound);
    writeMode = WriteMode::Real;
    require(!connected && maxWait > 0 && maxWait <= 5, "connect wait exceeds absolute 5ms budget");
}
void twoPeerRecovery() {
    auto aConfig = configFor();
    auto bConfig = aConfig;
    bConfig.tcpPort = freePort(SOCK_STREAM);
    bConfig.discoveryPort = freePort(SOCK_DGRAM);
    auto a = makeEthernetClusterTransport(aConfig, "a");
    auto b = makeEthernetClusterTransport(bConfig, "b");
    a->start(); b->start();
    Socket udp(SOCK_DGRAM);
    for (int round = 0; round < 2; ++round) {
        udpWrite(udp, bConfig.discoveryPort, EmsClusterProtocol::encode(messageFor(aConfig, "a"), aConfig));
        require(b->poll(0).size() == 1, "reverse peer rediscovery");
        udpWrite(udp, aConfig.discoveryPort, EmsClusterProtocol::encode(messageFor(bConfig, "b"), bConfig));
        require(a->poll(0).size() == 1, "peer rediscovery");
        EmsClusterOutbound outbound;
        outbound.message = messageFor(aConfig, "a");
        outbound.targetNodeId = "b";
        require(a->send(outbound), "valid peer send");
        b->poll(0);
        require(b->poll(20).size() == 1, "valid peer receive");
        outbound.message = messageFor(bConfig, "b");
        outbound.targetNodeId = "a";
        require(b->send(outbound), "valid reverse send");
        require(a->poll(20).size() == 1, "valid reverse receive");
        b->stop(); a->poll(0); b->start();
    }
}
}

int main() {
    alarm(18);
    int failed = 0;
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"udp-budget", udpBudget}, {"accept-pending-budget-expiry", acceptAndPendingBudget},
        {"tcp-budget-partial", tcpBudgetAndPartial}, {"malformed-version-auth", malformedAndVersion},
        {"incompatible-identity", incompatibleIdentity}, {"writer-deadlines", writerDeadlines},
        {"two-peer-recovery", twoPeerRecovery}
    };
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << std::endl; }
        catch (const std::exception& error) {
            measure = false; writeMode = WriteMode::Real;
            ++failed; std::cout << "FAIL " << test.first << ": " << error.what() << std::endl;
        }
    }
    return failed == 0 ? 0 : 1;
}
