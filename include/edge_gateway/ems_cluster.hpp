#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "edge_gateway/models.hpp"

namespace edge_gateway {

enum class EmsClusterRole : std::uint8_t {
    Disabled = 0,
    Discovering = 1,
    Follower = 2,
    Candidate = 3,
    Leader = 4,
    Quarantined = 5,
    Fault = 6
};

enum class EmsClusterMessageType : std::uint8_t {
    Discover = 1,
    Hello = 2,
    Heartbeat = 3,
    HeartbeatAck = 4,
    VoteRequest = 5,
    VoteReply = 6,
    LeaderCommit = 7,
    StepDown = 8,
    MembershipProposal = 9,
    MembershipAck = 10,
    MembershipCommit = 11
};

struct EmsClusterCabinetAssignment {
    std::string nodeId;
    int cabinetNo = 0;
};

struct EmsClusterMessage {
    EmsClusterMessageType type = EmsClusterMessageType::Discover;
    std::uint64_t clusterIdHash = 0;
    std::uint64_t configHash = 0;
    std::uint64_t term = 0;
    std::uint64_t membershipEpoch = 0;
    std::uint64_t sequence = 0;
    std::uint64_t proposalId = 0;
    std::string senderNodeId;
    std::string senderBootId;
    std::string leaderNodeId;
    double loadScore = 100.0;
    int electionPriority = 100;
    int tcpPort = 0;
    int cabinetNo = 0;
    int lockedCabinetNo = 0;
    bool voteGranted = false;
    bool metricsComplete = false;
    bool computeHealthy = false;
    std::vector<EmsClusterCabinetAssignment> assignments;
};

struct EmsClusterInbound {
    EmsClusterMessage message;
    std::string sourceAddress;
};

struct EmsClusterOutbound {
    EmsClusterMessage message;
    std::string targetNodeId;
    bool discovery = false;
};

struct EmsClusterLoadSample {
    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    double computeTimeoutPercent = 100.0;
    double controlQueueP95Ms = 1000.0;
    double packetLossPercent = 0.0;
    bool computeMetricsAvailable = false;
    bool computeHealthy = false;
};

struct EmsClusterMemberStatus {
    std::string nodeId;
    std::string bootId;
    std::string address;
    std::int64_t lastSeenMs = 0;
    std::int64_t lastSeenAgeMs = -1;
    double loadScore = 100.0;
    int electionPriority = 100;
    int cabinetNo = 0;
    int lockedCabinetNo = 0;
    bool metricsComplete = false;
    bool computeHealthy = false;
    bool compatible = false;
    bool online = false;
    bool duplicateIdentity = false;
};

struct EmsClusterStatus {
    EmsClusterRole role = EmsClusterRole::Disabled;
    std::string nodeId;
    std::string leaderNodeId;
    std::uint64_t term = 0;
    std::uint64_t membershipEpoch = 0;
    int cabinetNo = 0;
    int onlineMembers = 0;
    int quorum = 0;
    bool quorumValid = false;
    bool metricsComplete = false;
    bool computeHealthy = false;
    double loadScore = 100.0;
    std::string reason;
    std::vector<EmsClusterMemberStatus> members;
};

class EmsClusterProtocol {
public:
    static std::vector<std::uint8_t> encode(
        const EmsClusterMessage& message,
        const EmsClusterConfig& config
    );
    static EmsClusterMessage decode(
        const std::uint8_t* data,
        std::size_t size,
        const EmsClusterConfig& config
    );
    static std::uint64_t clusterIdHash(const std::string& clusterId);
    static std::uint64_t configHash(const EmsClusterConfig& config);
};

class EmsClusterNode {
public:
    EmsClusterNode(EmsClusterConfig config, std::string nodeId, std::string bootId);

    void tick(std::int64_t monotonicNowMs, const EmsClusterLoadSample& load);
    void receive(const EmsClusterInbound& inbound, std::int64_t monotonicNowMs);
    std::vector<EmsClusterOutbound> drainOutgoing();
    EmsClusterStatus status(std::int64_t monotonicNowMs) const;

