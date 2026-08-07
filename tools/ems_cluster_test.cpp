#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/ems_cluster_network.hpp"
#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

edge_gateway::EmsClusterConfig configFor(const std::string& name, int expectedMembers) {
    edge_gateway::EmsClusterConfig config;
    config.enabled = true;
    config.clusterId = "TEST_CLUSTER_" + name;
    config.transport = "ethernet";
    config.clusterInterface = "lo";
    config.ipMode = "static";
    config.staticAddress = "127.0.0.1";
    config.securityMode = "psk";
    config.psk = "cluster-test-key-1234567890";
    config.expectedMembers = expectedMembers;
    config.maxMembers = std::max(5, expectedMembers);
    config.minimumQuorum = expectedMembers / 2 + 1;
    config.discoveryIntervalMs = 200;
    config.heartbeatMs = 100;
    config.leaderLeaseMs = 400;
    config.electionTimeoutMinMs = 500;
    config.electionTimeoutMaxMs = 800;
    config.memberTimeoutMs = 1200;
    config.statusIntervalMs = 100;
    return config;
}

struct SimNode {
    std::string id;
    edge_gateway::EmsClusterConfig config;
    std::unique_ptr<edge_gateway::EmsClusterNode> node;
    edge_gateway::EmsClusterLoadSample load;
    edge_gateway::EmsClusterCapability capability;
    edge_gateway::EmsClusterPhasePower stationTarget;
    bool stationTargetValid = false;
    bool refreshControlInputs = true;
    bool active = true;
};

class Simulation {
public:
    Simulation(
        std::string name,
        int count,
        std::vector<int> lockedCabinetNumbers = {},
        int expectedMembers = 0,
        bool controlEnabled = false
    )
        : name_(std::move(name)) {
        for (int i = 0; i < count; ++i) {
            SimNode item;
            item.id = "COMM_TEST_" + std::to_string(i + 1);
            item.config = configFor(name_, expectedMembers > 0 ? expectedMembers : count);
            item.config.controlEnabled = controlEnabled;
            item.config.dispatchCycleMs = 100;
            item.config.dispatchTtlMs = 400;
            item.config.capabilityTtlMs = 400;
            item.config.consensusStateFile = tempPath(item.id + "-consensus.json");
            item.config.membershipFile = tempPath(item.id + "-membership.json");
            item.config.electionPriority = i == 0 ? 100 : 0;
            if (static_cast<std::size_t>(i) < lockedCabinetNumbers.size()) {
                item.config.lockedCabinetNo = lockedCabinetNumbers[static_cast<std::size_t>(i)];
            }
            item.load.cpuPercent = i == 0 ? 1.0 : 90.0;
            item.load.memoryPercent = i == 0 ? 10.0 : 80.0;
            item.load.computeTimeoutPercent = i == 0 ? 0.0 : 80.0;
            item.load.controlQueueP95Ms = i == 0 ? 1.0 : 800.0;
            item.load.computeMetricsAvailable = i == 0;
            item.load.computeHealthy = true;
            item.capability.controlEnabled = controlEnabled;
            item.capability.ready = controlEnabled;
            item.capability.interlocked = !controlEnabled;
            item.capability.socPercent = 50.0;
            item.capability.ratedActivePowerKw = 90.0;
            item.capability.ratedApparentPowerKva = 100.0;
            item.capability.availableChargePowerKw = 90.0;
            item.capability.availableDischargePowerKw = 90.0;
            item.capability.availableReactivePowerKvar = 60.0;
            item.node.reset(new edge_gateway::EmsClusterNode(item.config, item.id, "BOOT_" + item.id));
            nodes_.push_back(std::move(item));
        }
        links_.assign(count, std::vector<bool>(count, true));
    }

    ~Simulation() {
        for (const auto& node : nodes_) {
            std::remove(node.config.consensusStateFile.c_str());
            std::remove(node.config.membershipFile.c_str());
        }
    }

    void run(int durationMs) {
        const auto end = nowMs_ + durationMs;
        while (nowMs_ < end) {
            nowMs_ += 25;
            for (auto& item : nodes_) {
                if (!item.active) continue;
                if (item.refreshControlInputs) {
                    item.node->updateControlInputs(
                        item.capability,
                        item.stationTarget,
                        item.stationTargetValid,
                        nowMs_
                    );
                }
                item.node->tick(nowMs_, item.load);
            }
            deliver();
            deliver();
        }
    }

