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
int wouldBlock = 0;
std::size_t readBytes = 0;
enum class WriteMode { Real, Trickle, ConnectTimeout };
WriteMode writeMode = WriteMode::Real;
void resetCounters() { accepts = reads = datagrams = writeCalls = maxWait = wouldBlock = 0; readBytes = 0; }
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
bool injectWritablePoll(pollfd* fds, nfds_t count, int timeout, int& result) {
    if (count != 1 || fds[0].events != POLLOUT) return false;
    if (measure || writeMode != WriteMode::Real) maxWait = std::max(maxWait, timeout);
    if (writeMode == WriteMode::Real) return false;
    result = 0;
    if (writeMode == WriteMode::Trickle) {
        fds[0].revents = POLLOUT;
        result = 1;
    }
    return true;
}
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
#ifdef __GLIBC__
// Optimized fortified builds may bypass recv/recvfrom via their checked entry points.
ssize_t __real___recv_chk(int, void*, size_t, size_t, int);
ssize_t __real___recvfrom_chk(int, void*, size_t, size_t, int, sockaddr*, socklen_t*);
ssize_t __wrap___recv_chk(int fd, void* buffer, size_t size, size_t capacity, int flags) {
    const auto result = __real___recv_chk(fd, buffer, size, capacity, flags);
    if (measure) { ++reads; if (result > 0) readBytes += result; }
    return result;
}
ssize_t __wrap___recvfrom_chk(int fd, void* buffer, size_t size, size_t capacity, int flags,
                            sockaddr* address, socklen_t* length) {
    if (measure) ++datagrams;
    return __real___recvfrom_chk(fd, buffer, size, capacity, flags, address, length);
}
#endif
ssize_t __wrap_send(int fd, const void* buffer, size_t size, int flags) {
    if (writeMode == WriteMode::Trickle) {
        ++writeCalls;
        if (writeCalls % 2 == 0) { errno = EAGAIN; return -1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::min(size, size_t(1));
    }
    const auto result = __real_send(fd, buffer, size, flags);
    if (measure && result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++wouldBlock;
    return result;
}
int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    int result = 0;
    if (injectWritablePoll(fds, count, timeout, result)) return result;
    return __real_poll(fds, count, timeout);
}
#ifdef __GLIBC__
int __real___poll_chk(pollfd*, nfds_t, int, size_t);
int __wrap___poll_chk(pollfd* fds, nfds_t count, int timeout, size_t capacity) {
    // Preserve glibc's bounds failure even when the fixture injects writability.
    if (count > capacity / sizeof(*fds)) return __real___poll_chk(fds, count, timeout, capacity);
    int result = 0;
    if (injectWritablePoll(fds, count, timeout, result)) return result;
    return __real___poll_chk(fds, count, timeout, capacity);
}
#endif
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
EmsClusterMessage messageFor(const EmsClusterConfig& config, const std::string& peer = "peer",
                             std::uint64_t configurationHash = 0) {
    EmsClusterMessage message;
    message.clusterIdHash = EmsClusterProtocol::clusterIdHash(config.clusterId);
    message.configHash = configurationHash != 0 ? configurationHash : EmsClusterProtocol::configHash(config);
    message.senderNodeId = peer;
    message.senderBootId = "transport-test-os-boot";
    message.senderIncarnation = "transport-test-process-" + peer;
    message.discoveryChallenge = 1;
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
std::vector<EmsClusterInbound> measuredPoll(IClusterTransport& transport, int timeoutMs = 0) {
    resetCounters();
    measure = true;
    auto result = transport.poll(timeoutMs);
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
    for (int variant = 0; variant < 6; ++variant) {
        auto message = messageFor(config);
        if (variant == 0) ++message.clusterIdHash;
        if (variant == 1) ++message.configHash;
        if (variant == 2) message.senderNodeId.clear();
        if (variant == 3) message.tcpPort = 0;
        auto encodingConfig = config;
        if (variant == 4) encodingConfig.psk = "wrong-authentication-key";
        if (variant == 5) encodingConfig.securityMode = "none";
        const auto frame = EmsClusterProtocol::encode(message, encodingConfig);
        udpWrite(udp, config.discoveryPort, frame);
        require(transport->poll(0).empty(), "incompatible UDP identity accepted");
        Socket tcp(SOCK_STREAM);
        tcpConnect(tcp, config.tcpPort);
        transport->poll(0);
        writeAll(tcp, frame);
        require(transport->poll(0).empty(), "incompatible TCP identity accepted");
    }
}
void duplicateLocalIdentitySignal() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket udp(SOCK_DGRAM);
    auto duplicate = messageFor(config, "local");
    duplicate.senderIncarnation = "different-process-incarnation";
    const auto frame = EmsClusterProtocol::encode(duplicate, config);
    for (int i = 0; i < 12; ++i) udpWrite(udp, config.discoveryPort, frame);
    const auto received = measuredPoll(*transport);
    require(received.size() == 4 && datagrams == 4, "duplicate-local UDP signal missing or unbounded");
    require(received.front().message.senderIncarnation == duplicate.senderIncarnation &&
            received.front().message.senderBootId == duplicate.senderBootId, "duplicate incarnation changed");
    for (int i = 0; i < 4; ++i) transport->poll(0);
    Socket client(SOCK_STREAM);
    tcpConnect(client, config.tcpPort);
    transport->poll(0);
    const int before = fdCount();
    writeAll(client, frame);
    require(transport->poll(0).size() == 1, "duplicate-local TCP signal missing");
    require(fdCount() == before - 1, "duplicate-local TCP connection retained");
    EmsClusterOutbound outbound;
    outbound.message = messageFor(config);
    outbound.targetNodeId = "local";
    require(!transport->send(outbound), "self identity created an endpoint/peer mapping");
    Socket peer(SOCK_STREAM);
    tcpConnect(peer, config.tcpPort);
    transport->poll(0);
    writeAll(peer, EmsClusterProtocol::encode(messageFor(config), config));
    require(transport->poll(0).size() == 1, "remote peer identification");
    writeAll(peer, frame);
    require(transport->poll(0).empty(), "identified peer switched sender to local identity");
}
void endpointBoundAndStreamIdentity() {
    for (int members : {2, 5}) {
        auto config = configFor(members);
        auto transport = makeEthernetClusterTransport(config, "local");
        transport->start();
        Socket udp(SOCK_DGRAM);
        std::size_t accepted = 0;
        for (int i = 0; i < members + 3; ++i) {
            auto message = messageFor(config, "peer" + std::to_string(i));
            udpWrite(udp, config.discoveryPort, EmsClusterProtocol::encode(message, config));
            accepted += transport->poll(0).size();
        }
        require(accepted == static_cast<std::size_t>(members - 1), "endpoint table exceeds remote-member capacity");
        Socket tcp(SOCK_STREAM);
        tcpConnect(tcp, config.tcpPort);
        transport->poll(0);
        writeAll(tcp, EmsClusterProtocol::encode(messageFor(config, "peer0"), config));
        require(transport->poll(0).size() == 1, "known endpoint blocked by identity flood");
        const int before = fdCount();
        writeAll(tcp, EmsClusterProtocol::encode(messageFor(config, "different-peer"), config));
        require(transport->poll(0).empty(), "stream sender identity changed");
        require(fdCount() == before - 1, "identity-switch connection retained");
    }
}
void largeFrameAndSlowPartial() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket client(SOCK_STREAM);
    tcpConnect(client, config.tcpPort);
    transport->poll(0);
    auto frame = EmsClusterProtocol::encode(messageFor(config), config);
    frame.resize(65536, 0);
    const auto payloadSize = static_cast<uint32_t>(frame.size() - 16 - 32);
    for (int i = 0; i < 4; ++i) frame[8 + i] = static_cast<uint8_t>(payloadSize >> (24 - i * 8));
    writeAll(client, frame);
    const int before = fdCount();
    std::size_t totalBytes = 0;
    for (int i = 0; i < 10 && fdCount() == before; ++i) {
        require(measuredPoll(*transport, 50).empty(), "unauthenticated large frame delivered");
        require(readBytes <= 16384, "per-connection byte budget exceeded");
        totalBytes += readBytes;
    }
    if (totalBytes != 65536 || fdCount() != before - 1) {
        throw std::runtime_error("large malformed frame: bytes=" + std::to_string(totalBytes) +
            " fd_delta=" + std::to_string(fdCount() - before));
    }
    Socket slow(SOCK_STREAM);
    tcpConnect(slow, config.tcpPort);
    transport->poll(0);
    frame = EmsClusterProtocol::encode(messageFor(config), config);
    writeAll(slow, frame);
    require(transport->poll(0).size() == 1, "slow peer identification");
    writeAll(slow, std::vector<uint8_t>(frame.begin(), frame.begin() + 9));
    std::size_t partialBytes = 0;
    for (int i = 0; i < 5 && partialBytes < 9; ++i) {
        require(measuredPoll(*transport, 50).empty(), "partial frame delivered");
        partialBytes += readBytes;
    }
    require(partialBytes == 9, "fixture partial bytes not received");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    writeAll(slow, std::vector<uint8_t>(frame.begin() + 9, frame.begin() + 10));
    transport->poll(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int partialFds = fdCount();
    transport->poll(0);
    require(fdCount() == partialFds - 1, "trickle bytes renewed absolute partial-frame deadline");
}
void realStalledWriter() {
    auto config = configFor();
    auto transport = makeEthernetClusterTransport(config, "local");
    transport->start();
    Socket client(SOCK_STREAM);
    const int small = 1024;
    require(setsockopt(client.fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0, "small receive window");
    tcpConnect(client, config.tcpPort);
    transport->poll(0);
    writeAll(client, EmsClusterProtocol::encode(messageFor(config), config));
    require(transport->poll(0).size() == 1, "stalled peer identification");
    EmsClusterOutbound outbound;
    outbound.message = messageFor(config, "local");
    outbound.message.senderBootId.assign(1024, 'b');
    outbound.targetNodeId = "peer";
    resetCounters();
    measure = true;
    bool failed = false;
    const int before = fdCount();
    for (int i = 0; i < 128; ++i) {
        if (!transport->send(outbound)) { failed = true; break; }
    }
    measure = false;
    require(failed && wouldBlock > 0, "kernel backpressure did not fail bounded writer");
    require(maxWait <= 20 && fdCount() == before - 1, "stalled writer wait/cleanup bound failed");
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
    auto a = makeEthernetClusterTransport(aConfig, "a");
    a->start();
    auto bConfig = aConfig;
    bConfig.tcpPort = freePort(SOCK_STREAM);
    bConfig.discoveryPort = freePort(SOCK_DGRAM);
    auto b = makeEthernetClusterTransport(bConfig, "b");
    b->start();
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
void suppliedConfigurationHash() {
    auto aConfig = configFor();
    const auto baseHash = EmsClusterProtocol::configHash(aConfig);
    auto suppliedHash = baseHash + 1;
    if (suppliedHash == 0) suppliedHash = 1;
    auto a = makeEthernetClusterTransport(aConfig, "a", suppliedHash);
    a->start();
    auto bConfig = aConfig;
    bConfig.tcpPort = freePort(SOCK_STREAM);
    bConfig.discoveryPort = freePort(SOCK_DGRAM);
    auto b = makeEthernetClusterTransport(bConfig, "b", suppliedHash);
    b->start();
    Socket udp(SOCK_DGRAM);
    udpWrite(udp, aConfig.discoveryPort, EmsClusterProtocol::encode(messageFor(bConfig, "b"), bConfig));
    require(a->poll(20).empty(), "base configuration hash admitted by supplied-hash transport");
    EmsClusterOutbound outbound;
    outbound.message = messageFor(aConfig, "a", suppliedHash);
    outbound.targetNodeId = "b";
    require(!a->send(outbound), "mismatched discovery learned an endpoint");
    udpWrite(udp, aConfig.discoveryPort, EmsClusterProtocol::encode(messageFor(bConfig, "b", suppliedHash), bConfig));
    require(a->poll(20).size() == 1, "supplied-hash discovery rejected");
    require(a->send(outbound), "supplied-hash forward send");
    b->poll(20);
    const auto forward = b->poll(20);
    require(forward.size() == 1 && forward.front().message.configHash == suppliedHash,
            "supplied-hash forward receive");
    outbound.message = messageFor(bConfig, "b", suppliedHash);
    outbound.targetNodeId = "a";
    require(b->send(outbound), "supplied-hash reverse send");
    const auto reverse = a->poll(20);
    require(reverse.size() == 1 && reverse.front().message.configHash == suppliedHash,
            "supplied-hash reverse receive");
    outbound.message.configHash = baseHash;
    const int before = fdCount();
    require(b->send(outbound), "mismatched TCP frame fixture send");
    require(a->poll(50).empty(), "mismatched TCP configuration hash admitted");
    require(fdCount() == before - 1, "mismatched TCP configuration retained its stream");
}
}

int main(int argc, char** argv) {
    alarm(18);
    int failed = 0;
    int selected = 0;
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"udp-budget", udpBudget}, {"accept-pending-budget-expiry", acceptAndPendingBudget},
        {"tcp-budget-partial", tcpBudgetAndPartial}, {"malformed-version-auth", malformedAndVersion},
        {"incompatible-identity", incompatibleIdentity}, {"writer-deadlines", writerDeadlines},
        {"endpoint-bound-stream-identity", endpointBoundAndStreamIdentity},
        {"duplicate-local-identity-signal", duplicateLocalIdentitySignal},
        {"large-frame-slow-partial", largeFrameAndSlowPartial}, {"real-stalled-writer", realStalledWriter},
        {"two-peer-recovery", twoPeerRecovery}, {"supplied-configuration-hash", suppliedConfigurationHash}
    };
    for (const auto& test : tests) {
        if (argc > 1 && std::string(argv[1]) != test.first) continue;
        ++selected;
        try { test.second(); std::cout << "PASS " << test.first << std::endl; }
        catch (const std::exception& error) {
            measure = false; writeMode = WriteMode::Real;
            ++failed; std::cout << "FAIL " << test.first << ": " << error.what() << std::endl;
        }
    }
    return failed == 0 && selected != 0 ? 0 : 1;
}
