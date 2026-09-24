#include "edge_gateway/ems_cluster.hpp"
#include "edge_gateway/ems_cluster_network.hpp"
#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/ems_cluster_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
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

void presetMembership(const edge_gateway::EmsClusterConfig& config,
                      const std::vector<edge_gateway::EmsClusterCabinetAssignment>& assignments) {
    std::ofstream out(config.membershipFile.c_str());
    out << "{\"schemaVersion\":\"1.0\",\"clusterId\":\"" << config.clusterId
        << "\",\"membershipEpoch\":1,\"assignments\":[";
    for (std::size_t i = 0; i < assignments.size(); ++i) {
        out << (i ? "," : "") << "{\"nodeId\":\"" << assignments[i].nodeId
            << "\",\"cabinetNo\":" << assignments[i].cabinetNo << "}";
    }
    out << "]}";
    require(static_cast<bool>(out), "test membership fixture must be writable");
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
    std::int64_t lastHeartbeatAckMs = 0;
    edge_gateway::EmsClusterMessage lastHeartbeatAck;
    std::map<std::string, edge_gateway::EmsClusterMessage> heartbeatAcks;
    std::uint64_t lastSentDispatchSequence = 0;
};

class Simulation {
public:
    std::function<bool(int, int, const edge_gateway::EmsClusterMessage&)> deliverMessage;
    Simulation(
        std::string name,
        int count,
        std::vector<int> lockedCabinetNumbers = {},
        int expectedMembers = 0,
        bool controlEnabled = false,
        std::function<void(edge_gateway::EmsClusterConfig&)> configure = {}
    )
        : name_(std::move(name)) {
        std::vector<edge_gateway::EmsClusterCabinetAssignment> assignments;
        std::set<int> used;
        for (const auto number : lockedCabinetNumbers) if (number > 0) used.insert(number);
        for (int i = 0; i < count; ++i) {
            int number = static_cast<std::size_t>(i) < lockedCabinetNumbers.size() ? lockedCabinetNumbers[i] : 0;
            if (number == 0) { number = 1; while (used.count(number)) ++number; used.insert(number); }
            assignments.push_back({"COMM_TEST_" + std::to_string(i + 1), number});
        }
        for (int i = 0; i < count; ++i) {
            SimNode item;
            item.id = "COMM_TEST_" + std::to_string(i + 1);
            item.config = configFor(name_, expectedMembers > 0 ? expectedMembers : count);
            item.config.controlEnabled = controlEnabled;
            item.config.dispatchCycleMs = 100;
            item.config.dispatchTtlMs = 400;
            item.config.capabilityTtlMs = 400;
            if (configure) configure(item.config);
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
            presetMembership(item.config, assignments);
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

    void rejoin() {
        for (auto& row : links_) std::fill(row.begin(), row.end(), true);
    }

    void alignLeaderHeartbeat() {
        for (int step = 0; step < 5 && at(0).lastHeartbeatAckMs != now(); ++step) run(25);
        require(leaderIndex() == 0 && at(0).lastHeartbeatAckMs == now(),
                "fixture must observe a fresh leader heartbeat ACK");
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
                if (outbound.message.type == edge_gateway::EmsClusterMessageType::DispatchTarget) {
                    nodes_[i].lastSentDispatchSequence = outbound.message.dispatchSequence;
                }
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
                if (deliverMessage && !deliverMessage(envelope.source, static_cast<int>(target), inbound.message)) continue;
                nodes_[target].node->receive(inbound, nowMs_);
                if (inbound.message.type == edge_gateway::EmsClusterMessageType::HeartbeatAck) {
                    nodes_[target].lastHeartbeatAckMs = nowMs_;
                    nodes_[target].lastHeartbeatAck = inbound.message;
                    nodes_[target].heartbeatAcks[inbound.message.senderNodeId] = inbound.message;
                }
            }
        }
    }

    std::string name_;
    std::int64_t nowMs_ = 1000;
    std::vector<SimNode> nodes_;
    std::vector<std::vector<bool>> links_;
};

void testDelayedFirstAckCannotMoveSendDeadline() {
    Simulation simulation("delayed-first-ack", 3);
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    require(simulation.leaderIndex() == 0, "delayed ACK test needs leader zero");
    edge_gateway::EmsClusterMessage delayed;
    std::int64_t sentAt = 0;
    simulation.deliverMessage = [&](int, int target, const edge_gateway::EmsClusterMessage& message) {
        if (target != 0) return true;
        if (message.type == edge_gateway::EmsClusterMessageType::HeartbeatAck) {
            delayed = message;
            sentAt = simulation.now();
        }
        return false;
    };
    simulation.run(100);
    require(sentAt > 0, "must capture an undelivered first ACK");
    simulation.partition({0}, {1, 2});
    simulation.run(250);
    simulation.at(0).node->receive({delayed, "169.254.1.3"}, simulation.now());
    require(simulation.at(0).node->status(simulation.now()).authorityExpireAtMs == sentAt + 400,
            "valid delayed ACK must anchor to send time, not merely be dropped");
    require(!simulation.at(0).node->status(sentAt + 400).quorumValid,
            "delayed first ACK must not extend authority beyond originating heartbeat send+L");
}

void testDiscoverCannotResetAckReplayWindow() {
    Simulation simulation("discover-ack-replay", 3);
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    const auto ack = simulation.at(0).lastHeartbeatAck;
    const auto expiry = simulation.now() + 400;
    simulation.partition({0}, {1, 2});
    simulation.run(375);
    auto discover = ack;
    discover.type = edge_gateway::EmsClusterMessageType::Discover;
    discover.sequence = 1;
    simulation.at(0).node->receive({discover, "169.254.1.3"}, simulation.now());
    simulation.at(0).node->receive({ack, "169.254.1.3"}, simulation.now());
    require(!simulation.at(0).node->status(expiry).quorumValid,
            "Discover must not reset replay state and admit an already-used ACK");
}

