# EMS Lease Worker Result (2026-09-24)

## Scope and provenance

- Base: `7e392cec6218244b0a230a5f0d58c96888fd51de`.
- Branch: `fix/ems-lease-worker-20260924`.
- Worktree: `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924`.
- Red commit: `8733db3f7e384281cea15c000e7675c1b839a35c`.
- Green source is the commit containing this report, a direct child of the red commit.
- Reused the paused worker's 24-line public-API overlap regression and its runner,
  changing only the runner's private build-cache path. Did not edit that worktree.
- Read fixed-source evidence from commits `90fe23d` and `32c5d7e`, including
  the probe trace interpretation and exact 775 ms effective-leader overlap.
- Product changes: `src/ems_cluster.cpp`, `src/ems_cluster_dispatch.cpp`.
- Regression changes: `tools/ems_cluster_test.cpp`; remaining changes are this evidence directory.
- No wire/header/config-schema change, distributed-system framework, device access,
  physical writes, remote build, push, release build, or writes to `D:/workspace/realese1`.

## Implementation and compatibility

Tick no longer advances lastQuorumMs using retained ACKs. An accepted HeartbeatAck
updates its peer receipt timestamp; the (quorum-1)-th newest peer ACK anchors the
lease, counting the leader's self vote. In five nodes this requires two peer ACKs,
not just the newest one. The election's initial majority lease remains unchanged.
ACK timestamps are cleared on becoming leader and committing a membership epoch.

ACK renewal requires current term, this leaderNodeId, current membershipEpoch,
voting membership, compatible configuration, and an already-live local lease.
Ordinary duplicate sequences remain rejected by the existing receive filter.
An ACK arriving at or after expiry cannot revive expired leader authority, even
before the next tick. Tick steps down before generating another heartbeat/dispatch.

**Intentional boundary change:** lease validity changes from age <= L to age < L.
At exactly last quorum ACK + L, quorumValid, controlActive and activeDispatch.valid
are false; accepted power is zero. The same predicate gates dispatch generation.
Follower heartbeat leases use the same exclusive boundary. Role can still read
Leader before the next tick, but public effective authority is already false.
activeDispatch can now report NoQuorum earlier than the target's independent TTL.
No transport field is repurposed and no KECP/1 wire version changes. Old peers can
exchange existing frames, but an unpatched old leader retains the original defect;
mixed-version operation is NOT a safety qualification.

## Actual tests

Existing Simulation and public APIs only; no private/public visibility macro.
Runs use WSL Ubuntu Linux x86_64, native `/usr/bin/c++` GCC 15.2.0, Debug.
Only `edge_gateway` and the two named test programs were built. Test processes
run in private mount/net/ipc/pid namespaces with mount propagation private,
tmpfs /dev/shm and private loopback. Temporary test working directories were
removed by the existing runner; namespace lifetime ends with each test process.
The private build cache remains available for evidence inspection.

| Stage | Compile/build | Test process result |
| --- | --- | --- |
| red-overlap | compile 0, 1.821 s, fixed-source library | exit 1, 0.019 s: partition must not overlap effective control leaders at 775ms |
| red-boundary | compile 0, 1.721 s, fixed-source library | exit 1, 0.018 s: public authority must expire exactly at lastACK+L, even before tick |
| green-final | configure 0, 2.923 s; incremental library 0, 0.266 s; test compiles 0, 1.671/1.480 s | ems_cluster_test exit 0, 4.177 s; graph_ems_cluster_dispatch_test exit 0, 0.116 s |

Initial native library compilation is recorded in green-initial (19.212 s).
green-initial and green-expanded are intermediate snapshots, not final-source claims.
The unchanged original suite cases remain in main and ran on final source. Suite
logs report whole-suite success, not separately timed per-case results.

Added regressions cover:

- Original three-node effective-control overlap sampled every 25 ms.
- Exact lastACK+L-1, lastACK+L, lastACK+L+1 getter boundary before tick;
  no local sequence advance or new outbound DispatchTarget sequence after expiry.
- Two-node safe stop, three/five-node minority fencing, majority takeover and
  rejoin, with authority agreement and at-most-one quorum leader sampled every 25 ms.
