#include "edge_gateway/cluster_write_authorization.hpp"
#include <iostream>
#include <stdexcept>
using namespace edge_gateway;
void require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
int main() {
    try {
        EmsClusterConfig config;
        config.enabled = config.controlEnabled = true;
        config.virtualSharedMemoryName = "authority_guard_test";
        config.controlTargetIndexes = {1234};
        ClusterWriteAuthorization auth;
        auth.authorityStoreName = config.virtualSharedMemoryName;
        auth.kernelBootId.fill(1);
        auth.authorityEpoch.fill(2);
        auth.dispatchSequence = 3;
        auth.notAfterMonotonicMs = 1500;
        PendingWriteCommand command;
        command.index = 1234;
        command.source = "graph-ems";
        command.value = 18;
        command.clusterAuthorization = auth;
        ClusterAuthoritySnapshot snapshot;
        snapshot.valid = true;
        snapshot.authorization = auth;
        snapshot.targetIndexes = {1234};
        require(clusterAuthorizationValid(config, command, snapshot, auth.kernelBootId, 1499), "valid before deadline");
        require(!clusterAuthorizationValid(config, command, snapshot, auth.kernelBootId, 1500), "reject exact deadline");
        require(!clusterAuthorizationValid(config, command, snapshot, auth.kernelBootId, 1501), "reject after deadline");
        snapshot.authorization->notAfterMonotonicMs = 5000;
        require(!clusterAuthorizationValid(config, command, snapshot, auth.kernelBootId, 1501), "renewal must not renew queued command");
        for (int wrong = 0; wrong < 8; ++wrong) {
            auto changed = snapshot;
            if (wrong == 0) changed.valid = false;
            if (wrong == 1) changed.authorization->kernelBootId.fill(3);
            if (wrong == 2) changed.authorization->authorityEpoch.fill(4);
            if (wrong == 3) changed.authorization->dispatchSequence = 4;
            if (wrong == 4) changed.authorization->authorityStoreName = "other";
            if (wrong == 5) changed.targetIndexes = {4321};
            if (wrong == 6) changed.authorization->version = 2;
            if (wrong == 7) changed.authorization->flags = 1;
            require(!clusterAuthorizationValid(config, command, changed, auth.kernelBootId, 1499), "reject changed context");
        }
        command.highPriority = true;
        command.value = 0;
        require(!clusterAuthorizationValid(config, command, snapshot, auth.kernelBootId, 1500), "zero and priority do not bypass expiry");
        command.clusterAuthorization = NullOpt;
        require(clusterAuthorizationRequired(config, command), "protected graph output needs metadata");
        config.controlTargetIndexes.clear();
        require(clusterAuthorizationRequired(config, command), "empty control scope is all-stop, not all-allow");
        command.source = "mqtt-forwarder";
        require(!clusterAuthorizationRequired(config, command), "ordinary MQTT remains on existing ownership policy");
        std::cout << "cluster_write_authorization_test passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