void testDelayedVotesAndHeartbeatActivation() {
    Simulation delayed("delayed-votes", 3);
    edge_gateway::EmsClusterMessage vote;
    std::int64_t electionSentAt = 0;
    delayed.deliverMessage = [&](int source, int target, const edge_gateway::EmsClusterMessage& message) {
        if (source == 0 && message.type == edge_gateway::EmsClusterMessageType::VoteRequest) electionSentAt = delayed.now();
        if (target == 0 && message.type == edge_gateway::EmsClusterMessageType::VoteReply && message.voteGranted) {
            vote = message;
            return false;
        }
        return true;
    };
    for (int i = 0; i < 100 && vote.sequence == 0; ++i) delayed.run(25);
    require(vote.sequence != 0 && electionSentAt > 0, "must capture a granted vote reply");
    delayed.partition({0}, {1, 2});
    delayed.run(static_cast<int>(electionSentAt + 400 - delayed.now()));
    delayed.at(0).node->receive({vote, "169.254.1.3"}, delayed.now());
    require(delayed.at(0).node->role() != edge_gateway::EmsClusterRole::Leader &&
            !delayed.at(0).node->status(delayed.now()).quorumValid,
            "expired election evidence must not activate a leader");

    Simulation activation("heartbeat-activation", 3, {}, 3, true);
    activation.deliverMessage = [](int, int target, const edge_gateway::EmsClusterMessage& message) {
        return target != 0 || message.type != edge_gateway::EmsClusterMessageType::HeartbeatAck;
    };
    for (int i = 0; i < 100 && activation.leaderIndex() != 0; ++i) activation.run(25);
    require(activation.leaderIndex() == 0, "fresh votes may establish a provisional leader");
    require(!activation.at(0).node->status(activation.now()).quorumValid &&
            !activation.at(0).node->activeDispatch(activation.now()).valid,
            "votes alone must never confer effective control");
    activation.run(400);
    require(activation.at(0).node->role() != edge_gateway::EmsClusterRole::Leader,
            "provisional leader must time out without heartbeat quorum");
}

void testVoteHoldSurvivesHigherTermAndRestart() {
    Simulation simulation("vote-hold", 3);
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    auto& follower = simulation.at(1);
    auto request = simulation.at(0).heartbeatAcks.at(simulation.at(2).id);
    request.type = edge_gateway::EmsClusterMessageType::VoteRequest;
    request.electionId = 700;
    request.term = follower.node->currentTerm() + 1;
    request.sequence += 10000;
    request.computeHealthy = true;
    const auto originalTerm = follower.node->currentTerm();
    follower.node->drainOutgoing();
    follower.node->receive({request, "169.254.1.3"}, simulation.now() + 499);
    require(follower.node->currentTerm() == originalTerm && follower.node->drainOutgoing().empty(),
            "higher-term request must not break a live leader promise");
    follower.node->receive({request, "169.254.1.3"}, simulation.now() + 500);
    auto replies = follower.node->drainOutgoing();
    require(std::any_of(replies.begin(), replies.end(), [](const auto& item) {
        return item.message.type == edge_gateway::EmsClusterMessageType::VoteReply && item.message.voteGranted;
    }), "vote must become eligible after the full hold");

    follower.node.reset(new edge_gateway::EmsClusterNode(follower.config, follower.id, "PROCESS_RESTART_VOTE"));
    const auto restartedAt = simulation.now() + 500;
    follower.node->tick(restartedAt, follower.load);
    std::uint64_t challenge = 0;
    for (const auto& item : follower.node->drainOutgoing()) challenge = item.message.discoveryChallenge;
    auto hello = request;
    hello.type = edge_gateway::EmsClusterMessageType::Hello;
    hello.discoveryReplyTo = challenge;
    hello.recipientIncarnation = follower.node->bootId();
    follower.node->receive({hello, "169.254.1.3"}, restartedAt);
    request.term += 1;
    request.sequence += 1;
    follower.node->receive({request, "169.254.1.3"}, restartedAt + 499);
    require(follower.node->drainOutgoing().empty(), "restart must wait out unrecoverable lease promises");
    follower.node->receive({request, "169.254.1.3"}, restartedAt + 500);
    replies = follower.node->drainOutgoing();
    require(std::any_of(replies.begin(), replies.end(), [](const auto& item) { return item.message.voteGranted; }),
            "restarted process may vote only after startup hold and session validation");
}

void testRestartRejectsPreviousIncarnation() {
    Simulation simulation("restart-incarnation", 3);
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    const auto oldAck = simulation.at(0).heartbeatAcks.at(simulation.at(1).id);
    auto& restarted = simulation.at(1);
    const auto previousTerm = restarted.node->currentTerm();
    restarted.node.reset(new edge_gateway::EmsClusterNode(restarted.config, restarted.id, "PROCESS_NEW_1"));
    restarted.node->receive({oldAck, "169.254.1.2"}, simulation.now());
    require(!restarted.node->status(simulation.now()).quorumValid && restarted.node->currentTerm() == previousTerm,
            "restart cannot recover authority from an old reply");
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    const auto deadline = simulation.at(0).node->status(simulation.now()).authorityExpireAtMs;
    simulation.partition({0}, {1, 2});
    simulation.run(375);
    auto replay = oldAck;
    replay.sequence += 100000;
    simulation.at(0).node->receive({replay, "169.254.1.2"}, simulation.now());
    require(simulation.at(0).node->status(simulation.now()).authorityExpireAtMs == deadline,
            "retired sender incarnation cannot renew the current lease");
}