    void partition(const std::vector<int>& first, const std::vector<int>& second) {
        for (const auto lhs : first) for (const auto rhs : second) links_[lhs][rhs] = links_[rhs][lhs] = false;
    }

    int leaderCount() const {
        return static_cast<int>(std::count_if(nodes_.begin(), nodes_.end(), [](const auto& item) {
            return item.active && item.node->role() == edge_gateway::EmsClusterRole::Leader;
        }));
    }

    int leaderIndex() const {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].active && nodes_[i].node->role() == edge_gateway::EmsClusterRole::Leader) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    SimNode& at(int index) { return nodes_.at(static_cast<std::size_t>(index)); }
    const SimNode& at(int index) const { return nodes_.at(static_cast<std::size_t>(index)); }
    std::int64_t now() const { return nowMs_; }

private:
    std::string tempPath(const std::string& suffix) const {
        return "ems-cluster-test-" + name_ + "-" + suffix;
    }

    void deliver() {
        struct Envelope { int source; edge_gateway::EmsClusterOutbound outbound; };
        std::vector<Envelope> messages;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (!nodes_[i].active) continue;
            for (auto& outbound : nodes_[i].node->drainOutgoing()) {
                messages.push_back({static_cast<int>(i), std::move(outbound)});
            }
        }
        for (const auto& envelope : messages) {
            for (std::size_t target = 0; target < nodes_.size(); ++target) {
                if (static_cast<int>(target) == envelope.source || !nodes_[target].active ||
                    !links_[envelope.source][target]) continue;
                if (!envelope.outbound.targetNodeId.empty() &&
                    envelope.outbound.targetNodeId != nodes_[target].id) continue;
                edge_gateway::EmsClusterInbound inbound;
                inbound.message = envelope.outbound.message;
                inbound.sourceAddress = "169.254.1." + std::to_string(envelope.source + 1);
                nodes_[target].node->receive(inbound, nowMs_);
            }
        }
    }

    std::string name_;
    std::int64_t nowMs_ = 1000;
    std::vector<SimNode> nodes_;
    std::vector<std::vector<bool>> links_;
};

