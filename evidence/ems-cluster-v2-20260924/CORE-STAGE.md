# KECP/2 Core Stage - Not Production Approval

Base c583bb8aad3e98fca9bb1e1815dd5c62560662d5; red commit 4afaf81.
This report is committed with the core green stage. The tested product write set
is ems_cluster.hpp, ems_cluster.cpp, ems_cluster_dispatch.cpp. Changes to
ems_cluster_main.cpp are integration-pending and NOT compiled in this stage.
Regression file: tools/ems_cluster_test.cpp. No points, Graph, transport, CMake,
models, shared-memory, router or driver files were edited by this worker.

## Results

- red-delayed-ack: compile exit 0, 1.821s; test exit 1, 0.018s. Delayed first ACK
  incorrectly extends authority past the originating heartbeat send+L.
- red-discover-replay: compile exit 0, 1.620s; test exit 1, 0.018s. Discover resets
  the replay filter and admits an already-used ACK.
- Both initial reds link the c583bb8 library retained from the prior lease stage.
- green-core-initial: configure succeeded, library FAILED exit 2. A truncated
  status-JSON tail introduced while editing was repaired from the unchanged base
  tail. No tests ran on that failed build; its configure/library logs are retained.
- green-core-compile-repair/timing are intermediate failed cluster runs retained
  for traceability, not final green claims. Heartbeat phase changed with startup
  hold; boundary tests now align with an actually observed ACK. Old-term dispatch
  rejection continues returning its diagnostic ACK without mutating liveness.
- red-core-window-capacity: compile exit 0, 1.721s; test exit 1, 0.017s against
  the green-core-safety library: healthy 60000ms lease/100ms traffic reached a
  permanent Fault at the 32-challenge limit.
- red-core-self-vote: compile exit 0, 1.872s; test exit 1, 0.018s against the
  green-core-windows library: new-term control overlaps a still-valid old follower
  target after shifting A+B to A+C.
- FINAL green-core-self-vote: configure exit 0 (3.374s), library exit 0 (2.623s),
  cluster compile exit 0 (2.121s), ems_cluster_test exit 0 (4.327s), graph compile
  exit 0 (1.570s), existing graph_ems_cluster_dispatch_test exit 0 (0.117s).
- git diff --check passes. No new test framework or CMake targets.

Exact argv, working source byte hashes, compiler/library/test binary SHA256 and
timings are in each result JSON. For red-core-* the linked library's source is
the preceding named green JSON, not the modified working source hash captured at
red invocation. Labels beginning green are execution labels, not verdicts: use
exitCode and the result above. Logs/JSON are committed without newline conversion.

Invocation: wsl -d Ubuntu -u root -- python3
/mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-v2-20260924/run_fix.py
green-core-self-vote

Native Ubuntu GCC15 Debug only. Test processes use private mount/net/ipc/pid
namespaces, private /dev/shm tmpfs and loopback through the existing isolation
runner with argument forwarding. Only edge_gateway and the two named tests were
built. No device/network access outside the private test namespace, remote build,
physical writes, push or sealed-release modification.

## Wire and State Semantics

Header magic is 0x4b454332 (KEC2), version byte 2. KEC1 (including its pre-existing
version byte 2) and any other version are rejected without fallback. MQTT/API
version is unaffected. Unknown header flags are rejected. Existing authenticated
payload fields remain in order; immediately after leaderNodeId the added fields
are serialized as follows (existing network-endian u64/u32 and length-prefixed
UTF-8 string encoding):

1. senderIncarnation string; recipientIncarnation string.
2. discoveryChallenge u64; discoveryReplyTo u64.
3. heartbeatId u64; heartbeatAckId u64; heartbeatIncarnation string.
4. electionId u64; voteReplyTo u64; voteCandidateIncarnation string.
5. dispatchRequestId u64; dispatchRequestIncarnation string; authorityTtlMs u32.

The remaining old fields start with loadScore. senderBootId is retained as a
diagnostic field; it is NOT used to admit a peer. The main supplies a fresh
256-bit /dev/urandom process identity, no longer the reused OS boot_id. The
constructor's existing bootId parameter/accessor carries that process identity;
new senderIncarnation is the explicit authority/session wire identity.