void testDelayedDispatchCannotAcquireFreshTtl() {
    Simulation simulation("delayed-dispatch", 3, {}, 3, true);
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    edge_gateway::EmsClusterMessage target;
    simulation.deliverMessage = [&](int, int receiver, const edge_gateway::EmsClusterMessage& message) {
        if (receiver != 1) return true;
        if (target.sequence == 0 && message.type == edge_gateway::EmsClusterMessageType::DispatchTarget) target = message;
        return false;
    };
    simulation.run(375);
    auto& follower = *simulation.at(1).node;
    require(target.sequence != 0 && follower.status(simulation.now()).quorumValid,
            "delayed dispatch test requires a target while follower heartbeat lease still lives");
    const auto before = follower.activeDispatch(simulation.now()).sequence;
    follower.receive({target, "169.254.1.1"}, simulation.now());
    require(!follower.activeDispatch(simulation.now()).valid && follower.activeDispatch(simulation.now()).sequence == before,
            "delayed target cannot restart its TTL at receipt");
    const auto replies = follower.drainOutgoing();
    require(std::any_of(replies.begin(), replies.end(), [](const auto& item) {
        return item.message.type == edge_gateway::EmsClusterMessageType::DispatchAck &&
            item.message.dispatchCode == edge_gateway::EmsClusterDispatchCode::Expired;
    }), "expired local request correlation must reject delayed control explicitly");
}

void testBoundedIncarnationAndChallengeState() {
    Simulation simulation("session-budget", 2);
    auto& node = *simulation.at(0).node;
    node.tick(simulation.now(), simulation.at(0).load);
    std::uint64_t challenge = 0;
    for (const auto& item : node.drainOutgoing()) challenge = item.message.discoveryChallenge;
    edge_gateway::EmsClusterMessage hello;
    hello.type = edge_gateway::EmsClusterMessageType::Hello;
    hello.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(node.config().clusterId);
    hello.configHash = node.configurationHash();
    hello.senderNodeId = "COMM_TEST_2";
    hello.recipientIncarnation = node.bootId();
    hello.discoveryReplyTo = challenge;
    hello.sequence = 1;
    for (int incarnation = 0; incarnation < 18; ++incarnation) {
        hello.senderIncarnation = "INCARNATION_" + std::to_string(incarnation);
        node.receive({hello, "169.254.1.2"}, simulation.now());
    }
    require(node.role() == edge_gateway::EmsClusterRole::Fault && node.status(simulation.now()).members.size() == 1,
            "retired incarnation storage must have a fail-closed hard limit");
    require(node.drainOutgoing().empty(), "state exhaustion must clear pending traffic");
}

void testLongLeaseBoundedWindows() {
    const auto configure = [](edge_gateway::EmsClusterConfig& config) {
        config.leaderLeaseMs = 60000;
        config.electionTimeoutMinMs = 60001;
        config.electionTimeoutMaxMs = 60800;
        config.memberTimeoutMs = 62000;
        config.discoveryIntervalMs = 100;
    };
    Simulation simulation("long-lease-window", 3, {}, 3, true, configure);
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(70000);
    for (int i = 0; i < 3; ++i) {
        require(simulation.at(i).node->role() != edge_gateway::EmsClusterRole::Fault &&
                simulation.at(i).node->status(simulation.now()).controlActive,
                "healthy 60s lease / 100ms traffic must not fault at the 32-challenge limit");
    }
    Simulation eviction("evicted-discovery", 2, {}, 2, false, configure);
    auto& node = *eviction.at(0).node;
    node.tick(1000, eviction.at(0).load);
    std::uint64_t firstChallenge = 0;
    for (const auto& item : node.drainOutgoing()) firstChallenge = item.message.discoveryChallenge;
    for (int i = 1; i <= 32; ++i) { node.tick(1000 + i * 100, eviction.at(0).load); node.drainOutgoing(); }
    edge_gateway::EmsClusterMessage stale;
    stale.type = edge_gateway::EmsClusterMessageType::Hello;
    stale.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(node.config().clusterId);
    stale.configHash = node.configurationHash();
    stale.senderNodeId = "COMM_TEST_2";
    stale.senderIncarnation = "EVICTED_PROCESS";
    stale.recipientIncarnation = node.bootId();
    stale.discoveryReplyTo = firstChallenge;
    stale.sequence = 1;
    node.receive({stale, "169.254.1.2"}, 4200);
    require(node.status(4200).members.empty(), "evicted discovery challenge must not admit a peer");
}

void testLeaderSelfVoteProtectsOldFollowerTargets() {
    Simulation simulation("self-vote-promise", 3, {}, 3, true);
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    bool shifted = false;
    simulation.deliverMessage = [&](int source, int target, const edge_gateway::EmsClusterMessage& message) {
        if (!shifted && source == 2 && target == 0 &&
            message.type == edge_gateway::EmsClusterMessageType::VoteRequest) shifted = true;
        // Initially A+B renew while C cannot hear them. C's delayed election then
        // reaches A as B is isolated: A's self vote must still protect B's target.
        return shifted ? source != 1 && target != 1 : source != 2 && target != 2;
    };
    for (int elapsed = 25; elapsed <= 6000; elapsed += 25) {
        simulation.run(25);
        std::set<std::uint64_t> activeTerms;
        for (int i = 0; i < 3; ++i) {
            const auto dispatch = simulation.at(i).node->activeDispatch(simulation.now());
            if (dispatch.valid) activeTerms.insert(dispatch.term);
        }
        require(activeTerms.size() <= 1,
                "leader self vote must prevent new-term control while old follower targets remain live");
    }
    require(shifted && simulation.leaderCount() == 1,
            "self-vote scenario must reconnect a majority and eventually elect one leader");
}