- Five-node minority continuing to receive one peer's ACK still expires.
- Five-node explicit ACK order statistic: receipt times T+100 and T+200
  expire at T+100+L; a single new peer ACK does not move expiry beyond T+L.
- Wrong term, wrong leaderId, wrong epoch, ordinary duplicate ACK and a valid-context
  ACK delivered exactly at expiry cannot renew the original lease.

Invocation (run from Windows PowerShell; label selects evidence filenames):

```powershell
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-lease-20260924/run_fix.py red-overlap
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-lease-20260924/run_fix.py red-boundary
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-lease-20260924/run_fix.py green-final
```

The red invocations were executed on their then-current test snapshots before
the product edits. Each result JSON contains exact compiler, build and unshare
argv, exit codes, elapsed times and source/library/binary SHA256. Do not rerun
these labels over the sealed evidence files when doing another verification.
`git diff --check` passed. No Grok or super-power was used.

## Final SHA256

| Input/output | SHA256 |
| --- | --- |
| src/ems_cluster.cpp | 1da60ed0a5ad9f154e114f473dc7cfb1defdb96d971063f5ace9b697bf35aa05 |
| src/ems_cluster_dispatch.cpp | 649945bd75b1ea8457613f0143328c73c81d538dcb8a488504d12f8ce82bd3c9 |
| tools/ems_cluster_test.cpp | dbf4c7fe8a69bb28692c4a599d256b2f21aa355bd6bae2624a78868cf36d0b5d |
| include/edge_gateway/ems_cluster.hpp (unchanged) | 97039d198f3725ae4af00ca9f9b61a516d4979a86cf8eb93c266aa43398a2b03 |
| tools/graph_ems_cluster_dispatch_test.cpp (unchanged) | c8aed6df92e5bc0ff20e00ad469bd80b034db19b19b5d0016a5bd6f0a2afb872 |
| CMakeLists.txt (unchanged) | 86d456538feb6812b8cd9deb72caa9ea3af890e00e8c627384425da07ca58da2 |
| final libedge_gateway.a | 97a42b9a84b67625ae760450f8ccdf9fb75dd70ae5f600245048216afb4dcdcc |
| final ems_cluster_test | d6d2ff12509585d04bb3dd388605c9c3b5f896499cfcc8dedc07543c3214d1a6 |
| final graph_ems_cluster_dispatch_test | cee24e65b3cb6b0def8a83286382fa64205b1f73d6741ad10e4d2e2412251b7a |
| fixed-source library used for red | 385b538253e5a7efd82d845da035ad7afba9533c2b120fc2e405ded14da4869f |
| reused isolated_test.sh | 2586eca35ec174ddc40d52d6fa16331e8fddcd1fdf786e9e087e84a2079b7b11 |
| run_fix.py | 63875dbd21fbd3e8fcbd5f5f24e3b0b55d4f5c8649741f9f2c5ab890de22c035 |
| final CMakeCache.txt | e16807fb5bb47b456a874135f19d2be5a93c4ad14d1de76f238c9172f04d6636 |

Source hashes describe tested worktree bytes, including their existing line endings.
Final cache is `/home/wmzdxs/.cache/edge-cluster-lease-worker-20260924`;
CMAKE_HOME_DIRECTORY points to this worker's worktree, not the paused worker.

## Residual safety risk / acceptance block

This fixes repeated reuse of already-received ACK evidence. It does NOT establish
general asynchronous-network split-brain safety. HeartbeatAck has no originating
heartbeat challenge, so a delayed first ACK received while the local lease is
still live can extend authority from its late receipt time. Current term,
leaderId and epoch checks cannot identify that delay. The generic sender sequence
does not correlate an ACK with its originating heartbeat, and Discover can reset
the existing sequence filter; pre-expiry Discover-assisted replay remains a risk.
The new expiry guard prevents revival only once authority has actually expired.

Full freshness/fencing needs a separately reviewed protocol change (explicit
heartbeat correlation and appropriate timing/fencing semantics), not silent use
of an unrelated wire field. Delayed votes, restart/replay and downstream physical
actuator fencing are not broadly qualified by these local partition simulations.
No physical or device acceptance was run. Physical acceptance and production
approval remain BLOCKED/false; final green qualifies only this narrow repair.
