#define main upstream_driver_test_main
#include "cluster_driver_send_guard_test.cpp"
#undef main

void mixedBatch(const std::string& mode, bool daemonPath) {
    Fixture f;
    Peer peer;
    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = peer.port;
    tcp.timeoutMs = 200;
    auto client = std::make_shared<ModbusTcpClient>(tcp);
    f.config.mqttDriver.powerControlOwnershipFile = "batch-owner-" + mode;
    f.config.mqttDriver.priorityControlLeaseFile = "batch-priority-" + mode;
    if (mode == "expired") f.command.clusterAuthorization->notAfterMonotonicMs = clusterMonotonicNowMs() - 1;
    if (mode == "missing") f.command.clusterAuthorization = NullOpt;
    if (mode == "missing-store") shm_unlink(("/" + f.name).c_str());
    if (mode == "epoch") {
        f.snapshot.authorization->authorityEpoch.fill(3);
        f.store.publishClusterAuthority(f.snapshot);
    }
    auto ordinary = f.command;
    ordinary.cmdId = "ordinary-after-rejection";
    ordinary.source = "manual";
    ordinary.clusterAuthorization = NullOpt;
    f.store.submitWriteCommand(f.command);
    f.store.submitWriteCommand(ordinary);
    sentBytes = 0;
    if (daemonPath) {
        GatewayDaemon daemon(f.config, f.store, client);
        require(daemon.processWritebackOnce(1000) == 1, "daemon must continue after cluster rejection");
        const auto rejected = f.store.getWritebackResult(f.command.cmdId);
        const auto accepted = f.store.getWritebackResult(ordinary.cmdId);
        require(rejected && !rejected->success && accepted && accepted->success,
                "daemon must retain both rejection and following ordinary result");
    } else {
        CommandExecutor executor(f.config, f.store, client);
        WritebackService service(f.store, executor);
        const auto results = service.processPendingWrites(1000);
        require(results.size() == 2 && !results[0].success && results[1].success,
                "guard rejection must remain a per-command result, not abort a drained batch");
    }
    require(sentBytes == 12, "only the following ordinary command may send TCP bytes");
    std::cout << (daemonPath ? "daemon " : "service ") << mode
              << " rejected-first/accepted-second bytes=" << sentBytes << '\n';
}

void ownership() {
    Fixture f;
    Peer peer;
    TcpTransportConfig tcp;
    tcp.host = "127.0.0.1";
    tcp.port = peer.port;
    tcp.timeoutMs = 200;
    auto client = std::make_shared<ModbusTcpClient>(tcp);
    f.config.mqttDriver.powerControlOwnershipFile = "mqtt-owner";
    f.config.mqttDriver.priorityControlLeaseFile = "mqtt-priority";
    PowerControlOwnership owner("mqtt-owner", "mqtt-forwarder");
    const auto acquired = owner.acquireOrRenew("power", "session", {1234}, "acquire", 1000, 30000);
    require(acquired.accepted && acquired.generation != 0, "acquire MQTT ownership");
    auto command = f.command;
    command.clusterAuthorization = NullOpt;
    command.source = "mqtt-forwarder";
    command.cmdId = "mqtt-stale-generation";
    command.controlGeneration = acquired.generation + 1;
    f.store.submitWriteCommand(command);
    command.cmdId = "mqtt-current-generation";
    command.controlGeneration = acquired.generation;
    f.store.submitWriteCommand(command);
    command.cmdId = "manual-during-mqtt-ownership";
    command.source = "manual";
    command.controlGeneration = 0;
    f.store.submitWriteCommand(command);
    sentBytes = 0;
    GatewayDaemon daemon(f.config, f.store, client);
    require(daemon.processWritebackOnce(1001) == 1 && sentBytes == 12, "only current MQTT owner may send");
    require(!f.store.getWritebackResult("mqtt-stale-generation")->success &&
            f.store.getWritebackResult("mqtt-current-generation")->success &&
            !f.store.getWritebackResult("manual-during-mqtt-ownership")->success,
            "ordinary MQTT/manual ownership arbitration must remain intact");
    owner.releaseAndAdvance("session");
    command.cmdId = "mqtt-after-release";
    command.source = "mqtt-forwarder";
    command.controlGeneration = acquired.generation;
    f.store.submitWriteCommand(command);
    require(daemon.processWritebackOnce(1002) == 0 && sentBytes == 12 &&
            !f.store.getWritebackResult(command.cmdId)->success, "released MQTT generation must remain rejected");
    std::cout << "MQTT ownership current/stale/manual/released passed bytes=" << sentBytes << '\n';
}

int main() {
    try {
        for (bool daemon : {false, true}) {
            for (const auto& mode : {"expired", "missing", "epoch"}) mixedBatch(mode, daemon);
        }
        mixedBatch("missing-store", false);
        ownership();
        std::cout << "driver scoped review probe passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
