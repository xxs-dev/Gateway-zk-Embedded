# Same-process bridge recovery after re-election

## Scope and integration

- Date: 2026-09-24. Branch: `fix/ems-cluster-shm11-egress-20260924`.
- Worker parent: `cb7734ee5b7ebbe589abc2b522fb8fc792113965`.
- Red test and logs: `b35a306878ce2a40c7a6eb92d48b20a0793afec7`.
- Runtime fix and first green logs: `63b0cee1cbc06c93e9be989f789e8d4893b11573`.
- This report's commit adds expanded tests and final evidence, not another runtime change.
- Coordinator integration candidate: `f2bd6f9`. Cherry-pick only this red/fix/evidence sequence, not the older worker history.
- `git diff cb7734e f2bd6f9` is empty for the cluster core, dispatch, bridge, Graph, SHM, authorization implementation and cluster/bridge headers used by this regression.
- No protocol, header, ABI, SHM layout, configuration, quorum or deadline changes.
- No device access, remote build, deployment, production SHM operation, or Grok review.

## Evidence and cause

The implementation task preserved the real failed transaction in commit
`1a296c46cdcfa2ca03f0e9e304cfa7dc8df93ea7`, under
`tests/e2e/GW-20260809-002/ems-shadow-20260924/pair-execution-one-20260924`.
Its `RESULT.md` and paired full samples show all 20 recovery observations with
valid term4 core dispatch and quorum on both nodes, but A Graph sinks invalid,
zero P/Q, strategy gate 1, reason 16. B recovered. Only B Coordinator restarted;
A Coordinator and both Compute processes stayed alive. The original failure and
its disclosed action-audit gaps remain unchanged. This local test is not a new
device acceptance result.

`EmsClusterNode::startElection()` increments the status term while invalidating,
but retaining the old dispatch term/sequence. `becomeLeader()` resets the producer
sequence to zero but also retains that dispatch until a new target arrives.
Previously `EmsClusterPointBridge::publish()` rotated the authority epoch on a
status scope change, then unconditionally recorded the retained dispatch's
sequence in the new scope. A new-term dispatch at sequence 1 was therefore
rejected against the previous term's high water mark (157 in the exact fixture).
Reason 16 is the bridge's generic Expired diagnostic, not the root cause itself.

The fix resets sequence/deadline/publication bookkeeping on changes to
`(term, membershipEpoch, leaderNodeId)`. Only dispatches matching the current term
and membership epoch can seed or advance sequence history. A mismatched dispatch
still publishes invalid authority and clears the recorded deadline: it cannot
authorize output, extend a lease, or revive an already revoked current sequence.
The next strictly newer matching sequence can recover without catching up to an
unrelated old-term number. No core election logic was changed.

## Local results

All execution used WSL native binaries inside separate mount/network/IPC/PID
namespaces via the existing `isolated_test.sh`. Runtime builds were incremental
`edge_gateway` builds only; tests were linked individually. No full 31-test run.

| Evidence prefix | Result |
| --- | --- |
| `reelection-red-20260924` | Compile PASS, output-authority test exit 1 at the intended new-term sequence 1 recovery assertion. |
| `reelection-green-20260924` | Same minimal regression PASS after only the bridge fix. |
| `reelection-boundaries-20260924` | 5/5 PASS: output-authority, strategy-startup, write-authorization, router-authority, SHM11. |
| `reelection-comparison-final-20260924` | Both expanded tests linked against the old bridge fail at their exact expected recovery assertions; current bridge passes each test three times. C++14 syntax and cluster core-only PASS. |
| `reelection-graph-20260924` | Graph cluster-dispatch suite PASS. |

The real-node regression keeps the original elected `EmsClusterNode`, bridge and
Graph engine objects alive, accumulates sequence 21, destroys only the other
node, loses quorum, self-elects, and restarts that other node from its persisted
consensus files. It asserts the new leader status still carries the old dispatch
before Graph computes the first new target. The log reports
`same-process recovery term 1 seq 21 -> term 3 seq 1`.
The first new dispatch authorizes the bridge and restores Graph validity, leader
gate and P/Q sinks. No sequence catch-up or recovery sleep is used. The preexisting
startup test's deadline sleep remains solely an expiration-negative test.

