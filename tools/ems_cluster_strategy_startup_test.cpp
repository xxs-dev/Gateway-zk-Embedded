#include "edge_gateway/cluster_write_authorization.hpp"
#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/graph_ems_engine.hpp"
#include <cstdio>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace edge_gateway;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void indexes(GraphEmsNodeConfig& node, const std::string& key, std::uint32_t first) {
    node.params[key + ".count"] = "3";
    for (int i = 0; i < 3; ++i) node.params[key + "." + std::to_string(i)] = std::to_string(first + i);
}
void startup(bool testRecovery = false) {
    const auto name = std::string(testRecovery ? "cluster_recovery_" : "cluster_startup_") + std::to_string(getpid());
    struct Cleanup {
        std::string name;
        ~Cleanup() {
            shm_unlink(("/" + name).c_str());
            for (int i = 0; i < 2; ++i) {
                std::remove((name + std::to_string(i) + "-consensus.json").c_str());
                std::remove((name + std::to_string(i) + "-members.json").c_str());
            }
        }
    } cleanup{name};
    EmsClusterConfig config;
    config.enabled = config.controlEnabled = true;
    config.clusterId = name;
    config.clusterInterface = "lo";
    config.securityMode = "psk";
    config.psk = "startup-test-key-only";
    config.expectedMembers = config.maxMembers = config.minimumQuorum = 2;
    config.discoveryIntervalMs = 200;
    config.heartbeatMs = config.statusIntervalMs = config.dispatchCycleMs = 100;
    config.leaderLeaseMs = config.dispatchTtlMs = config.capabilityTtlMs = 400;
    config.electionTimeoutMinMs = 500;
    config.electionTimeoutMaxMs = 800;
    config.memberTimeoutMs = 1200;
    config.virtualSharedMemoryName = name;
    config.controlTargetIndexes = {1234};
    std::vector<std::unique_ptr<EmsClusterNode>> nodes;
    std::vector<EmsClusterConfig> nodeConfigs;
    for (int i = 0; i < 2; ++i) {
        auto local = config;
        local.consensusStateFile = name + std::to_string(i) + "-consensus.json";
        local.membershipFile = name + std::to_string(i) + "-members.json";
        {
            std::ofstream roster(local.membershipFile);
            roster << "{\"schemaVersion\":\"1.0\",\"clusterId\":\"" << name
                   << "\",\"membershipEpoch\":1,\"assignments\":["
                      "{\"nodeId\":\"NODE_0\",\"cabinetNo\":1},"
                      "{\"nodeId\":\"NODE_1\",\"cabinetNo\":2}]}";
            require(static_cast<bool>(roster), "fixed voter fixture must be writable");
        }
        local.electionPriority = i == 0 ? 100 : 0;
        nodeConfigs.push_back(local);
        nodes.emplace_back(new EmsClusterNode(local, "NODE_" + std::to_string(i), "BOOT_" + std::to_string(i)));
    }
    EmsClusterLoadSample load;
    load.computeMetricsAvailable = load.computeHealthy = true;
    EmsClusterCapability capability;
    capability.controlEnabled = capability.ready = true;
    capability.interlocked = false;
    capability.socPercent = 50;
    capability.ratedActivePowerKw = capability.availableChargePowerKw = capability.availableDischargePowerKw = 90;
    capability.ratedApparentPowerKva = 100;
    capability.availableReactivePowerKvar = 60;
    auto now = clusterMonotonicNowMs() - 5000;
    const auto deliver = [&] {
        for (int delivery = 0; delivery < 3; ++delivery) {
            for (int i = 0; i < 2; ++i) {
                for (const auto& outbound : nodes[i]->drainOutgoing()) {
                    if (!outbound.targetNodeId.empty() && outbound.targetNodeId != nodes[1-i]->nodeId()) continue;
                    EmsClusterInbound inbound;
                    inbound.message = outbound.message;
                    inbound.sourceAddress = "127.0.0." + std::to_string(i + 1);
                    nodes[1-i]->receive(inbound, now);
                }
            }
        }
    };
    for (int tick = 0; tick < 200; ++tick) {
        now += 25;
        for (auto& node : nodes) {
            node->updateControlInputs(capability, {}, false, now);
            node->tick(now, load);
        }
        deliver();
    }
    EmsClusterNode* leader = nullptr;
    for (auto& node : nodes) if (node->role() == EmsClusterRole::Leader) leader = node.get();
    require(leader && leader->status(now).quorumValid, "startup fixture requires an elected leased leader");
    require(!leader->activeDispatch(now).valid, "no station target must mean no initial dispatch");
    EmsClusterPointBridge bridge(config, "test");
    MemoryPointStore store(name, MemoryStoreOpenMode::OpenExisting);
    PointStoreRouter router;
    router.addStore(name, store);
    addEmsClusterPointRoutes(router, config, "test");
    const auto base = config.virtualPointBaseIndex;
    for (std::uint32_t i = 100; i < 111; ++i) {
        PointStoreRoute route;
        route.index = base + i;
        route.sharedMemoryName = name;
        router.addRoute(route);
    }
    constexpr std::int64_t wall = 1000000;
    PointValue enable;
    enable.index = base + ems_cluster_point::kEnable;
    enable.value = enable.quality = 1;
    enable.ts = wall;
    enable.expireAt = wall + 10000;
    store.putLatest(enable);
    GraphEmsNodeConfig gate;
    gate.id = "cluster";
    gate.type = "clusterDispatch";
    gate.params = {{"enableIndex", std::to_string(base)}, {"roleIndex", std::to_string(base + 1)},
        {"quorumIndex", std::to_string(base + 5)}, {"dispatchValidIndex", std::to_string(base + 50)},
        {"dispatchReasonIndex", std::to_string(base + 51)}, {"stationStrategyActiveIndex", std::to_string(base + 52)},
        {"validOutputIndex", std::to_string(base + 106)}, {"stationLeaderOutputIndex", std::to_string(base + 107)},
        {"reasonOutputIndex", std::to_string(base + 108)}};
    indexes(gate, "dispatchActiveIndexes", base + 30);
    indexes(gate, "dispatchReactiveIndexes", base + 33);
    indexes(gate, "activeOutputIndexes", base + 100);
    indexes(gate, "reactiveOutputIndexes", base + 103);
    GraphEmsConfig graph;
    graph.graphCode = "startup";
    graph.nodes.push_back(gate);
    for (int phase = 0; phase < 6; ++phase) {
        GraphEmsNodeConfig strategy;
        strategy.id = "strategy-" + std::to_string(phase);
        strategy.type = "switch";
        strategy.params = {{"conditionIndex", std::to_string(base + 107)}, {"trueValue", "12"},
            {"falseValue", "0"}, {"outputIndex", std::to_string(base + 60 + phase)}};
        graph.nodes.push_back(strategy);
        graph.edges.push_back({gate.id, strategy.id});
    }
    GraphEmsEngine engine(graph, router, 1000);
    bool targetValid = false;
    bridge.sampleStationTarget(wall, targetValid);
    require(!targetValid, "initial station targets must be absent");
    bridge.publish(leader->status(now), leader->activeDispatch(now), wall, now);
    const auto firstRun = engine.runOnce(wall + 1);
    require(firstRun.errors.empty(), "startup strategy graph must execute");
    const auto target = bridge.sampleStationTarget(wall + 1, targetValid);
    require(targetValid && target.paKw == 12 && target.qcKvar == 12,
            "four-argument bridge must let Graph calculate first target with no initial dispatch");
    now += 25;
    leader->updateControlInputs(capability, target, targetValid, now);
    leader->tick(now, load);
    const auto dispatch = leader->activeDispatch(now);
    require(dispatch.valid && dispatch.accepted.paKw > 0, "first calculated station target must produce dispatch");
    bridge.publish(leader->status(now), dispatch, wall + 25, now);
    require(store.clusterAuthority()->valid && store.clusterAuthority()->authorization,
            "only the resulting dispatch may grant physical write authority");
    require(engine.runOnce(wall + 26).errors.empty(), "Graph must consume the first dispatch");
    require(store.getLatestByIndex(base + 106, wall + 26)->value == 1,
            "closed loop must expose dispatch validity");
    require(router.peekPendingWrites().empty(), "strategy calculation must not enqueue physical writes");
    if (testRecovery) {
        // Keep the elected node, bridge and Graph alive; restart only the other Coordinator.
        const int followerIndex = nodes[0].get() == leader ? 1 : 0;
        std::int64_t recoveryWall = wall + 25;
        for (int tick = 0; tick < 80; ++tick) {
            now += 25;
            recoveryWall += 25;
            for (auto& node : nodes) {
                node->updateControlInputs(capability, target, true, now);
                node->tick(now, load);
            }
            deliver();
            bridge.publish(leader->status(now), leader->activeDispatch(now), recoveryWall, now);
        }
        const auto beforeLoss = leader->activeDispatch(now);
        require(beforeLoss.valid && beforeLoss.sequence > 10 && store.clusterAuthority()->valid,
                "recovery fixture requires a live high-sequence dispatch");
        PendingWriteCommand oldCommand;
        oldCommand.index = 1234;
        oldCommand.value = beforeLoss.accepted.paKw;
        oldCommand.clusterAuthorization = store.clusterAuthority()->authorization;
        nodes[followerIndex].reset();
        bool lostQuorum = false;
        for (int tick = 0; tick < 160; ++tick) {
            now += 25;
            recoveryWall += 25;
            leader->updateControlInputs(capability, {}, false, now);
            leader->tick(now, load);
            leader->drainOutgoing();
            const auto status = leader->status(now);
            bridge.publish(status, leader->activeDispatch(now), recoveryWall, now);
            if (!status.quorumValid) {
                lostQuorum = true;
                require(!store.clusterAuthority()->valid, "quorum loss must revoke the bridge snapshot");
                require(engine.runOnce(recoveryWall).errors.empty() &&
                        store.getLatestByIndex(base + 106, recoveryWall)->value == 0 &&
                        store.getLatestByIndex(base + 107, recoveryWall)->value == 0,
                        "Graph must clear dispatch and strategy gates during quorum loss");
            }
            if (status.role == EmsClusterRole::Candidate && status.term > beforeLoss.term) break;
        }
        require(lostQuorum && leader->role() == EmsClusterRole::Candidate &&
                leader->activeDispatch(now).term == beforeLoss.term &&
                leader->activeDispatch(now).sequence == beforeLoss.sequence,
                "same node self-election must retain the old term high-sequence dispatch");
        nodes[followerIndex].reset(new EmsClusterNode(nodeConfigs[followerIndex],
            "NODE_" + std::to_string(followerIndex), "RESTARTED_" + std::to_string(followerIndex)));
        bool recoveredLeader = false;
        for (int tick = 0; tick < 200; ++tick) {
            now += 25;
            recoveryWall += 25;
            for (auto& node : nodes) {
                node->updateControlInputs(capability, {}, false, now);
                node->tick(now, load);
            }
            deliver();
            const auto status = leader->status(now);
            const auto transition = leader->activeDispatch(now);
            bridge.publish(status, transition, recoveryWall, now);
            if (status.role == EmsClusterRole::Leader && status.quorumValid) {
                require(status.term > beforeLoss.term && !transition.valid &&
                        transition.term == beforeLoss.term && transition.sequence == beforeLoss.sequence,
                        "new leader status must precede replacement of the old dispatch");
                require(!store.clusterAuthority()->valid && store.clusterAuthority()->stationStrategyActive,
                        "re-elected leader may compute but must not execute old dispatch");
                recoveredLeader = true;
                break;
            }
        }
        require(recoveredLeader, "surviving Coordinator must regain two-voter quorum without restart");
        enable.ts = recoveryWall;
        enable.expireAt = recoveryWall + 1000;
        store.putLatest(enable);
        require(engine.runOnce(recoveryWall).errors.empty(), "same Graph must resume station strategy");
        const auto recoveredTarget = bridge.sampleStationTarget(recoveryWall, targetValid);
        require(targetValid && recoveredTarget.paKw == 12 && recoveredTarget.qcKvar == 12,
                "same Graph must generate the first new-term target");
        now += 25;
        recoveryWall += 25;
        leader->updateControlInputs(capability, recoveredTarget, true, now);
        leader->tick(now, load);
        const auto recoveredDispatch = leader->activeDispatch(now);
        require(recoveredDispatch.valid && recoveredDispatch.term > beforeLoss.term && recoveredDispatch.sequence == 1,
                "core must accept the first new-term dispatch at sequence 1");
        bridge.publish(leader->status(now), recoveredDispatch, recoveryWall, now);
        const auto recoveredSnapshot = store.clusterAuthority();
        require(recoveredSnapshot && recoveredSnapshot->valid && recoveredSnapshot->authorization,
                "same bridge must authorize the first low-sequence dispatch after real re-election");
        require(!clusterAuthorizationValid(config, oldCommand, *recoveredSnapshot, localKernelBootId(), now),
                "real re-election must reject queued old-term authorization");
        require(engine.runOnce(recoveryWall).errors.empty() &&
                store.getLatestByIndex(base + 106, recoveryWall)->value == 1 &&
                store.getLatestByIndex(base + 107, recoveryWall)->value == 1 &&
                store.getLatestByIndex(base + 100, recoveryWall)->value == recoveredDispatch.accepted.paKw &&
                store.getLatestByIndex(base + 103, recoveryWall)->value == recoveredDispatch.accepted.qaKvar,
                "same Graph must recover valid P/Q sinks on the first dispatch, not after sequence catch-up");
        require(router.peekPendingWrites().empty(), "recovery Graph must remain virtual-only");
        std::cout << "same-process recovery term " << beforeLoss.term << " seq " << beforeLoss.sequence
                  << " -> term " << recoveredDispatch.term << " seq " << recoveredDispatch.sequence << '\n';
        return;
    }
    PointStoreRoute physical;
    physical.index = 1234;
    physical.sharedMemoryName = name;
    physical.writable = true;
    router.addRoute(physical);
    GraphEmsNodeConfig multiply;
    multiply.id = "multiply";
    multiply.type = "formula";
    multiply.params = {{"operation", "multiply"}, {"inputs.count", "2"},
        {"inputs.0.index", std::to_string(base + 100)}, {"inputs.1.value", "2"},
        {"outputIndex", std::to_string(base + 109)}};
    GraphEmsNodeConfig limit;
    limit.id = "limit";
    limit.type = "formula";
    limit.params = {{"operation", "clamp"}, {"inputs.count", "1"},
        {"inputs.0.index", std::to_string(base + 109)}, {"lower", "-5"}, {"upper", "5"},
        {"outputIndex", std::to_string(base + 110)}};
    GraphEmsNodeConfig writer;
    writer.id = "writer";
    writer.type = "controlWrite";
    writer.params = {{"submitWrites", "true"}, {"inputIndex", std::to_string(base + 110)},
        {"targetIndex", "1234"}, {"minValue", "-100"}, {"maxValue", "100"}};
    GraphEmsConfig control;
    control.graphCode = "lineage";
    control.nodes = {gate, multiply, limit, writer};
    control.edges = {{gate.id, multiply.id}, {multiply.id, limit.id}, {limit.id, writer.id}};
    GraphEmsEngine controlEngine(control, router, 1000);
    const auto controlRun = controlEngine.runOnce(wall + 26);
    require(controlRun.errors.empty() && controlRun.deviceWrites == 1,
            "authorized phase through arithmetic and clamp must enqueue one command");
    auto queued = store.drainPendingWriteCommands();
    require(queued.size() == 1 && queued[0].value == 5 && queued[0].clusterAuthorization &&
            queued[0].clusterAuthorization->notAfterMonotonicMs == store.clusterAuthority()->authorization->notAfterMonotonicMs,
            "Graph must preserve immutable input authorization through intermediate nodes and queue");
    const auto live = *store.clusterAuthority();
    auto staleWriter = writer;
    staleWriter.params["permitIndex"] = std::to_string(base + 106);
    GraphEmsConfig stale;
    stale.graphCode = "stale-lineage";
    stale.nodes = {gate, staleWriter};
    stale.edges = {{gate.id, staleWriter.id}};
    GraphEmsEngine staleEngine(stale, router, 1000);
    require(!staleEngine.runOnce(wall + 27).errors.empty() && store.peekPendingWriteCommands().empty(),
            "fresh permit must not authorize a previous scan's scalar target");
    auto mixedFormula = multiply;
    mixedFormula.params.erase("inputs.1.value");
    mixedFormula.params["inputs.1.index"] = std::to_string(base + 110);
    auto mixedWriter = writer;
    mixedWriter.params["inputIndex"] = std::to_string(base + 109);
    GraphEmsConfig mixed;
    mixed.graphCode = "mixed-lineage";
    mixed.nodes = {gate, mixedFormula, mixedWriter};
    mixed.edges = {{gate.id, mixedFormula.id}, {mixedFormula.id, mixedWriter.id}};
    GraphEmsEngine mixedEngine(mixed, router, 1000);
    require(!mixedEngine.runOnce(wall + 27).errors.empty() && store.peekPendingWriteCommands().empty(),
            "arithmetic mixing a fresh dispatch with a scalar of unknown lineage must fail closed");
    ClusterWriteGuard sendGuard(config);
    std::vector<double> actuator;
    const auto send = [&](const PendingWriteCommand& command) {
        sendGuard.check(command);
        actuator.push_back(command.value);
    };
    send(queued[0]);
    require(actuator.size() == 1, "live queued command must reach fake actuator");
    auto revoked = live;
    revoked.valid = false;
    store.publishClusterAuthority(revoked);
    bool rejected = false;
    try { send(queued[0]); } catch (const std::exception&) { rejected = true; }
    require(rejected && actuator.size() == 1, "revocation between batch sends must block the next actuator call");
    store.publishClusterAuthority(live);
    auto changed = live;
    changed.strategyKernelBootId[0] ^= 1;
    store.publishClusterAuthority(changed);
    require(engine.runOnce(wall + 27).errors.empty(), "boot mismatch must fail closed without crashing Graph");
    require(store.getLatestByIndex(base + 107, wall + 27)->value == 0,
            "Graph must reject strategy authority from a different kernel boot");
    changed = live;
    changed.strategyNotAfterMonotonicMs = clusterMonotonicNowMs();
    store.publishClusterAuthority(changed);
    require(engine.runOnce(wall + 28).errors.empty(), "expired strategy must fail closed without crashing Graph");
    require(store.getLatestByIndex(base + 107, wall + 28)->value == 0,
            "Graph must check monotonic strategy deadline even with fresh wall-clock diagnostics");
    store.publishClusterAuthority(live);
    require(engine.runOnce(wall + 29).errors.empty() && store.getLatestByIndex(base + 107, wall + 29)->value == 1,
            "fixture must have an active strategy gate before authority-store failure");
    auto missingStore = config;
    missingStore.virtualSharedMemoryName = name + "_missing";
    router.setEmsClusterConfig(missingStore);
    require(!engine.runOnce(wall + 29).errors.empty(), "missing authority store must be reported");
    require(store.getLatestByIndex(base + 107, wall + 29)->value == 0,
            "authority read failure must clear a previous strategy gate, not retain its scalar value");
    router.setEmsClusterConfig(config);
    EmsClusterPointBridge restarted(config, "test");
    require(engine.runOnce(wall + 29).errors.empty(), "restart-cleared authority must fail closed");
    require(store.getLatestByIndex(base + 107, wall + 29)->value == 0 &&
            store.getLatestByIndex(base + 106, wall + 29)->value == 0,
            "fresh scalar diagnostics must not revive either gate after publisher restart");
    store.publishClusterAuthority(live);
    const auto deadline = queued[0].clusterAuthorization->notAfterMonotonicMs;
    const auto remaining = deadline - clusterMonotonicNowMs();
    if (remaining > 0) std::this_thread::sleep_for(std::chrono::milliseconds(remaining));
    rejected = false;
    try { send(queued[0]); } catch (const std::exception&) { rejected = true; }
    require(rejected && actuator.size() == 1, "delayed drain after a stalled coordinator must not reach actuator");
    require(!controlEngine.runOnce(wall + 30).errors.empty() && store.peekPendingWriteCommands().empty(),
            "fresh wall timestamps must not refresh expired Graph command authority");
}
}
int main() {
    try { startup(); startup(true); std::cout << "ems_cluster_strategy_startup_test passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
