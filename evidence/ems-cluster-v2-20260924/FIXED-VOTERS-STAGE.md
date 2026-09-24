# Fixed Voting Membership Stage

Core green base: 1011d73c630fdbc88bb665d145488b6704a67616.
Membership red: aa3526b. This report accompanies the fixed-voter green commit.
It closes the membership blocker documented in CORE-STAGE.md by REMOVING online
reconfiguration, not by claiming to implement joint consensus.

## Production Scope

An enabled KECP/2 process must load a complete, valid, pre-provisioned membership
file with positive membershipEpoch, exactly expectedMembers unique voters and
cabinet numbers, its own identity present, and its locked cabinet number honored.
Missing, foreign-cluster, incomplete, mismatched-size or invalid membership fails
construction/startup. There is no automatic bootstrap, online expansion/removal,
renumbering, forced singleton or quorum reduction.

All peers must be provisioned OFFLINE with the identical full cluster identity,
epoch and assignment set. Example for two voters (distinct node identities):

```json
{"schemaVersion":"1.0","clusterId":"SITE_CLUSTER","membershipEpoch":1,
 "assignments":[{"nodeId":"COMM_A","cabinetNo":1},
                {"nodeId":"COMM_B","cabinetNo":2}]}
```

This is a contract example, not an executed provisioning instruction. Existing
validated state format remains readable. A legacy five-member file paired with
expectedMembers=2 now fails rather than silently changing quorum. Do not erase
term/vote files or rewrite a live membership file to force startup.

The existing KECP/2 configHash is now the full configuration digest: append the
original configuration digest and membershipEpoch as network-endian u64, then
each assignment sorted by nodeId as the existing length-prefixed string plus
network-endian u16 cabinet number; hash those bytes with the existing FNV-1a64.
This is the configuration field's intended purpose, not reuse of an unrelated
correlation field. No additional wire field/layout change is introduced.
Peers with different identities/cabinet assignments/epochs fail config matching
before session admission, liveness, votes or heartbeat authority. Non-voters are
also rejected before session admission. node.configurationHash() exposes this
complete local digest for diagnostics/tests; the static protocol configHash(config)
remains the configuration-only input to that digest.

Automatic proposal/commit functions and pending reconfiguration state are removed.
MembershipProposal, MembershipAck and MembershipCommit are rejected before any
term/liveness/replay mutation. Heartbeats must match the fixed epoch/assignments.
The max(oldQuorum,newQuorum) shortcut no longer exists on an executable path.

## Actual Verification

red-core-membership linked the 1011d73 library (provenance in
green-core-self-vote-result.json): compile exit 0, 2.121s; test exit 1, 0.018s,
"missing complete voting membership must fail startup, not auto-bootstrap".

FINAL green-fixed-voters-final: configure exit 0, 4.076s; library exit 0, 0.317s;
cluster compile exit 0, 2.173s; ems_cluster_test exit 0, 4.326s; graph compile
exit 0, 1.629s; existing graph_ems_cluster_dispatch_test exit 0, 0.117s.
The initial fixed-voter build also passed; final adds startup partition and
online-change/config-mismatch regressions. Exact commands, source SHA256,
compiler/library/binary hashes and actual results are in the named JSON/logs.

Simulation now pre-provisions complete voter files before constructing nodes.
The former unsafe dynamic-expansion test instead requires rejection. Cabinet
conflicts fail at startup, and an offline pre-provisioned member retains its
cabinet/vote slot. Existing election, dispatch, persistence, 2/3/5 partition/rejoin,
delayed ACK/vote/target, self-vote hold and bounded-state cases remain running.

New tests cover startup partition 1+1, 1+2, 2+3 with intermediate effective
authority checks; no startup minority gets control. A+D+E MembershipAck messages
and an expansion proposal/commit cannot mutate fixed ABC. Different provisioned
full sets cannot exchange sessions or votes. No test claims dynamic reconfiguration
support or treats a fully connected five-node expected-two bootstrap as safe.

## Integration and Limits

Changed products: include/edge_gateway/ems_cluster.hpp and src/ems_cluster.cpp.
Changed test: tools/ems_cluster_test.cpp. No downstream or transport files changed.
The frozen status.authorityExpireAtMs LOCAL monotonic interface is unchanged.
Main remains integration-pending on four-argument PointBridge publish; it is not
part of this library/test build. The old graph suite PASS does not qualify the
new downstream authorization/queue/actual-send implementation.

Fixed-set provisioning consistency and unique cluster identity are deployment
preconditions. Two separately mis-provisioned disjoint voter sets are not a
single supported cluster merely because they share a PSK/clusterId. Offline set
changes require stopping control on ALL prior voters and retiring old processes
and queued authority; no automatic migration/reconfiguration procedure is supplied
or qualified here. State rollback, Byzantine nodes, clock rollback/suspend and
physical output latching remain outside the core model described in CORE-STAGE.md.

After integrating downstream and transport commits, build ONLY the pinned candidate
in a separate ARM directory with the verified Allwinner Linaro GCC6.3.1 toolchain
and /opt/ky-cross/allwinner-sysroot, then target EmsClusterCoordinator,
ems_cluster_test and graph_ems_cluster_dispatch_test (plus the other workers'
named targeted regressions). Do not use RK3568, rebuild a release package or
overwrite sealed artifacts. Toolchain file/compiler paths must come from the
verified existing toolchain, not inferred from this native GCC15 environment.
This stage has performed NO ARM/remote/device build or deployment.

Integrated main/transport/downstream green, GCC6.3 ARM verification, dual-device
virtual-only acceptance and downstream actual-send checks remain outstanding.
Production approval remains false; this commit is a fixed-core integration candidate.
