#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/command_executor.hpp"
#include "edge_gateway/modbus_tcp_client.hpp"
#include "edge_gateway/writeback_service.hpp"
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

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

void expiredQueue() {
    Fixture f;
    Peer peer;
    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = peer.port;
    tcp.timeoutMs = 200;
    auto client = std::make_shared<ModbusTcpClient>(tcp);
    CommandExecutor executor(f.config, f.store, client);
    WritebackService service(f.store, executor);
    f.command.clusterAuthorization->notAfterMonotonicMs = clusterMonotonicNowMs() - 1;
    f.store.submitWriteCommand(f.command);
    const auto result = service.processPendingWrites(1);
    std::cout << "expired-queue result=" << result.at(0).success << " bytes=" << peer.bytes << '\n';
    require(peer.bytes == 0, "expired queued command reached physical TCP send");
    require(result.size() == 1 && !result[0].success, "expired queue must fail");
}
}
int main() {
    try { expiredQueue(); std::cout << "cluster_driver_send_guard_test passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