void testProtocolAuthentication() {
    auto config = configFor("protocol", 2);
    edge_gateway::EmsClusterMessage message;
    message.type = edge_gateway::EmsClusterMessageType::Heartbeat;
    message.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(config.clusterId);
    message.configHash = edge_gateway::EmsClusterProtocol::configHash(config);
    message.term = 7;
    message.sequence = 12;
    message.senderNodeId = "COMM_A";
    message.senderBootId = "BOOT_A";
    message.loadScore = 12.5;
    message.lockedCabinetNo = 2;
    message.dispatchSequence = 99;
    message.dispatchTtlMs = 3000;
    message.dispatchCode = edge_gateway::EmsClusterDispatchCode::Clamped;
    message.capability.socPercent = 63.5;
    message.capability.ready = true;
    message.requestedPower.paKw = 12.5;
    message.acceptedPower.paKw = 10.0;
    message.assignments = {{"COMM_A", 1}, {"COMM_B", 2}};
    const auto frame = edge_gateway::EmsClusterProtocol::encode(message, config);
    const auto decoded = edge_gateway::EmsClusterProtocol::decode(frame.data(), frame.size(), config);
    require(decoded.term == 7 && decoded.lockedCabinetNo == 2 && decoded.assignments.size() == 2 &&
            decoded.dispatchSequence == 99 &&
            decoded.dispatchCode == edge_gateway::EmsClusterDispatchCode::Clamped &&
            decoded.capability.ready && decoded.capability.socPercent == 63.5 &&
            decoded.requestedPower.paKw == 12.5 && decoded.acceptedPower.paKw == 10.0,
            "authenticated KECP/1 frame must round-trip");
    auto wrong = config;
    wrong.psk = "different-test-key-123456";
    bool rejected = false;
    try { edge_gateway::EmsClusterProtocol::decode(frame.data(), frame.size(), wrong); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "KECP/1 frame signed with another PSK must be rejected");

    auto nonFinite = message;
    nonFinite.capability.socPercent = std::numeric_limits<double>::quiet_NaN();
    rejected = false;
    try {
        const auto invalidFrame = edge_gateway::EmsClusterProtocol::encode(nonFinite, config);
        edge_gateway::EmsClusterProtocol::decode(invalidFrame.data(), invalidFrame.size(), config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "KECP/1 decoder must reject non-finite capability and control values");
}

void testThreeNodeElectionAndMembership() {
    Simulation simulation("three", 3);
    simulation.run(5000);
    require(simulation.leaderCount() == 1, "three-node cluster must elect exactly one leader");
    require(simulation.leaderIndex() == 0, "healthy low-load node should win a new election");
    for (int i = 0; i < 3; ++i) {
        const auto status = simulation.at(i).node->status(simulation.now());
        require(status.membershipEpoch > 0, "membership must be majority committed");
        require(status.cabinetNo > 0, "every member must receive a stable cabinet number");
        require(status.quorumValid, "healthy three-node cluster must hold quorum");
    }
}

void testLeaderPartitionFencesMinority() {
    Simulation simulation("partition", 3);
    simulation.run(4000);
    const auto oldLeader = simulation.leaderIndex();
    require(oldLeader >= 0, "partition test requires an initial leader");
    std::vector<int> majority;
    for (int i = 0; i < 3; ++i) if (i != oldLeader) majority.push_back(i);
    simulation.partition({oldLeader}, majority);
    simulation.run(4000);
    require(simulation.at(oldLeader).node->role() != edge_gateway::EmsClusterRole::Leader,
            "isolated old leader must lose its majority lease");
    require(simulation.leaderCount() == 1, "majority partition must elect one replacement leader");
}

void testTwoNodePartitionStopsBoth() {
    Simulation simulation("two", 2);
    simulation.run(3500);
    require(simulation.leaderCount() == 1, "connected two-node cluster must elect one leader");
    simulation.partition({0}, {1});
    simulation.run(3000);
    require(simulation.leaderCount() == 0, "both nodes must leave leader state after a two-node split");
}

void testFiveNodeFormation() {
    Simulation simulation("five", 5);
    simulation.run(6000);
    require(simulation.leaderCount() == 1, "five-node cluster must elect exactly one leader");
    const auto leader = simulation.leaderIndex();
    require(simulation.at(leader).node->status(simulation.now()).onlineMembers == 5,
            "five-node leader must observe all members");
}

void testQuorumExpandsWithCommittedMembership() {
    Simulation simulation("dynamic-quorum", 5, {}, 2);
    simulation.run(6000);
    require(simulation.leaderCount() == 1, "expanded cluster must elect exactly one leader");
    const auto oldLeader = simulation.leaderIndex();
    const auto before = simulation.at(oldLeader).node->status(simulation.now());
    require(before.membershipEpoch > 0 && before.quorum == 3,
            "five committed members must require three votes even when expectedMembers started at two");
    std::vector<int> majority;
    std::vector<int> minority{oldLeader};
    for (int i = 0; i < 5; ++i) {
        if (i == oldLeader) continue;
        if (minority.size() < 2) minority.push_back(i);
        else majority.push_back(i);
    }
    simulation.partition(minority, majority);
    simulation.run(4000);
    require(simulation.at(oldLeader).node->role() != edge_gateway::EmsClusterRole::Leader,
            "two-node minority must lose leadership after membership expands to five");
    require(simulation.leaderCount() == 1, "three-node majority must retain exactly one leader");
}

void testLockedCabinetNumbersAreHonored() {
    Simulation simulation("locked", 3, {0, 3, 2});
    simulation.run(5000);
    require(simulation.leaderCount() == 1, "locked-cabinet test must elect one leader");
    require(simulation.at(0).node->status(simulation.now()).cabinetNo == 1,
            "unlocked member should receive the remaining lowest cabinet number");
    require(simulation.at(1).node->status(simulation.now()).cabinetNo == 3,
            "remote locked cabinet number must be propagated to the leader");
    require(simulation.at(2).node->status(simulation.now()).cabinetNo == 2,
            "every remote locked cabinet number must be committed unchanged");
}

void testDuplicateLockedCabinetNumberIsRejected() {
    Simulation simulation("duplicate-lock", 3, {1, 1, 0});
    simulation.run(4000);
    require(simulation.leaderCount() == 1, "duplicate lock test still requires a consensus leader");
    const auto status = simulation.at(simulation.leaderIndex()).node->status(simulation.now());
    require(status.membershipEpoch == 0, "conflicting locked cabinet numbers must not be committed");
    require(status.reason.find("duplicate locked cabinet number") != std::string::npos,
            "locked cabinet conflict must remain visible in cluster status");
}

void testStaleUncommittedMemberIsNotNumbered() {
    Simulation simulation("stale-member", 3);
    simulation.at(0).load.computeHealthy = false;
    simulation.at(1).load.computeHealthy = false;
    simulation.run(150);
    simulation.at(2).active = false;
    simulation.run(1500);
    simulation.at(0).load.computeHealthy = true;
    simulation.at(1).load.computeHealthy = true;
    simulation.run(4000);
    require(simulation.leaderCount() == 1, "two live nodes still form the configured majority");
    const auto status = simulation.at(simulation.leaderIndex()).node->status(simulation.now());
    const auto stale = std::find_if(status.members.begin(), status.members.end(), [](const auto& member) {
        return member.nodeId == "COMM_TEST_3";
    });
    require(stale != status.members.end(), "stale member remains observable for diagnostics");
    require(stale->cabinetNo == 0, "stale uncommitted member must not consume a cabinet number");
}

void testRestartKeepsTermAndCabinetNumber() {
    Simulation simulation("restart", 3);
    simulation.run(5000);
    auto& member = simulation.at(1);
    const auto before = member.node->status(simulation.now());
    const auto term = member.node->currentTerm();
    member.node.reset(new edge_gateway::EmsClusterNode(member.config, member.id, "BOOT_" + member.id));
    const auto after = member.node->status(simulation.now());
    require(member.node->currentTerm() >= term, "restart must not decrease persisted term");
    require(after.cabinetNo == before.cabinetNo, "restart must retain committed cabinet number");
}

void testStateFromAnotherClusterIsIgnored() {
    auto config = configFor("state-isolation", 2);
    config.consensusStateFile = "ems-cluster-test-state-isolation-consensus.json";
    config.membershipFile = "ems-cluster-test-state-isolation-membership.json";
    {
        std::ofstream consensus(config.consensusStateFile.c_str());
        consensus << "{\"clusterId\":\"OLD_CLUSTER\",\"term\":99,\"votedFor\":\"OLD_NODE\"}";
        std::ofstream membership(config.membershipFile.c_str());
        membership << "{\"clusterId\":\"OLD_CLUSTER\",\"membershipEpoch\":9,"
                      "\"assignments\":[{\"nodeId\":\"COMM_NEW\",\"cabinetNo\":4}]}";
    }
    edge_gateway::EmsClusterNode node(config, "COMM_NEW", "BOOT_NEW");
    require(node.currentTerm() == 0, "consensus state from another cluster must be ignored");
    require(node.status(1000).cabinetNo == 0, "membership from another cluster must be ignored");
    std::remove(config.consensusStateFile.c_str());
    std::remove(config.membershipFile.c_str());
}

void testDuplicateMachineCodeQuarantinesNode() {
    auto config = configFor("duplicate", 2);
    config.consensusStateFile = "ems-cluster-test-duplicate-consensus.json";
    config.membershipFile = "ems-cluster-test-duplicate-membership.json";
    edge_gateway::EmsClusterNode node(config, "COMM_DUP", "BOOT_LOCAL");
    edge_gateway::EmsClusterInbound inbound;
    inbound.message.type = edge_gateway::EmsClusterMessageType::Discover;
    inbound.message.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(config.clusterId);
    inbound.message.configHash = edge_gateway::EmsClusterProtocol::configHash(config);
    inbound.message.senderNodeId = "COMM_DUP";
    inbound.message.senderBootId = "BOOT_OTHER_DEVICE";
    inbound.message.sequence = 1;
    node.receive(inbound, 1000);
    require(node.role() == edge_gateway::EmsClusterRole::Quarantined,
            "same machineCode from another boot identity must quarantine the node");
    std::remove(config.consensusStateFile.c_str());
    std::remove(config.membershipFile.c_str());
}

void testConfigAndAddressValidation() {
    auto config = configFor("validation", 2);
    config.minimumQuorum = 1;
    bool rejected = false;
    try { edge_gateway::EmsClusterNode::validateConfig(config); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "unsafe quorum lower than majority must be rejected");
    const auto first = edge_gateway::deriveEmsClusterLinkLocalAddress("COMM_A", "00:11:22:33:44:55");
    const auto second = edge_gateway::deriveEmsClusterLinkLocalAddress("COMM_A", "00:11:22:33:44:55");
    require(first == second && first.rfind("169.254.", 0) == 0, "Link-Local candidate must be stable");
    require(first.find(".0") == std::string::npos && first.find(".255") == std::string::npos,
            "Link-Local candidate must avoid reserved host octets");
}

void testMissingComputeMetricsArePenalized() {
    edge_gateway::EmsClusterLoadSample complete;
    complete.cpuPercent = 10;
    complete.memoryPercent = 10;
    complete.computeTimeoutPercent = 0;
    complete.controlQueueP95Ms = 0;
    complete.computeMetricsAvailable = true;
    complete.computeHealthy = true;
    edge_gateway::EmsClusterLoadSample missing = complete;
    missing.computeMetricsAvailable = false;
    require(edge_gateway::EmsClusterNode::calculateLoadScore(missing) >
            edge_gateway::EmsClusterNode::calculateLoadScore(complete) + 30.0,
            "missing compute metrics must not be treated as zero load");
}

void testCapabilityWeightedDispatchAllocation() {
    std::map<std::string, edge_gateway::EmsClusterCapability> capabilities;
    for (int i = 0; i < 3; ++i) {
        edge_gateway::EmsClusterCapability capability;
        capability.controlEnabled = true;
        capability.ready = true;
        capability.interlocked = false;
        capability.socPercent = i == 0 ? 90.0 : (i == 1 ? 50.0 : 20.0);
        capability.ratedActivePowerKw = 90.0;
        capability.ratedApparentPowerKva = 100.0;
        capability.availableChargePowerKw = 90.0;
        capability.availableDischargePowerKw = 90.0;
        capability.availableReactivePowerKvar = 60.0;
        capabilities.emplace("N" + std::to_string(i + 1), capability);
    }
    edge_gateway::EmsClusterPhasePower target;
    target.paKw = 45.0;
    target.pbKw = 30.0;
    target.pcKw = 15.0;
    target.qaKvar = 15.0;
    const auto allocated = edge_gateway::EmsClusterNode::allocateDispatch(target, capabilities);
    double pa = 0.0;
    double pb = 0.0;
    double pc = 0.0;
    double qa = 0.0;
    for (const auto& entry : allocated) {
        pa += entry.second.paKw;
        pb += entry.second.pbKw;
        pc += entry.second.pcKw;
        qa += entry.second.qaKvar;
    }
    require(std::fabs(pa - target.paKw) < 1e-6 && std::fabs(pb - target.pbKw) < 1e-6 &&
            std::fabs(pc - target.pcKw) < 1e-6 && std::fabs(qa - target.qaKvar) < 1e-6,
            "water-fill allocation must preserve reachable station phase targets");
    require(allocated.at("N1").paKw > allocated.at("N3").paKw,
            "higher-SOC cabinet must carry more discharge when capability is otherwise equal");

    target.paKw = -45.0;
    const auto charging = edge_gateway::EmsClusterNode::allocateDispatch(target, capabilities);
    require(std::fabs(charging.at("N3").paKw) > std::fabs(charging.at("N1").paKw),
            "lower-SOC cabinet must carry more charging when capability is otherwise equal");
}

void testDispatchClosedLoopAndInterlock() {
    Simulation simulation("dispatch", 3, {}, 0, true);
    simulation.at(0).capability.socPercent = 85.0;
    simulation.at(1).capability.socPercent = 55.0;
    simulation.at(2).capability.socPercent = 25.0;
    edge_gateway::EmsClusterPhasePower target;
    target.paKw = 30.0;
    target.pbKw = 24.0;
    target.pcKw = 18.0;
    target.qaKvar = 9.0;
    target.qbKvar = 6.0;
    target.qcKvar = 3.0;
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = target;
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(6000);
    require(simulation.leaderCount() == 1, "control-stage cluster must retain one leader");

    edge_gateway::EmsClusterPhasePower total;
    for (int i = 0; i < 3; ++i) {
        const auto dispatch = simulation.at(i).node->activeDispatch(simulation.now());
        require(dispatch.valid, "every healthy cabinet must hold a live accepted dispatch");
        total.paKw += dispatch.accepted.paKw;
        total.pbKw += dispatch.accepted.pbKw;
        total.pcKw += dispatch.accepted.pcKw;
        total.qaKvar += dispatch.accepted.qaKvar;
        total.qbKvar += dispatch.accepted.qbKvar;
        total.qcKvar += dispatch.accepted.qcKvar;
    }
    require(std::fabs(total.paKw - target.paKw) < 1e-6 &&
            std::fabs(total.pbKw - target.pbKw) < 1e-6 &&
            std::fabs(total.pcKw - target.pcKw) < 1e-6 &&
            std::fabs(total.qaKvar - target.qaKvar) < 1e-6,
            "accepted cabinet targets must sum to the station target");

    const auto leader = simulation.leaderIndex();
    const auto leaderStatus = simulation.at(leader).node->status(simulation.now());
    require(std::count_if(leaderStatus.members.begin(), leaderStatus.members.end(), [](const auto& member) {
                return member.dispatch.valid;
            }) >= 2,
            "leader must receive dispatch ACK state from both remote cabinets");

    const int interlocked = leader == 1 ? 2 : 1;
    simulation.at(interlocked).capability.interlocked = true;
    simulation.run(800);
    const auto blocked = simulation.at(interlocked).node->activeDispatch(simulation.now());
    require(!blocked.valid && blocked.code == edge_gateway::EmsClusterDispatchCode::Interlocked,
            "interlocked cabinet must reject and clear cluster dispatch");
}

void testDispatchExpiresAcrossLeaderPartition() {
    Simulation simulation("dispatch-partition", 3, {}, 0, true);
    edge_gateway::EmsClusterPhasePower target;
    target.paKw = 18.0;
    target.pbKw = 18.0;
    target.pcKw = 18.0;
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = target;
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    const auto oldLeader = simulation.leaderIndex();
    require(oldLeader >= 0 && simulation.at(oldLeader).node->activeDispatch(simulation.now()).valid,
            "partition test requires an active dispatch leader");
    std::vector<int> majority;
    for (int i = 0; i < 3; ++i) if (i != oldLeader) majority.push_back(i);
    simulation.partition({oldLeader}, majority);
    simulation.run(1500);
    const auto isolated = simulation.at(oldLeader).node->activeDispatch(simulation.now());
    require(!isolated.valid &&
            (isolated.code == edge_gateway::EmsClusterDispatchCode::NoQuorum ||
             isolated.code == edge_gateway::EmsClusterDispatchCode::NotLeader ||
             isolated.code == edge_gateway::EmsClusterDispatchCode::Expired),
            "isolated old leader must clear its local target after losing majority");
    require(simulation.leaderCount() == 1, "majority side must elect one replacement control leader");
}

void testStationTargetUsesIndependentTtl() {
    Simulation simulation("station-target-ttl", 3, {}, 0, true);
    edge_gateway::EmsClusterPhasePower target;
    target.paKw = 12.0;
    target.pbKw = 12.0;
    target.pcKw = 12.0;
    for (int i = 0; i < 3; ++i) {
        auto& node = simulation.at(i);
        node.config.stationTargetTtlMs = 200;
        node.config.capabilityTtlMs = 1000;
        node.stationTarget = target;
        node.stationTargetValid = true;
        node.node.reset(new edge_gateway::EmsClusterNode(
            node.config,
            node.id,
            "BOOT_" + node.id
        ));
    }
    simulation.run(5000);
    const auto leader = simulation.leaderIndex();
    require(leader >= 0 && simulation.at(leader).node->activeDispatch(simulation.now()).valid,
            "station-target TTL test requires an active leader dispatch");

    simulation.at(leader).refreshControlInputs = false;
    simulation.run(250);
    const auto expired = simulation.at(leader).node->activeDispatch(simulation.now());
    require(!expired.valid && expired.code == edge_gateway::EmsClusterDispatchCode::Expired,
            "station target must expire using stationTargetTtlMs while capability remains fresh");
}

void testCapabilityIsObservableBeforeControlEnable() {
    Simulation simulation("observe-capability", 3, {}, 0, false);
    simulation.run(5000);
    const auto leader = simulation.leaderIndex();
    require(leader >= 0, "read-only capability test requires a leader");
    const auto status = simulation.at(leader).node->status(simulation.now());
    require(std::count_if(status.members.begin(), status.members.end(), [](const auto& member) {
                return member.online && member.capabilityFresh && !member.capability.controlEnabled;
            }) == 2,
            "followers must report read-only capability before cluster control is enabled");
    require(!status.controlConfigured && !status.controlActive,
            "read-only capability exchange must not enable dispatch control");
}

void testClusterPointBridge() {
    auto config = configFor("point-bridge", 2);
    config.controlEnabled = true;
    config.dispatchCycleMs = 100;
    config.dispatchTtlMs = 400;
    config.capabilityTtlMs = 400;
    config.virtualSharedMemoryName = "ems_cluster_test_point_bridge";
    config.virtualPointBaseIndex = 824000;
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(config.virtualSharedMemoryName);
    {
        edge_gateway::MemoryPointStore writer(config.virtualSharedMemoryName);
        const auto put = [&](std::uint32_t offset, double value) {
            edge_gateway::PointValue point;
            point.index = config.virtualPointBaseIndex + offset;
            point.machineCode = "COMM_BRIDGE";
            point.meterCode = "EMS_CLUSTER";
            point.pointCode = "TEST_" + std::to_string(offset);
            point.value = value;
            point.quality = 1;
            point.ts = 1000;
            point.expireAt = 10000;
            writer.putLatest(point);
        };
        put(edge_gateway::ems_cluster_point::kEnable, 1);
        put(edge_gateway::ems_cluster_point::kSoc, 66);
        put(edge_gateway::ems_cluster_point::kRatedActivePower, 120);
        put(edge_gateway::ems_cluster_point::kRatedApparentPower, 130);
        put(edge_gateway::ems_cluster_point::kAvailableChargePower, 80);
        put(edge_gateway::ems_cluster_point::kAvailableDischargePower, 90);
        put(edge_gateway::ems_cluster_point::kAvailableReactivePower, 50);
        put(edge_gateway::ems_cluster_point::kControlReady, 1);
        put(edge_gateway::ems_cluster_point::kInterlocked, 0);
        put(edge_gateway::ems_cluster_point::kManualOverride, 0);
        put(edge_gateway::ems_cluster_point::kFeedbackPa, 4.5);
        for (std::uint32_t offset = edge_gateway::ems_cluster_point::kStationTargetPa;
             offset <= edge_gateway::ems_cluster_point::kStationTargetQc;
             ++offset) {
            put(offset, static_cast<double>(offset - edge_gateway::ems_cluster_point::kStationTargetPa + 1));
        }

        edge_gateway::EmsClusterPointBridge bridge(config, "COMM_BRIDGE");
        const auto capability = bridge.sampleCapability(1200);
        require(capability.controlEnabled && capability.ready && !capability.interlocked &&
                capability.socPercent == 66 && capability.actual.paKw == 4.5,
                "point bridge must read control capability from shared memory");
        bool targetValid = false;
        const auto stationTarget = bridge.sampleStationTarget(1200, targetValid);
        require(targetValid && stationTarget.paKw == 1 && stationTarget.qcKvar == 6,
                "point bridge must read all six station phase targets atomically enough for one cycle");
        bridge.sampleStationTarget(5000, targetValid);
        require(!targetValid, "point bridge must reject stale station targets");
        require(!bridge.sampleCapability(2000).ready,
                "point bridge must reject stale dynamic capability points");

        edge_gateway::EmsClusterStatus status;
        status.role = edge_gateway::EmsClusterRole::Follower;
        status.cabinetNo = 2;
        status.term = 7;
        status.onlineMembers = 2;
        status.quorumValid = true;
        status.controlConfigured = true;
        status.controlActive = true;
        edge_gateway::EmsClusterDispatchState dispatch;
        dispatch.valid = true;
        dispatch.sequence = 9;
        dispatch.code = edge_gateway::EmsClusterDispatchCode::Accepted;
        dispatch.accepted.paKw = 7.5;
        bridge.publish(status, dispatch, 2000);
        const auto published = writer.getLatestByIndex(
            config.virtualPointBaseIndex + edge_gateway::ems_cluster_point::kDispatchPa,
            2000
        );
        require(published && published->quality == 1 && published->value == 7.5,
                "point bridge must publish accepted dispatch to the cluster virtual store");
        status.role = edge_gateway::EmsClusterRole::Leader;
        status.capability.controlEnabled = true;
        status.controlActive = false;
        dispatch.valid = false;
        dispatch.code = edge_gateway::EmsClusterDispatchCode::Expired;
        bridge.publish(status, dispatch, 2100);
        const auto stationStrategy = writer.getLatestByIndex(
            config.virtualPointBaseIndex + edge_gateway::ems_cluster_point::kStationStrategyActive,
            2100
        );
        require(stationStrategy && stationStrategy->value == 1.0,
                "leader strategy gate must not depend on an already-active local dispatch");

        edge_gateway::EmsClusterMemberStatus member;
        member.nodeId = "COMM_MEMBER";
        member.capabilityFresh = true;
        member.capabilityAgeMs = 50;
        member.capability.controlEnabled = true;
        member.capability.ready = true;
        member.dispatchAgeMs = 25;
        member.dispatch.valid = true;
        member.dispatch.code = edge_gateway::EmsClusterDispatchCode::Clamped;
        status.members = {member};
        const auto statusJson = edge_gateway::emsClusterStatusJson(status, 2100);
        require(statusJson.find("\"capabilityFresh\":true") != std::string::npos &&
                statusJson.find("\"dispatchAgeMs\":25") != std::string::npos &&
                statusJson.find("\"code\":\"clamped\"") != std::string::npos,
                "cluster status JSON must expose member capability, ACK and feedback diagnostics");
        require(edge_gateway::emsClusterPointRoutes(config, "COMM_BRIDGE").size() >= 40,
                "cluster point catalog must expose status, capability, dispatch and feedback routes");
    }
    edge_gateway::MemoryPointStore::cleanupOrphanedSegment(config.virtualSharedMemoryName);
}

#ifndef _WIN32
void testEthernetTransportLoopback() {
    const auto basePort = 42000 + static_cast<int>(getpid() % 1000) * 4;
    auto firstConfig = configFor("transport", 2);
    auto secondConfig = firstConfig;
    firstConfig.discoveryPort = basePort;
    firstConfig.tcpPort = basePort + 1;
    firstConfig.seedPeers = {"127.0.0.1:" + std::to_string(basePort + 2)};
    secondConfig.discoveryPort = basePort + 2;
    secondConfig.tcpPort = basePort + 3;
    secondConfig.seedPeers = {"127.0.0.1:" + std::to_string(basePort)};
    firstConfig.consensusStateFile = "ems-cluster-test-transport-a-consensus.json";
    firstConfig.membershipFile = "ems-cluster-test-transport-a-membership.json";
    secondConfig.consensusStateFile = "ems-cluster-test-transport-b-consensus.json";
    secondConfig.membershipFile = "ems-cluster-test-transport-b-membership.json";
    edge_gateway::EmsClusterNode first(firstConfig, "COMM_NET_A", "BOOT_NET_A");
    edge_gateway::EmsClusterNode second(secondConfig, "COMM_NET_B", "BOOT_NET_B");
    auto firstTransport = edge_gateway::makeEthernetClusterTransport(firstConfig, "COMM_NET_A");
    auto secondTransport = edge_gateway::makeEthernetClusterTransport(secondConfig, "COMM_NET_B");
    firstTransport->start();
    secondTransport->start();
    edge_gateway::EmsClusterLoadSample firstLoad;
    firstLoad.cpuPercent = 5;
    firstLoad.memoryPercent = 10;
    firstLoad.computeMetricsAvailable = true;
    firstLoad.computeHealthy = true;
    auto secondLoad = firstLoad;
    secondLoad.cpuPercent = 80;
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(4)) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        for (const auto& inbound : firstTransport->poll(5)) first.receive(inbound, now);
        for (const auto& inbound : secondTransport->poll(5)) second.receive(inbound, now);
        first.tick(now, firstLoad);
        second.tick(now, secondLoad);
        for (const auto& outbound : first.drainOutgoing()) firstTransport->send(outbound);
        for (const auto& outbound : second.drainOutgoing()) secondTransport->send(outbound);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    firstTransport->stop();
    secondTransport->stop();
    const auto leaders = (first.role() == edge_gateway::EmsClusterRole::Leader ? 1 : 0) +
        (second.role() == edge_gateway::EmsClusterRole::Leader ? 1 : 0);
    require(leaders == 1, "two loopback Ethernet transports must discover each other and elect one leader");
    std::remove(firstConfig.consensusStateFile.c_str());
    std::remove(firstConfig.membershipFile.c_str());
    std::remove(secondConfig.consensusStateFile.c_str());
    std::remove(secondConfig.membershipFile.c_str());
}
#endif

}  // namespace

int main() {
    try {
        testProtocolAuthentication();
        testThreeNodeElectionAndMembership();
        testLeaderPartitionFencesMinority();
        testTwoNodePartitionStopsBoth();
        testFiveNodeFormation();
        testQuorumExpandsWithCommittedMembership();
        testLockedCabinetNumbersAreHonored();
        testDuplicateLockedCabinetNumberIsRejected();
        testStaleUncommittedMemberIsNotNumbered();
        testRestartKeepsTermAndCabinetNumber();
        testStateFromAnotherClusterIsIgnored();
        testDuplicateMachineCodeQuarantinesNode();
        testConfigAndAddressValidation();
        testMissingComputeMetricsArePenalized();
        testCapabilityWeightedDispatchAllocation();
        testDispatchClosedLoopAndInterlock();
        testDispatchExpiresAcrossLeaderPartition();
        testStationTargetUsesIndependentTtl();
        testCapabilityIsObservableBeforeControlEnable();
        testClusterPointBridge();
#ifndef _WIN32
        testEthernetTransportLoopback();
#endif
        std::cout << "ems cluster tests passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ems cluster test failed: " << error.what() << std::endl;
        return 1;
    }
}
