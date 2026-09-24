#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"
#include "edge_gateway/writeback_service.hpp"
#include "edge_gateway/gateway_daemon.hpp"
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <functional>
#include <cerrno>
#include <algorithm>

// Test-thread syscall injection; the loopback peer always uses real I/O.
thread_local std::function<void()> afterReady;
thread_local std::function<void()> afterSend;
thread_local std::size_t sendLimit = 0;
thread_local std::size_t sentBytes = 0;
thread_local bool interruptSend = false;
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    if (interruptSend) {
        interruptSend = false;
        if (afterSend) { auto action = std::move(afterSend); action(); }
        errno = EINTR;
        return -1;
    }
    const auto n = __real_send(fd, data, sendLimit ? std::min(size, sendLimit) : size, flags);
    if (n > 0) {
        sentBytes += n;
        if (afterSend) { auto action = std::move(afterSend); action(); }
    }
    return n;
}
extern "C" int __real_select(int, fd_set*, fd_set*, fd_set*, timeval*);
extern "C" int __wrap_select(int n, fd_set* r, fd_set* w, fd_set* e, timeval* t) {
    const auto rc = __real_select(n, r, w, e, t);
    if (rc > 0 && w && afterReady) { auto action = std::move(afterReady); action(); }
    return rc;
}

using namespace edge_gateway;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Peer {
    int listener = -1;
    std::uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bytes{0};
    std::thread thread;
    Peer() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        require(listener >= 0, "socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        socklen_t size = sizeof(addr);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &size) == 0, "getsockname");
        port = ntohs(addr.sin_port);
        require(listen(listener, 4) == 0, "listen");
        thread = std::thread([this] {
            while (!stop) {
                pollfd p{listener, POLLIN, 0};
                if (poll(&p, 1, 10) <= 0) continue;
                const int fd = accept(listener, nullptr, nullptr);
                if (fd < 0) continue;
                std::vector<std::uint8_t> frame;
                while (!stop) {
                    pollfd q{fd, POLLIN, 0};
                    if (poll(&q, 1, 10) <= 0) continue;
                    std::uint8_t buffer[260];
                    const auto n = recv(fd, buffer, sizeof(buffer), 0);
                    if (n <= 0) break;
                    bytes += n;
                    frame.insert(frame.end(), buffer, buffer + n);
                    if (frame.size() == 12) {
                        (void)send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
                        frame.clear();
                    }
                }
                close(fd);
            }
        });
    }
    ~Peer() { stop = true; thread.join(); close(listener); }
};

struct Fixture {
    std::string name = "driver_guard_" + std::to_string(getpid());
    MemoryPointStore store{name};
    DeviceConfig config;
    PendingWriteCommand command;
    ClusterAuthoritySnapshot snapshot;
    Fixture() {
        config.machineCode = "test";
        config.meterCode = "meter";
        config.protocol.type = "modbus_tcp";
        config.protocol.slave = 1;
        config.emsCluster.enabled = config.emsCluster.controlEnabled = true;
        config.emsCluster.virtualSharedMemoryName = name;
        config.emsCluster.controlTargetIndexes = {1234};
        PointDefinition point;
        point.index = 1234;
        point.pointCode = "power";
        point.address = 0;
        point.write.enable = true;
        point.write.function = 6;
        point.write.dataType = "uint16";
        point.write.verifyAfterWrite = false;
        config.points = {point};
        command.cmdId = "cluster-send";
        command.index = 1234;
        command.value = 18;
        command.source = "graph-ems";
        ClusterWriteAuthorization auth;
        auth.kernelBootId = localKernelBootId();
        auth.authorityEpoch.fill(2);
        auth.authorityStoreName = name;
        auth.dispatchSequence = 3;
        auth.notAfterMonotonicMs = clusterMonotonicNowMs() + 60000;
        command.clusterAuthorization = auth;
        snapshot.valid = true;
        snapshot.authorization = auth;
        snapshot.targetIndexes = {1234};
        store.publishClusterAuthority(snapshot);
    }
    ~Fixture() { shm_unlink(("/" + name).c_str()); }
};

void queueCase(const std::string& mode) {
    Fixture f;
    Peer peer;
    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = peer.port;
    tcp.timeoutMs = 200;
    auto client = std::make_shared<ModbusTcpClient>(tcp);
    CommandExecutor executor(f.config, f.store, client);
    WritebackService service(f.store, executor);
    const auto revoke = [&] { f.snapshot.valid = false; f.store.publishClusterAuthority(f.snapshot); };
    if (mode == "expired" || mode == "daemon")
        f.command.clusterAuthorization->notAfterMonotonicMs = clusterMonotonicNowMs() - 1;
    if (mode == "revoked") revoke();
    if (mode == "epoch") {
        f.snapshot.authorization->authorityEpoch.fill(3);
        f.store.publishClusterAuthority(f.snapshot);
    }
    if (mode == "ordinary" || mode == "missing") f.command.clusterAuthorization = NullOpt;
    if (mode == "ordinary") f.command.source = "mqtt-forwarder";
    if (mode == "after-wait") {
        client->writeSingleRegister(1, 0, 1);
        afterReady = revoke;
    }
    sentBytes = 0;
    if (mode == "partial") { sendLimit = 4; afterSend = revoke; }
    if (mode == "eintr") { interruptSend = true; afterSend = revoke; }
    f.store.submitWriteCommand(f.command);
    bool success = false;
    if (mode == "daemon") {
        f.config.mqttDriver.powerControlOwnershipFile = "/tmp/driver-guard-owner";
        f.config.mqttDriver.priorityControlLeaseFile = "/tmp/driver-guard-priority";
        GatewayDaemon daemon(f.config, f.store, client);
        daemon.processWritebackOnce(1);
        const auto receipt = f.store.getWritebackResult(f.command.cmdId);
        require(static_cast<bool>(receipt), "daemon receipt missing");
        success = receipt->success;
    } else {
        const auto result = service.processPendingWrites(1);
        require(result.size() == 1, "one result");
        success = result[0].success;
    }
    const bool allowed = mode == "ordinary" || mode == "valid";
    const std::size_t expected = allowed ? 12 : mode == "partial" ? 4 : 0;
    std::cout << mode << " success=" << success << " sentBytes=" << sentBytes << '\n';
    require(sentBytes == expected, "unexpected physical TCP bytes after authorization change");
    require(success == allowed, "incorrect queue result");
    afterReady = afterSend = nullptr;
    sendLimit = 0;
    interruptSend = false;
    // The same client must recover without leaking the previous command's guard.
    client->writeSingleRegister(1, 0, 1);
}
}
int main(int argc, char** argv) {
    try {
        if (argc > 1) queueCase(argv[1]);
        else for (const auto& mode : {"expired", "revoked", "epoch", "missing", "valid", "ordinary",
                                     "after-wait", "partial", "eintr", "daemon"}) queueCase(mode);
        std::cout << "cluster_driver_send_guard_test passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