    const EmsClusterConfig& config() const { return config_; }
    const std::string& nodeId() const { return nodeId_; }
    const std::string& bootId() const { return bootId_; }
    std::uint64_t currentTerm() const { return currentTerm_; }
    EmsClusterRole role() const { return role_; }

    static void validateConfig(const EmsClusterConfig& config);
    static double calculateLoadScore(const EmsClusterLoadSample& load);
    static const char* roleName(EmsClusterRole role);

private:
    struct Member {
        EmsClusterMemberStatus status;
        std::uint64_t lastSequence = 0;
        std::int64_t lastAckMs = 0;
    };

    void loadPersistentState();
    void persistConsensusState() const;
    void loadMembership();
    void persistMembership() const;
    void resetElectionDeadline(std::int64_t nowMs);
    void becomeFollower(std::uint64_t term, const std::string& leader, std::int64_t nowMs, const std::string& reason);
    void startElection(std::int64_t nowMs);
    void becomeLeader(std::int64_t nowMs);
    void refreshMember(const EmsClusterInbound& inbound, std::int64_t nowMs);
    void handleVoteRequest(const EmsClusterMessage& message, std::int64_t nowMs);
    void handleMembershipProposal(const EmsClusterMessage& message, std::int64_t nowMs);
    void handleMembershipCommit(const EmsClusterMessage& message, std::int64_t nowMs);
    void maybeProposeMembership(std::int64_t nowMs);
    void maybeCommitMembership(std::int64_t nowMs);
    void queue(EmsClusterMessageType type, const std::string& target = std::string(), bool discovery = false);
    EmsClusterMessage baseMessage(EmsClusterMessageType type) const;
    int effectiveQuorum() const;
    int onlineCompatibleCount(std::int64_t nowMs) const;
    int cabinetNoFor(const std::string& nodeId) const;
    bool isVotingMember(const std::string& nodeId) const;
    bool leaderLeaseValid(std::int64_t nowMs) const;
    bool sameAssignments(
        const std::vector<EmsClusterCabinetAssignment>& lhs,
        const std::vector<EmsClusterCabinetAssignment>& rhs
    ) const;
    bool validAssignmentsForLocal(const std::vector<EmsClusterCabinetAssignment>& assignments) const;

    EmsClusterConfig config_;
    std::string nodeId_;
    std::string bootId_;
    std::uint64_t clusterIdHash_ = 0;
    std::uint64_t configHash_ = 0;
    EmsClusterRole role_ = EmsClusterRole::Disabled;
    std::string reason_;
    std::uint64_t currentTerm_ = 0;
    std::string votedFor_;
    std::string leaderNodeId_;
    mutable std::uint64_t sequence_ = 0;
    std::int64_t electionDeadlineMs_ = 0;
    std::int64_t lastDiscoveryMs_ = 0;
    std::int64_t lastHeartbeatMs_ = 0;
    std::int64_t lastLeaderSeenMs_ = 0;
    std::int64_t lastQuorumMs_ = 0;
    EmsClusterLoadSample load_;
    double loadScore_ = 100.0;
    std::map<std::string, Member> members_;
    std::set<std::string> votesGranted_;
    std::uint64_t membershipEpoch_ = 0;
    std::vector<EmsClusterCabinetAssignment> assignments_;
    std::uint64_t pendingProposalId_ = 0;
    std::uint64_t pendingMembershipEpoch_ = 0;
    std::int64_t pendingProposalLastSentMs_ = 0;
    std::vector<EmsClusterCabinetAssignment> pendingAssignments_;
    std::set<std::string> membershipAcks_;
    std::vector<EmsClusterOutbound> outgoing_;
};

std::string emsClusterStatusJson(const EmsClusterStatus& status, std::int64_t wallNowMs);

}  // namespace edge_gateway
