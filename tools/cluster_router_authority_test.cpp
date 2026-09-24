#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/point_store_router.hpp"
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
using namespace edge_gateway;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    const auto name = "cluster_router_" + std::to_string(getpid());
    struct Cleanup {
        std::string name;
        ~Cleanup() { shm_unlink(("/"+name).c_str()); shm_unlink(("/"+name+"_device").c_str()); }
    } cleanup{name};
    try {
        MemoryPointStore authority(name), device(name+"_device");
        EmsClusterConfig config;
        config.enabled = config.controlEnabled = true;
        config.virtualSharedMemoryName = name;
        config.controlTargetIndexes = {1, 2, 3, 4};
        PointStoreRouter router;
        router.setEmsClusterConfig(config);
        router.addStore(name+"_device", device);
        for (std::uint32_t index : {1, 2}) {
            PointStoreRoute route;
            route.index = index;
            route.sharedMemoryName = name+"_device";
            route.writable = true;
            router.addRoute(route);
        }
        PendingWriteCommand command;
        command.index = 1;
        command.value = 0;
        command.highPriority = true;
        command.source = "graph-ems";
        require(!router.submitWriteCommand(command).accepted, "router must reject untagged protected zero/priority command");
        ClusterWriteAuthorization token;
        token.kernelBootId = localKernelBootId();
        token.authorityEpoch = newClusterAuthorityEpoch();
        token.authorityStoreName = name;
        token.dispatchSequence = 7;
        token.notAfterMonotonicMs = clusterMonotonicNowMs() + 10000;
        ClusterAuthoritySnapshot snapshot;
        snapshot.valid = true;
        snapshot.authorization = token;
        snapshot.targetIndexes = config.controlTargetIndexes;
        authority.publishClusterAuthority(snapshot);
        command.clusterAuthorization = token;
        command.controlGeneration = 42;
        require(router.submitWriteCommand(command).accepted, "valid command must route to separate physical store");
        auto drained = device.drainPendingWriteCommands();
        require(drained.size() == 1 && drained[0].clusterAuthorization && drained[0].controlGeneration == 42 &&
                drained[0].clusterAuthorization->notAfterMonotonicMs == token.notAfterMonotonicMs,
                "queue round trip must preserve immutable token and independent ownership generation");
        snapshot.valid = false;
        authority.publishClusterAuthority(snapshot);
        require(!router.submitWriteCommand(command).accepted, "router must reread revocation before submit");
        command.source = "mqtt-forwarder";
        require(!router.submitWriteCommand(command).accepted, "changing source cannot bypass an attached token");
        command.clusterAuthorization = NullOpt;
        require(router.submitWriteCommand(command).accepted, "ordinary MQTT policy must remain independent");
        device.drainPendingWriteCommands();
        snapshot.valid = true;
        authority.publishClusterAuthority(snapshot);
        command.clusterAuthorization = token;
        auto other = command;
        other.index = 2;
        other.clusterAuthorization->notAfterMonotonicMs = clusterMonotonicNowMs();
        require(!router.submitWriteCommands({command, other}).accepted && device.peekPendingWriteCommands().empty(),
                "batch with one expired token must enqueue nothing");
        PointStoreRouter unconfigured;
        unconfigured.addStore(name+"_device", device);
        unconfigured.addRoute(*router.routeByIndex(1));
        require(!unconfigured.submitWriteCommand(command).accepted, "unconfigured router must reject tagged commands");
        auto mailbox = *router.routeByIndex(1);
        mailbox.index = 3;
        mailbox.commandMailbox = true;
        router.addRoute(mailbox);
        command.index = 3;
        require(!router.submitCommandMailbox(command).accepted, "mailbox must not strip authorization into a scalar");
        auto parameter = *router.routeByIndex(1);
        parameter.index = 4;
        parameter.protocolType = "ems_virtual";
        parameter.write.enable = true;
        router.addRoute(parameter);
        command.index = 4;
        require(!router.submitWriteCommand(command).accepted && !device.getLatestByIndex(4, 1),
                "virtual parameter must not strip an otherwise valid token into a retained scalar");
        config.controlTargetIndexes.clear();
        router.setEmsClusterConfig(config);
        command.index = 1;
        command.source = "graph-ems";
        command.clusterAuthorization = NullOpt;
        require(!router.submitWriteCommand(command).accepted, "empty configured scope must block untagged graph control");
        std::cout << "cluster_router_authority_test passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
