#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/ems_cluster_network.hpp"
#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
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
    bool active = true;
};

class Simulation {
public:
    Simulation(
        std::string name,
        int count,
        std::vector<int> lockedCabinetNumbers = {},
        int expectedMembers = 0
    )
        : name_(std::move(name)) {
        for (int i = 0; i < count; ++i) {
            SimNode item;
            item.id = "COMM_TEST_" + std::to_string(i + 1);
            item.config = configFor(name_, expectedMembers > 0 ? expectedMembers : count);
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
            for (auto& item : nodes_) if (item.active) item.node->tick(nowMs_, item.load);
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
    message.assignments = {{"COMM_A", 1}, {"COMM_B", 2}};
    const auto frame = edge_gateway::EmsClusterProtocol::encode(message, config);
    const auto decoded = edge_gateway::EmsClusterProtocol::decode(frame.data(), frame.size(), config);
    require(decoded.term == 7 && decoded.lockedCabinetNo == 2 && decoded.assignments.size() == 2,
            "authenticated KECP/1 frame must round-trip");
    auto wrong = config;
    wrong.psk = "different-test-key-123456";
    bool rejected = false;
    try { edge_gateway::EmsClusterProtocol::decode(frame.data(), frame.size(), wrong); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "KECP/1 frame signed with another PSK must be rejected");
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