void testMissingMembershipCannotBootstrap() {
    auto config = configFor("missing-voters", 3);
    config.consensusStateFile = "missing-voters-consensus.json";
    config.membershipFile = "missing-voters-membership.json";
    bool rejected = false;
    try { edge_gateway::EmsClusterNode node(config, "COMM_A", "PROCESS_A"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "missing complete voting membership must fail startup, not auto-bootstrap");
}

void testFixedVotersPartitionAtStartup() {
    for (const auto count : {2, 3, 5}) {
        Simulation simulation("boot-partition-" + std::to_string(count), count, {}, count, true);
        std::vector<int> minority, majority;
        for (int i = 0; i < count; ++i) {
            (i < count / 2 ? minority : majority).push_back(i);
            simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
            simulation.at(i).stationTargetValid = true;
        }
        simulation.partition(minority, majority);
        for (int elapsed = 25; elapsed <= 8000; elapsed += 25) {
            simulation.run(25);
            int activeLeaders = 0;
            for (int i = 0; i < count; ++i) {
                const auto status = simulation.at(i).node->status(simulation.now());
                if (status.role == edge_gateway::EmsClusterRole::Leader && status.quorumValid) ++activeLeaders;
                if (i < count / 2 || count == 2) {
                    require(!status.controlActive, "startup partition minority must never acquire control");
                }
            }
            require(activeLeaders <= 1, "fixed voters must not form two startup quorums");
        }
        require(simulation.leaderCount() == (count == 2 ? 0 : 1), "only the full-set majority can elect at startup");
    }
}

void testOnlineExpansionWithOnlyAdeIsRejected() {
    Simulation simulation("fixed-membership-reject", 3);
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    auto change = simulation.at(0).lastHeartbeatAck;
    change.senderNodeId = simulation.at(0).id;
    change.senderIncarnation = simulation.at(0).node->bootId();
    change.senderBootId = change.senderIncarnation;
    change.leaderNodeId = change.senderNodeId;
    change.sequence += 10000;
    change.membershipEpoch += 1;
    change.proposalId = 77;
    change.assignments = {{simulation.at(0).id, 1}, {simulation.at(1).id, 2}, {simulation.at(2).id, 3},
                          {"COMM_D", 4}, {"COMM_E", 5}};
    auto& follower = *simulation.at(1).node;
    const auto before = follower.status(simulation.now());
    follower.drainOutgoing();
    change.type = edge_gateway::EmsClusterMessageType::MembershipProposal;
    follower.receive({change, "169.254.1.1"}, simulation.now());
    require(follower.drainOutgoing().empty(), "fixed voter must not ACK an online expansion proposal");
    change.type = edge_gateway::EmsClusterMessageType::MembershipCommit;
    ++change.sequence;
    follower.receive({change, "169.254.1.1"}, simulation.now());
    for (const auto id : {simulation.at(0).id, std::string("COMM_D"), std::string("COMM_E")}) {
        auto ack = change;
        ack.type = edge_gateway::EmsClusterMessageType::MembershipAck;
        ack.senderNodeId = id;
        simulation.at(0).node->receive({ack, "169.254.1.4"}, simulation.now());
    }
    simulation.run(1000);
    for (int i = 0; i < 3; ++i) {
        const auto status = simulation.at(i).node->status(simulation.now());
        require(status.membershipEpoch == before.membershipEpoch && status.quorum == 2 &&
                status.cabinetNo == i + 1, "A+D+E cannot change fixed ABC voting membership");
    }
}

void testDifferentProvisionedSetsCannotExchangeVotes() {
    Simulation simulation("fixed-set-mismatch", 3);
    const auto original = simulation.at(0).node->configurationHash();
    auto& other = simulation.at(2);
    presetMembership(other.config, {{other.id, 3}, {"FOREIGN_A", 1}, {"FOREIGN_B", 2}});
    other.node.reset(new edge_gateway::EmsClusterNode(other.config, other.id, "OTHER_VOTING_SET_PROCESS"));
    require(other.node->configurationHash() != original, "complete voting identities must bind configuration digest");
    simulation.run(6000);
    require(!other.node->status(simulation.now()).quorumValid && other.node->status(simulation.now()).members.empty(),
            "different provisioned sets cannot exchange membership, votes or heartbeats");
    require(simulation.leaderCount() == 1, "the original matching two voters may still form their configured majority");
}

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
    message.senderIncarnation = "PROCESS_A";
    message.heartbeatId = 101;
    message.heartbeatAckId = 100;
    message.heartbeatIncarnation = "PROCESS_B";
    message.discoveryChallenge = 17;
    message.discoveryReplyTo = 16;
    message.recipientIncarnation = "PROCESS_C";
    message.electionId = 21;
    message.voteReplyTo = 20;
    message.voteCandidateIncarnation = "PROCESS_D";
    message.dispatchRequestId = 25;
    message.dispatchRequestIncarnation = "PROCESS_E";
    message.authorityTtlMs = 200;
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
    require(frame[3] == '2' && frame[4] == 2 && decoded.senderIncarnation == "PROCESS_A" &&
            decoded.heartbeatId == 101 && decoded.heartbeatAckId == 100 &&
            decoded.heartbeatIncarnation == "PROCESS_B" && decoded.discoveryChallenge == 17 &&
            decoded.discoveryReplyTo == 16 && decoded.recipientIncarnation == "PROCESS_C" &&
            decoded.electionId == 21 && decoded.voteReplyTo == 20 &&
            decoded.voteCandidateIncarnation == "PROCESS_D" && decoded.dispatchRequestId == 25 &&
            decoded.dispatchRequestIncarnation == "PROCESS_E" && decoded.authorityTtlMs == 200,
            "KECP/2 must round-trip explicit correlation fields");
    for (const auto oldMagic : {true, false}) {
        auto incompatible = frame;
        incompatible[oldMagic ? 3 : 4] = oldMagic ? '1' : 1;
        bool versionRejected = false;
        try { edge_gateway::EmsClusterProtocol::decode(incompatible.data(), incompatible.size(), config); }
        catch (const std::invalid_argument&) { versionRejected = true; }
        require(versionRejected, "old KECP magic/version must fail closed");
    }
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

void testPartitionNeverOverlapsControlLeaders() {
    Simulation simulation("control-leader-overlap", 3, {}, 3, true);
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    require(simulation.leaderIndex() == 0, "overlap regression requires initial low-load leader");
    simulation.partition({0}, {1, 2});
    for (int elapsed = 25; elapsed <= 2500; elapsed += 25) {
        simulation.run(25);
        int activeLeaders = 0;
        for (int i = 0; i < 3; ++i) {
            const auto status = simulation.at(i).node->status(simulation.now());
            const auto dispatch = simulation.at(i).node->activeDispatch(simulation.now());
            if (status.role == edge_gateway::EmsClusterRole::Leader &&
                status.quorumValid && status.controlActive && dispatch.valid) ++activeLeaders;
        }
        require(activeLeaders <= 1,
                "partition must not overlap effective control leaders at " + std::to_string(elapsed) + "ms");
    }
}

void testLeaseExpiresAtLastAckBoundary() {
    Simulation simulation("ack-boundary", 3, {}, 3, true);
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    simulation.alignLeaderHeartbeat();
    auto& leader = simulation.at(0);
    require(simulation.leaderIndex() == 0 && leader.lastHeartbeatAckMs == simulation.now(),
            "boundary fixture must observe a freshly delivered heartbeat ACK");
    const auto expiry = leader.lastHeartbeatAckMs + leader.config.leaderLeaseMs;
    simulation.partition({0}, {1, 2});
    simulation.run(375);
    require(leader.node->status(expiry - 1).controlActive && leader.node->activeDispatch(expiry - 1).valid,
            "leader must retain control immediately before lastACK+L");
    const auto sequence = leader.node->activeDispatch(expiry - 1).sequence;
    const auto sentSequence = leader.lastSentDispatchSequence;
    for (const auto time : {expiry, expiry + 1}) {
        const auto status = leader.node->status(time);
        const auto dispatch = leader.node->activeDispatch(time);
        require(!status.quorumValid && !status.controlActive && !status.dispatch.valid && !dispatch.valid,
                "public authority must expire exactly at lastACK+L, even before tick");
        require(dispatch.accepted.paKw == 0 && dispatch.accepted.pbKw == 0 && dispatch.accepted.pcKw == 0,
                "expired authority must expose zero accepted power");
    }
    for (int elapsed = 400; elapsed <= 900; elapsed += 25) {
        simulation.run(25);
        require(leader.node->role() != edge_gateway::EmsClusterRole::Leader,
                "old leader must step down at lastACK+L");
        require(!leader.node->activeDispatch(simulation.now()).valid &&
                leader.node->activeDispatch(simulation.now()).sequence == sequence &&
                leader.lastSentDispatchSequence == sentSequence,
                "expired leader must not advance local or outbound dispatch sequence");
    }
}

void testControlPartitionAndRejoin(int count) {
    Simulation simulation("control-rejoin-" + std::to_string(count), count, {}, count, true);
    for (int i = 0; i < count; ++i) {
        simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    require(simulation.leaderIndex() == 0, "partition fixture requires node zero as leader");
    const auto oldTerm = simulation.at(0).node->status(simulation.now()).term;
    std::vector<int> minority, majority;
    for (int i = 0; i < count; ++i) (i < count / 2 ? minority : majority).push_back(i);
    simulation.partition(minority, majority);
    const auto checkAuthority = [&]() {
        int activeLeaders = 0;
        for (int i = 0; i < count; ++i) {
            const auto status = simulation.at(i).node->status(simulation.now());
            const auto dispatch = simulation.at(i).node->activeDispatch(simulation.now());
            require(status.controlActive == dispatch.valid && status.dispatch.valid == dispatch.valid,
                    "status and activeDispatch authority must agree");
            require(!dispatch.valid || status.quorumValid, "dispatch requires a live lease");
            if (status.role == edge_gateway::EmsClusterRole::Leader && status.quorumValid) ++activeLeaders;
        }
        require(activeLeaders <= 1, "partition/rejoin must never overlap quorum leaders");
    };
    for (int elapsed = 25; elapsed <= 2500; elapsed += 25) {
        simulation.run(25);
        checkAuthority();
        if (elapsed >= 400) {
            require(!simulation.at(0).node->status(simulation.now()).quorumValid &&
                    !simulation.at(0).node->activeDispatch(simulation.now()).valid,
                    "a minority's continuing ACKs cannot extend the majority lease");
            if (count == 2) {
                for (int i = 0; i < count; ++i) {
                    require(!simulation.at(i).node->status(simulation.now()).controlActive &&
                            !simulation.at(i).node->activeDispatch(simulation.now()).valid,
                            "both nodes must stop control after a two-node split");
                }
            }
        }
    }
    require(simulation.leaderCount() == (count == 2 ? 0 : 1), "only a majority can take over");
    if (count > 2) {
        const auto replacement = simulation.leaderIndex();
        require(replacement >= count / 2 &&
                simulation.at(replacement).node->status(simulation.now()).term > oldTerm &&
                simulation.at(replacement).node->activeDispatch(simulation.now()).valid,
                "majority replacement must acquire a new term and active dispatch");
    }
    simulation.rejoin();
    for (int elapsed = 25; elapsed <= 3000; elapsed += 25) {
        simulation.run(25);
        checkAuthority();
    }
    require(simulation.leaderCount() == 1, "rejoined cluster must converge to one leader");
    const auto status = simulation.at(simulation.leaderIndex()).node->status(simulation.now());
    require(status.controlActive && status.term >= oldTerm, "rejoined leader must restore valid control");
    for (int i = 0; i < count; ++i) {
        require(simulation.at(i).node->status(simulation.now()).leaderNodeId == status.nodeId &&
                simulation.at(i).node->activeDispatch(simulation.now()).term == status.term,
                "rejoined nodes must follow the current term, not reactivate old targets");
    }
}

void testHeartbeatAckContextDoesNotRenewLease() {
    for (const auto kind : {"term", "leader", "epoch", "duplicate", "expired", "unknown", "incarnation"}) {
        Simulation simulation(std::string("ack-context-") + kind, 3, {}, 3, true);
        for (int i = 0; i < 3; ++i) {
            simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
            simulation.at(i).stationTargetValid = true;
        }
        simulation.run(5000);
        simulation.alignLeaderHeartbeat();
        auto& leader = simulation.at(0);
        require(simulation.leaderIndex() == 0 && leader.lastHeartbeatAckMs == simulation.now(),
                "ACK context test needs an observed heartbeat ACK");
        auto ack = leader.lastHeartbeatAck;
        const auto expiry = leader.lastHeartbeatAckMs + leader.config.leaderLeaseMs;
        simulation.partition({0}, {1, 2});
        simulation.run(375);
        const std::string scenario(kind);
        if (scenario != "duplicate") ack.sequence += 10000;
        if (scenario == "term") --ack.term;
        if (scenario == "leader") ack.leaderNodeId = "OTHER_LEADER";
        if (scenario == "epoch") ++ack.membershipEpoch;
        if (scenario == "unknown") ack.heartbeatAckId += 10000;
        if (scenario == "incarnation") ack.heartbeatIncarnation = "OLD_PROCESS";
        leader.node->receive({ack, "169.254.1.3"}, scenario == "expired" ? expiry : simulation.now());
        require(!leader.node->status(expiry).quorumValid && !leader.node->activeDispatch(expiry).valid,
                scenario + " ACK must not renew authority past its original lease");
    }
}

void testFiveNodeLeaseRequiresTwoPeerAcks() {
    for (const auto peerCount : {1, 2}) {
        Simulation simulation("five-ack-order-" + std::to_string(peerCount), 5, {}, 5, true);
        for (int i = 0; i < 5; ++i) {
            simulation.at(i).stationTarget = {18, 18, 18, 0, 0, 0};
            simulation.at(i).stationTargetValid = true;
        }
        simulation.run(5000);
        simulation.alignLeaderHeartbeat();
        auto& leader = simulation.at(0);
        require(simulation.leaderIndex() == 0 && leader.heartbeatAcks.size() == 4 &&
                leader.lastHeartbeatAckMs == simulation.now(), "five-node ACK fixture must be settled");
        const auto initialAck = simulation.now();
        std::map<std::string, edge_gateway::EmsClusterMessage> delayedAcks;
        simulation.deliverMessage = [&](int, int target, const edge_gateway::EmsClusterMessage& message) {
            if (target != 0) return true;
            if (message.type == edge_gateway::EmsClusterMessageType::HeartbeatAck) delayedAcks[message.senderNodeId] = message;
            return false;
        };
        for (int peer = 1; peer <= peerCount; ++peer) {
            simulation.run(100);
            auto ack = delayedAcks.at(simulation.at(peer).id);
            leader.node->receive({ack, "169.254.1." + std::to_string(peer + 1)}, simulation.now());
        }
        // Two distinct peers must acknowledge heartbeat sends, not merely arrive recently.
        const auto expiry = initialAck + leader.config.leaderLeaseMs + (peerCount == 2 ? 100 : 0);
        leader.node->updateControlInputs(leader.capability, leader.stationTarget, true, expiry - 1);
        leader.node->tick(expiry - 1, leader.load);
        require(leader.node->status(expiry - 1).controlActive,
                "five-node authority must last until the quorum ACK boundary");
        require(!leader.node->status(expiry).quorumValid && !leader.node->activeDispatch(expiry).valid,
                "five-node lease must use the second-newest peer ACK, never the newest alone");
    }
}

void testFiveNodeFormation() {
    Simulation simulation("five", 5);
    simulation.run(6000);
    require(simulation.leaderCount() == 1, "five-node cluster must elect exactly one leader");
    const auto leader = simulation.leaderIndex();
    require(simulation.at(leader).node->status(simulation.now()).onlineMembers == 5,
            "five-node leader must observe all members");
}

void testMismatchedExpectedMembershipRejected() {
    bool rejected = false;
    try { Simulation simulation("dynamic-quorum", 5, {}, 2); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "five provisioned voters cannot bootstrap with expectedMembers=2");
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
    bool rejected = false;
    try { Simulation simulation("duplicate-lock", 3, {1, 1, 0}); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate cabinet numbers in a provisioned voting set must fail startup");
}

void testOfflineCommittedMemberKeepsNumber() {
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
    require(stale->cabinetNo == 3, "offline provisioned member keeps its identity and vote slot");
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
    bool rejected = false;
    try { edge_gateway::EmsClusterNode invalid(config, "COMM_NEW", "BOOT_NEW"); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "foreign membership cannot replace missing provisioned voters");
    presetMembership(config, {{"COMM_NEW", 1}, {"COMM_OTHER", 2}});
    edge_gateway::EmsClusterNode node(config, "COMM_NEW", "BOOT_NEW");
    require(node.currentTerm() == 0, "consensus state from another cluster must be ignored");
    require(node.status(1000).cabinetNo == 1, "only current cluster provisioned membership is used");
    std::remove(config.consensusStateFile.c_str());
    std::remove(config.membershipFile.c_str());
}

void testCorruptPersistedClusterStateFailsSafe() {
    struct Case {
        const char* label;
        const char* fileKind;
        std::string content;
    };
    const std::vector<Case> cases = {
        {
            "truncated-consensus",
            "consensus",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"TEST_CLUSTER_corrupt-state\",\"term\":41"
        },
        {
            "empty-consensus",
            "consensus",
            ""
        },
        {
            "missing-consensus-cluster-id",
            "consensus",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"\",\"term\":1,\"votedFor\":\"\"}"
        },
        {
            "vote-without-term",
            "consensus",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"TEST_CLUSTER_corrupt-state\","
            "\"term\":0,\"votedFor\":\"COMM_NEW\"}"
        },
        {
            "truncated-membership",
            "membership",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"TEST_CLUSTER_corrupt-state\","
            "\"membershipEpoch\":9,\"assignments\":[{\"nodeId\":\"COMM_NEW\",\"cabinetNo\":1}"
        },
        {
            "missing-membership-epoch",
            "membership",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"TEST_CLUSTER_corrupt-state\","
            "\"assignments\":[{\"nodeId\":\"COMM_NEW\",\"cabinetNo\":1}]}"
        },
        {
            "missing-membership-cluster-id",
            "membership",
            "{\"schemaVersion\":\"1.0\",\"clusterId\":\"\","
            "\"membershipEpoch\":0,\"assignments\":[]}"
        }
    };

    for (const auto& item : cases) {
        auto config = configFor("corrupt-state", 2);
        config.consensusStateFile = std::string("ems-cluster-test-") + item.label + "-consensus.json";
        config.membershipFile = std::string("ems-cluster-test-") + item.label + "-membership.json";
        std::remove(config.consensusStateFile.c_str());
        std::remove(config.membershipFile.c_str());
        const auto path = std::string(item.fileKind) == "consensus"
            ? config.consensusStateFile
            : config.membershipFile;
        {
            std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(output), "failed to create corrupt cluster state fixture");
            output << item.content;
        }

        bool rejected = false;
        std::string error;
        try {
            edge_gateway::EmsClusterNode node(config, "COMM_NEW", "BOOT_NEW");
        } catch (const std::exception& ex) {
            rejected = true;
            error = ex.what();
        }
        std::remove(config.consensusStateFile.c_str());
        std::remove(config.membershipFile.c_str());
        require(rejected, std::string("corrupt persisted ") + item.fileKind + " state must stop cluster startup");
        require(error.find(item.fileKind) != std::string::npos,
                std::string("corrupt ") + item.fileKind + " error must identify the state kind");
        require(error.find(path) != std::string::npos,
                std::string("corrupt ") + item.fileKind + " error must include the file path");
    }
}

void testDuplicateMachineCodeQuarantinesNode() {
    auto config = configFor("duplicate", 2);
    config.consensusStateFile = "ems-cluster-test-duplicate-consensus.json";
    config.membershipFile = "ems-cluster-test-duplicate-membership.json";
    presetMembership(config, {{"COMM_DUP", 1}, {"COMM_OTHER", 2}});
    edge_gateway::EmsClusterNode node(config, "COMM_DUP", "BOOT_LOCAL");
    edge_gateway::EmsClusterInbound inbound;
    inbound.message.type = edge_gateway::EmsClusterMessageType::Discover;
    inbound.message.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(config.clusterId);
    inbound.message.configHash = node.configurationHash();
    inbound.message.senderNodeId = "COMM_DUP";
    inbound.message.senderBootId = "BOOT_OTHER_DEVICE";
    inbound.message.senderIncarnation = "PROCESS_OTHER_DEVICE";
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

    config = configFor("validation-state-path", 2);
    config.consensusStateFile.clear();
    rejected = false;
    try { edge_gateway::EmsClusterNode::validateConfig(config); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "enabled EMS cluster must require a consensus state path");

    config = configFor("validation-membership-path", 2);
    config.membershipFile.clear();
    rejected = false;
    try { edge_gateway::EmsClusterNode::validateConfig(config); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "enabled EMS cluster must require a membership state path");

    config = configFor("validation-shared-state-path", 2);
    config.membershipFile = config.consensusStateFile;
    rejected = false;
    try { edge_gateway::EmsClusterNode::validateConfig(config); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "consensus and membership state must not share one file");

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
    const auto replacementLeader = simulation.leaderIndex();
    for (const auto member : majority) {
        if (member == replacementLeader) continue;
        const auto dispatch = simulation.at(member).node->activeDispatch(simulation.now());
        const auto status = simulation.at(member).node->status(simulation.now());
        require(dispatch.valid,
                "majority follower must accept the replacement leader dispatch in the new term");
        require(dispatch.term == status.term,
                "majority follower dispatch must belong to its current term");
    }
}

void testDispatchRejectsSameTermReplayAndOldTermMessage() {
    Simulation simulation("dispatch-replay", 3, {}, 0, true);
    edge_gateway::EmsClusterPhasePower target;
    target.paKw = 9.0;
    target.pbKw = 6.0;
    target.pcKw = 3.0;
    for (int i = 0; i < 3; ++i) {
        simulation.at(i).stationTarget = target;
        simulation.at(i).stationTargetValid = true;
    }
    simulation.run(5000);
    const auto leader = simulation.leaderIndex();
    require(leader >= 0, "dispatch replay test requires a leader");
    const auto follower = leader == 0 ? 1 : 0;
    auto& followerNode = *simulation.at(follower).node;
    const auto status = followerNode.status(simulation.now());
    const auto active = followerNode.activeDispatch(simulation.now());
    require(active.valid && active.sequence > 0, "dispatch replay test requires an active follower target");

    const auto requireAck = [&](edge_gateway::EmsClusterMessage message,
                                edge_gateway::EmsClusterDispatchCode expected,
                                const std::string& failure) {
        edge_gateway::EmsClusterInbound inbound;
        inbound.message = std::move(message);
        inbound.sourceAddress = "169.254.1.1";
        followerNode.receive(inbound, simulation.now());
        bool found = false;
        for (const auto& outbound : followerNode.drainOutgoing()) {
            if (outbound.message.type == edge_gateway::EmsClusterMessageType::DispatchAck &&
                outbound.message.dispatchSequence == inbound.message.dispatchSequence) {
                require(outbound.message.dispatchCode == expected, failure);
                found = true;
            }
        }
        require(found, failure + ": dispatch ACK missing");
    };

    edge_gateway::EmsClusterMessage replay;
    replay.type = edge_gateway::EmsClusterMessageType::DispatchTarget;
    replay.clusterIdHash = edge_gateway::EmsClusterProtocol::clusterIdHash(followerNode.config().clusterId);
    replay.configHash = followerNode.configurationHash();
    replay.term = status.term;
    replay.membershipEpoch = status.membershipEpoch;
    replay.sequence = std::numeric_limits<std::uint64_t>::max() - 1;
    replay.senderNodeId = simulation.at(leader).node->nodeId();
    replay.senderBootId = simulation.at(leader).node->bootId();
    replay.senderIncarnation = simulation.at(leader).node->bootId();
    replay.leaderNodeId = replay.senderNodeId;
    replay.dispatchSequence = active.sequence;
    replay.dispatchTtlMs = static_cast<std::uint32_t>(followerNode.config().dispatchTtlMs);
    replay.requestedPower = active.requested;
    requireAck(
        replay,
        edge_gateway::EmsClusterDispatchCode::StaleSequence,
        "same-term dispatch replay must be rejected"
    );

    auto oldTerm = replay;
    oldTerm.term = status.term - 1;
    oldTerm.sequence = std::numeric_limits<std::uint64_t>::max();
    oldTerm.dispatchSequence = active.sequence + 1;
    requireAck(
        oldTerm,
        edge_gateway::EmsClusterDispatchCode::TermMismatch,
        "old-term dispatch message must be rejected"
    );
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
    presetMembership(firstConfig, {{"COMM_NET_A", 1}, {"COMM_NET_B", 2}});
    presetMembership(secondConfig, {{"COMM_NET_B", 2}, {"COMM_NET_A", 1}});
    edge_gateway::EmsClusterNode first(firstConfig, "COMM_NET_A", "BOOT_NET_A");
    edge_gateway::EmsClusterNode second(secondConfig, "COMM_NET_B", "BOOT_NET_B");
    require(first.configurationHash() == second.configurationHash(),
            "fixed membership digest must not depend on provisioning JSON order");
    auto firstTransport = edge_gateway::makeEthernetClusterTransport(firstConfig, "COMM_NET_A", first.configurationHash());
    auto secondTransport = edge_gateway::makeEthernetClusterTransport(secondConfig, "COMM_NET_B", second.configurationHash());
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

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const std::string scenario(argv[1]);
            if (scenario == "delayed-ack") testDelayedFirstAckCannotMoveSendDeadline();
            else if (scenario == "discover-replay") testDiscoverCannotResetAckReplayWindow();
            else if (scenario == "window-capacity") testLongLeaseBoundedWindows();
            else if (scenario == "self-vote") testLeaderSelfVoteProtectsOldFollowerTargets();
            else if (scenario == "membership") testMissingMembershipCannotBootstrap();
            else throw std::runtime_error("unknown regression case");
            std::cout << scenario << " passed" << std::endl;
            return 0;
        }
        testDelayedFirstAckCannotMoveSendDeadline();
        testDiscoverCannotResetAckReplayWindow();
        testDelayedVotesAndHeartbeatActivation();
        testVoteHoldSurvivesHigherTermAndRestart();
        testRestartRejectsPreviousIncarnation();
        testDelayedDispatchCannotAcquireFreshTtl();
        testBoundedIncarnationAndChallengeState();
        testLongLeaseBoundedWindows();
        testLeaderSelfVoteProtectsOldFollowerTargets();
        testFixedVotersPartitionAtStartup();
        testOnlineExpansionWithOnlyAdeIsRejected();
        testDifferentProvisionedSetsCannotExchangeVotes();
        testProtocolAuthentication();
        testLeaseExpiresAtLastAckBoundary();
        testControlPartitionAndRejoin(2);
        testControlPartitionAndRejoin(3);
        testControlPartitionAndRejoin(5);
        testHeartbeatAckContextDoesNotRenewLease();
        testFiveNodeLeaseRequiresTwoPeerAcks();
        testPartitionNeverOverlapsControlLeaders();
        testThreeNodeElectionAndMembership();
        testLeaderPartitionFencesMinority();
        testTwoNodePartitionStopsBoth();
        testFiveNodeFormation();
        testMissingMembershipCannotBootstrap();
        testMismatchedExpectedMembershipRejected();
        testLockedCabinetNumbersAreHonored();
        testDuplicateLockedCabinetNumberIsRejected();
        testOfflineCommittedMemberKeepsNumber();
        testRestartKeepsTermAndCabinetNumber();
        testStateFromAnotherClusterIsIgnored();
        testCorruptPersistedClusterStateFailsSafe();
        testDuplicateMachineCodeQuarantinesNode();
        testConfigAndAddressValidation();
        testMissingComputeMetricsArePenalized();
        testCapabilityWeightedDispatchAllocation();
        testDispatchClosedLoopAndInterlock();
        testDispatchExpiresAcrossLeaderPartition();
        testDispatchRejectsSameTermReplayAndOldTermMessage();
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
