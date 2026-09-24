#include "edge_gateway/ems_cluster_points.hpp"
#include "edge_gateway/cluster_write_authorization.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
using namespace edge_gateway;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void reelectionRecovery() {
    const auto name = "cluster_recovery_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/"+name).c_str()); } } cleanup{name};
    EmsClusterConfig config;
    config.enabled = config.controlEnabled = true;
    config.virtualSharedMemoryName = name;
    config.controlTargetIndexes = {1234};
    EmsClusterPointBridge bridge(config, "test");
    MemoryPointStore observer(name, MemoryStoreOpenMode::OpenExisting);
    EmsClusterStatus status;
    status.role = EmsClusterRole::Leader;
    status.leaderNodeId = "A";
    status.quorumValid = status.controlConfigured = status.capability.controlEnabled = true;
    status.term = 2;
    status.membershipEpoch = 1;
    status.authorityExpireAtMs = 20000;
    EmsClusterDispatchState dispatch;
    dispatch.valid = true;
    dispatch.term = status.term;
    dispatch.membershipEpoch = status.membershipEpoch;
    dispatch.sequence = 157;
    dispatch.expireAtMs = 19000;
    dispatch.code = EmsClusterDispatchCode::Accepted;
    dispatch.accepted.paKw = 9;
    bridge.publish(status, dispatch, 1000000, 10000);
    auto snapshot = observer.clusterAuthority();
    require(snapshot && snapshot->valid && snapshot->authorization, "recovery fixture must start authorized");
    PendingWriteCommand oldCommand;
    oldCommand.index = 1234;
    oldCommand.value = 9;
    oldCommand.clusterAuthorization = snapshot->authorization;
    require(clusterAuthorizationValid(config, oldCommand, *snapshot, localKernelBootId(), 10000),
            "pre-election command must initially be authorized");
    status.quorumValid = false;
    status.authorityExpireAtMs = 0;
    dispatch.valid = false;
    dispatch.code = EmsClusterDispatchCode::Expired;
    bridge.publish(status, dispatch, 1000001, 10001);
    require(!observer.clusterAuthority()->valid, "quorum loss must revoke old dispatch");
    status.role = EmsClusterRole::Candidate;
    status.leaderNodeId.clear();
    status.term = 3;
    bridge.publish(status, dispatch, 1000002, 10002);
    require(!observer.clusterAuthority()->valid, "candidate with old dispatch must remain unauthorized");
    status.role = EmsClusterRole::Leader;
    status.leaderNodeId = "A";
    status.term = 4;
    status.quorumValid = true;
    status.authorityExpireAtMs = 20000;
    bridge.publish(status, dispatch, 1000003, 10003);
    require(!observer.clusterAuthority()->valid && observer.clusterAuthority()->stationStrategyActive,
            "re-elected leader must calculate targets but cannot authorize old-term dispatch");
    dispatch.valid = true;
    dispatch.term = status.term;
    dispatch.sequence = 1;
    dispatch.code = EmsClusterDispatchCode::Accepted;
    bridge.publish(status, dispatch, 1000004, 10004);
    snapshot = observer.clusterAuthority();
    require(snapshot && snapshot->valid && snapshot->authorization && snapshot->targets[0] == 9,
            "new term sequence 1 must recover in the same bridge after old term sequence 157");
    require(!clusterAuthorizationValid(config, oldCommand, *snapshot, localKernelBootId(), 10004),
            "re-election must never revive the old command");
    const auto recoveredEpoch = snapshot->authorization->authorityEpoch;
    require(recoveredEpoch != oldCommand.clusterAuthorization->authorityEpoch,
            "re-election must rotate the authority epoch");
    PendingWriteCommand currentCommand = oldCommand;
    currentCommand.clusterAuthorization = snapshot->authorization;
    auto staleEpochCommand = currentCommand;
    staleEpochCommand.clusterAuthorization->authorityEpoch = oldCommand.clusterAuthorization->authorityEpoch;
    require(!clusterAuthorizationValid(config, staleEpochCommand, *snapshot, localKernelBootId(), 10004),
            "old epoch command must fail even when its sequence matches the new term");
    status.authorityExpireAtMs = dispatch.expireAtMs = 22000;
    bridge.publish(status, dispatch, 1000005, 10005);
    require(observer.clusterAuthority()->authorization->notAfterMonotonicMs == 19000,
            "recovered sequence must preserve its earliest deadline");
    auto mismatched = dispatch;
    mismatched.term = 2;
    mismatched.sequence = 9999;
    bridge.publish(status, mismatched, 1000006, 10006);
    require(!observer.clusterAuthority()->valid, "valid-flagged old term must revoke current authority");
    bridge.publish(status, dispatch, 1000007, 10007);
    require(!observer.clusterAuthority()->valid, "mismatched publication must not allow revoked sequence resurrection");
    dispatch.sequence = 2;
    bridge.publish(status, dispatch, 1000008, 10008);
    require(observer.clusterAuthority()->valid,
            "old term high sequence must not poison the current scope after a valid publication");
    require(!clusterAuthorizationValid(config, currentCommand, *observer.clusterAuthority(), localKernelBootId(), 10008),
            "previous sequence command must not authorize a newer dispatch");
    mismatched = dispatch;
    mismatched.sequence = 1;
    bridge.publish(status, mismatched, 1000009, 10009);
    require(!observer.clusterAuthority()->valid, "older sequence in the same scope must be rejected");
    bridge.publish(status, dispatch, 1000010, 10010);
    require(!observer.clusterAuthority()->valid, "older sequence rejection must keep current sequence revoked");
    dispatch.sequence = 3;
    bridge.publish(status, dispatch, 1000011, 10011);
    require(observer.clusterAuthority()->valid, "strictly newer same-scope sequence must recover");

    status.membershipEpoch = 2;
    dispatch.valid = false;
    dispatch.sequence = 9999;
    bridge.publish(status, dispatch, 1000012, 10012);
    require(!observer.clusterAuthority()->valid, "old membership dispatch must not grant authority in new membership");
    dispatch.valid = true;
    dispatch.membershipEpoch = status.membershipEpoch;
    dispatch.sequence = 1;
    bridge.publish(status, dispatch, 1000013, 10013);
    snapshot = observer.clusterAuthority();
    require(snapshot && snapshot->valid && snapshot->authorization &&
            snapshot->authorization->authorityEpoch != recoveredEpoch,
            "membership transition must recover low sequence with a new authority epoch");
    require(!clusterAuthorizationValid(config, currentCommand, *snapshot, localKernelBootId(), 10013),
            "old membership command with the same sequence must remain rejected");
    mismatched = dispatch;
    mismatched.membershipEpoch = 1;
    mismatched.sequence = 9999;
    bridge.publish(status, mismatched, 1000014, 10014);
    require(!observer.clusterAuthority()->valid, "valid-flagged old membership must be rejected");
    dispatch.sequence = 2;
    bridge.publish(status, dispatch, 1000015, 10015);
    require(observer.clusterAuthority()->valid, "old membership high sequence must not poison current membership");
    const auto previousLeaderEpoch = observer.clusterAuthority()->authorization->authorityEpoch;
    status.leaderNodeId = "B";
    dispatch.sequence = 1;
    bridge.publish(status, dispatch, 1000016, 10016);
    require(observer.clusterAuthority()->valid &&
            observer.clusterAuthority()->authorization->authorityEpoch != previousLeaderEpoch,
            "leader identity change must begin a separate authority scope");
    bridge.publish(status, dispatch, 1000017);
    bridge.publish(status, dispatch, 1000018, 10018);
    require(!observer.clusterAuthority()->valid, "diagnostic-only publication must tombstone current sequence");
    dispatch.sequence = 2;
    dispatch.expireAtMs = 10100;
    bridge.publish(status, dispatch, 1000019, 10019);
    require(observer.clusterAuthority()->valid, "new sequence must recover after diagnostic-only revocation");
    bridge.publish(status, dispatch, 1000100, 10100);
    require(!observer.clusterAuthority()->valid, "recovered authority must expire exactly at its deadline");
}
int main() {
    const auto name = "cluster_output_authority_" + std::to_string(getpid());
    struct Cleanup { std::string name; ~Cleanup() { shm_unlink(("/"+name).c_str()); } } cleanup{name};
    try {
        reelectionRecovery();
        EmsClusterConfig config;
        config.enabled = config.controlEnabled = true;
        config.virtualSharedMemoryName = name;
        config.controlTargetIndexes = {1234};
        EmsClusterPointBridge bridge(config, "test");
        MemoryPointStore observer(name, MemoryStoreOpenMode::OpenExisting);
        EmsClusterStatus status;
        status.role = EmsClusterRole::Leader;
        status.quorumValid = status.controlConfigured = status.capability.controlEnabled = true;
        status.term = status.membershipEpoch = 1;
        status.authorityExpireAtMs = 10100;
        EmsClusterDispatchState dispatch;
        bridge.publish(status, dispatch, 999999, 9999);
        const auto startup = observer.clusterAuthority();
        require(startup && startup->stationStrategyActive,
                "leader strategy must start without an existing dispatch in four-argument publish");
        require(!startup->valid && !startup->authorization,
                "strategy permission alone must not authorize physical writes");
        require(clusterStationStrategyAllowed(*startup, localKernelBootId(), 9999),
                "strategy gate must survive the atomic shared-memory round trip");
        require(!clusterStationStrategyAllowed(*startup, localKernelBootId(), 10100),
                "strategy must stop at exact monotonic expiry even without a new publication");
        auto wrongBoot = localKernelBootId();
        wrongBoot[0] ^= 1;
        require(!clusterStationStrategyAllowed(*startup, wrongBoot, 9999), "strategy must reject a different boot");
        const auto gate = observer.getLatestByIndex(config.virtualPointBaseIndex +
                                                   ems_cluster_point::kStationStrategyActive, 999999);
        require(gate && gate->value == 1 && gate->expireAt == 1000100,
                "strategy diagnostic TTL must use leader lease, not absent dispatch deadline");
        dispatch.valid = true;
        dispatch.sequence = dispatch.term = dispatch.membershipEpoch = 1;
        dispatch.expireAtMs = 12000;
        dispatch.code = EmsClusterDispatchCode::Accepted;
        dispatch.accepted.paKw = 18;
        bridge.publish(status, dispatch, 1000000, 10000);
        auto snapshot = observer.clusterAuthority();
        require(snapshot && snapshot->valid && snapshot->authorization, "bridge must publish authoritative snapshot");
        require(snapshot->authorization->notAfterMonotonicMs == 10100, "bridge must preserve earliest absolute deadline");
        const auto original = *snapshot->authorization;
        bridge.publish(status, dispatch, 1000050, 10050);
        snapshot = observer.clusterAuthority();
        require(snapshot && snapshot->authorization && snapshot->authorization->notAfterMonotonicMs == 10100,
                "status refresh must not grant a new lease");
        require(snapshot->targets[0] == 18, "snapshot targets must match dispatch");
        const auto point = observer.getLatestByIndex(config.virtualPointBaseIndex + ems_cluster_point::kDispatchPa, 1000050);
        require(point && point->expireAt == 1000100, "diagnostic TTL must not exceed remaining authority");
        auto invalidStatus = status;
        invalidStatus.authorityExpireAtMs = 0;
        bridge.publish(invalidStatus, dispatch, 1000060, 10060);
        status.authorityExpireAtMs = 20000;
        bridge.publish(status, dispatch, 1000070, 10070);
        require(!observer.clusterAuthority()->valid, "revoked sequence must not revive with a later deadline");
        require(clusterStationStrategyAllowed(*observer.clusterAuthority(), localKernelBootId(), 10070),
                "renewed leader may calculate a new target without reviving a revoked dispatch");
        bridge.publish(status, dispatch, 1000100, 10100);
        require(!observer.clusterAuthority()->valid, "exact expiry must invalidate snapshot");
        status.authorityExpireAtMs = 20000;
        dispatch.expireAtMs = 10300;
        dispatch.sequence = 2;
        bridge.publish(status, dispatch, 1000200, 10200);
        snapshot = observer.clusterAuthority();
        require(snapshot && snapshot->valid && snapshot->authorization &&
                snapshot->authorization->notAfterMonotonicMs == 10300, "dispatch expiry caps renewed lease");
        EmsClusterPointBridge restarted(config, "test");
        require(!observer.clusterAuthority()->valid, "publisher restart must clear old authority before first publish");
        require(!clusterStationStrategyAllowed(*observer.clusterAuthority(), localKernelBootId(), 10250),
                "publisher restart must also clear station strategy permission");
        restarted.publish(status, dispatch, 1000250, 10250);
        require(observer.clusterAuthority()->authorization->authorityEpoch != original.authorityEpoch,
                "restart must use a new authority epoch even in the same kernel boot");
        status.role = EmsClusterRole::Follower;
        dispatch.sequence++;
        restarted.publish(status, dispatch, 1000251, 10251);
        require(observer.clusterAuthority()->valid &&
                !clusterStationStrategyAllowed(*observer.clusterAuthority(), localKernelBootId(), 10251),
                "follower may accept a dispatch but must not calculate the station strategy");
        restarted.publish(status, dispatch, 1000252);
        require(!observer.clusterAuthority()->valid && !observer.clusterAuthority()->stationStrategyActive,
                "three-argument diagnostic publication must not grant either permission");
        std::cout << "ems_cluster_output_authority_test passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