Discover can solicit a Hello but cannot reset accepted sequence state, update
liveness, change term or renew authority. Hello must echo a live local discovery
challenge and this local process incarnation before admission. Replacing an
admitted incarnation retires the old identity. Other messages must pass admitted
identity, monotonically increasing sequence and type-specific correlation checks.
Membership and fixed voting-set safety remain a separate unresolved gate below.

Heartbeat rounds bind dedicated ID, local prepare-to-send monotonic time, term
and membership epoch. Enqueue time is a conservative send bound: later transport
delay never enlarges the deadline. ACKs echo the originating process and round.
The (quorum-1)-th newest peer heartbeat SEND anchor defines authority; neither
ACK arrival nor repeated ticks move it. Expired/unknown/duplicate/mismatched ACKs
cannot renew. Becoming leader through votes is provisional, with no control
until a fresh heartbeat quorum; initial activation expires at election-send+L.

Each follower ACK holds a voting promise until receipt+L+heartbeatMs. The leader's
self vote holds until its granted authority deadline+heartbeatMs. Higher terms,
StepDown and role changes do not clear these holds. A new process waits L+H after
its first local clock observation before votes/ACKs/election. Persisted term/vote
are retained; votedForIncarnation is additive. Old states without it cannot grant
another same-term vote by identity alone. Consensus/membership writes sync file,
rename, then sync parent; persistence exceptions fault the node and clear output.

Follower capability reports are locally timed dispatch requests. Targets echo
request ID/incarnation and a remaining authority duration capped by target TTL
and leader remaining authority minus one heartbeat of drift margin. The follower
anchors expiry to its request SEND time, not target receipt, and consumes that
request. activeDispatch expiry is also capped by local authority. status exposes
authorityExpireAtMs in the LOCAL monotonic domain, never a remote/wall deadline.

At most maxMembers-1 peers, 32 live entries in each challenge map, 16 retired
incarnations per peer and 256 queued outbound messages. Full challenge windows
evict their oldest entry; late responses to evicted entries are rejected. Retired
identities are not evicted: exhaustion faults closed until a fresh process with
startup hold and fresh session challenges. Maximum L=60000ms, H>=100ms. Regression
covers healthy 60s L with 100ms heartbeat/discovery/dispatch intervals.

## Coverage and Remaining Gates

Public Simulation tests cover delayed first ACK, Discover replay, explicit wire
field round-trip/version rejection, invalid correlation/term/epoch/incarnation,
delayed vote expiry, provisional leader without heartbeat quorum, follower vote
hold/restart hold, sender incarnation retirement, local delayed dispatch expiry,
bounded state, exact L boundary, five-node two-peer SEND order statistic,
2/3/5-node partition/rejoin and the self-vote old-follower overlap regression.
Existing cluster tests including loopback transport and old graph suite pass.

BLOCKED: dynamic membership currently uses an insufficient max(old,new) ACK-count
test. Separate membership stage must close it or freeze committed voting sets.
This commit MUST NOT be used as a production-safe membership implementation.

Integration-pending main changes: fresh process identity, consensus file ownership
lock, fresh timestamps after poll/input/load/send, skip expired outbound targets,
four-argument PointBridge publish(status,dispatch,wallNow,monotonicNow), initial
invalidation and scope-exit invalidation. Current points dependency still has
three parameters, so main has deliberately NOT been built or claimed passing.
Transport resource bounds and downstream snapshot/Graph/queue/driver actual-send
fences belong to separate workers and are not qualified by this stage.

Model: trusted PSK non-Byzantine peers, durable non-rolled-back state, one runtime
per identity, monotonic clocks advancing during process pauses with <=100ppm
rate error per clock. L<=60s and >=100ms promise/dispatch margin exceed relative
drift over L. Suspend that stops CLOCK_MONOTONIC, VM/time rollback, lost/restored
consensus files, compromised PSKs, and physical actuators retaining old outputs
are not covered. A hard-stopped process cannot publish invalidation; independent
deadline enforcement by downstream software/actuator watchdog remains required.

GCC6.3.1/Linaro T536 targeted ARM build, integrated main, transport/downstream
suites, dual-host virtual acceptance and physical-watchdog acceptance are NOT_RUN.
Production approval remains false. No C++17 library additions such as filesystem
or random_device; entropy, fsync and process lock use Linux/POSIX interfaces.