Expanded checks preserve rejection of old term, membership epoch, authorization
epoch (including equal sequence), and older same-scope sequence. Repeated sequence
deadlines cannot extend. Quorum loss, stale publications, and diagnostic-only
publication revoke output; the revoked sequence cannot resurrect. Exact deadline
expiration and new-sequence recovery are covered. Recovery Graph creates no
physical pending writes.

Each runner result JSON records commands, exit codes and hashes; logs are retained
alongside this report. `verify_reelection.py` links the pre-fix bridge object ahead
of the unchanged current static library for the controlled red comparison. It
reads the original Git object without modifying or checking out the old tree.

## Failed or limited checks

- The first comparison attempt (`reelection-comparison-20260924`) passed C++14
  syntax, then stopped because Linux Git could not resolve the Windows drive path
  in the worktree's `.git` file. Its partial result is preserved. The runner now
  reads the common Git directory explicitly; both fixed-path and final runs passed.
- `reelection-adjacent-20260924` could not compile the full `ems_cluster_test`:
  the existing worker test calls the three-argument transport factory, while this
  worker's older transport header accepts two arguments. This difference predates
  the fix; candidate `f2bd6f9` has the newer three-argument declaration. No transport
  source/header was imported or changed. The test's existing
  `EMS_CLUSTER_CORE_ONLY` mode compiled and passed, including its bridge checks.
  Full transport-loopback validation remains an integration check on the candidate.
- Native C++14 syntax checks exercise the experimental Optional path, but are not
  an AArch64 GCC 6.3.1 build or device verification.

## Affected targets

`src/ems_cluster_points.cpp` belongs to `edge_gateway` (`CMakeLists.txt:40`).
The following classification is based on source references, not a new ARM link map.

| Target | Reference and action |
| --- | --- |
| `EmsClusterCoordinator` | The sole production bridge constructor is `ems_cluster_main.cpp:252`; its loop uses the four-argument publish. Required production rebuild for this fix. Target definition: `CMakeLists.txt:285`. |
| `ems_cluster_output_authority_test` | Constructs the bridge and changes in this patch. Rebuild/retest. |
| `ems_cluster_strategy_startup_test` | Constructs the bridge, changes in this patch, covers real core to Graph recovery. Rebuild/retest. |
| `ems_cluster_test` | Constructs the bridge at `tools/ems_cluster_test.cpp:1485`. Re-link/retest on the integrated candidate; local core-only passed. |
| `ComputeEngine` | `compute_engine_main.cpp:154` calls only unchanged `addEmsClusterPointRoutes`, not the bridge publisher. |
| `MqttDriver` | `mqtt_driver_main.cpp:399` calls only unchanged route registration. |
| `MqttForwarder` | `mqtt_forwarder_main.cpp:233` calls only unchanged route registration. |

The last three production targets reference the same translation unit through
route helpers but have no changed runtime path. Default size optimization uses
`-ffunction-sections -fdata-sections` and `--gc-sections`
(`CMakeLists.txt:187-190`), allowing unused bridge methods to be discarded.
Do not infer their deployed binary hashes are identical without checking the
actual build flags/link map. For a conservative source-to-artifact refresh,
re-link these three alongside Coordinator; for behavioral scope alone only
Coordinator requires replacement. Other approved artifacts need not be blindly
rebuilt solely because they link `edge_gateway`.

## Remaining authorization boundary

Coordinator owns integration and the next fixed source/artifact manifest.
AArch64 compilation and dual-T536 recovery retest require subsequent authorization.
Implementation alone operates devices. Do not reuse the consumed execution
approval, clear its SHM, restart A to hide the bug, reduce quorum, or extend the
acceptance window. The local fix is verified; real-device recovery remains pending.
