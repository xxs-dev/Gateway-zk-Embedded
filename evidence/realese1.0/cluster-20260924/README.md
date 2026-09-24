# Fixed-source EMS Cluster Local Evidence

## Stage 1: Existing Suites

Source: 7e392cec6218244b0a230a5f0d58c96888fd51de, unchanged tracked product files.
Evidence branch: evidence/realese1-cluster-20260924.
Execution date: 2026-09-24. Exact UTC save time, argv, SHA256, elapsed time and
real process exit codes are in fixed-result.json and fixed-provenance.json.

Only ems_cluster_test and graph_ems_cluster_dispatch_test plus their CMake
dependency library were built locally as Linux x86 tests. No release target,
remote compiler, device, deployment, or sealed-package modification was used.

| Step | Exit | Seconds |
| --- | ---: | ---: |
| Configure fixed worktree | 0 | 4.576 |
| Build two requested targets and library | 0 | 22.319 |
| ems_cluster_test | 0 | 4.127 |
| graph_ems_cluster_dispatch_test | 0 | 0.117 |

The cluster main invokes 23 existing cases sequentially; their names are in
fixed-provenance.json. The unmodified executable prints one suite-level success,
not 23 individual pass events. Coverage includes authentication/nonfinite frames,
3/5-member formation, 3-member majority partition, 2-member split-stop, expanded
quorum, cabinet assignment, persisted restart/corruption, duplicate identity,
load penalty, weighted allocation, interlock, expired/replayed targets, point
bridge and real loopback Ethernet transport.

Graph coverage includes accepted/clamped virtual outputs, invalid/stale zeroing,
leader gate, direct-engine zeroOnInvalid=false semantics, malformed phase count,
invalid power-constraint state, restored-output timestamp, and fractional legacy
PCS writeback inside the isolated test. This is not permission for physical writes.

Each suite ran in private mount/network/IPC/PID namespaces, with private tmpfs
/dev/shm and loopback enabled. Processes exited; their private SHM disappeared
with the namespaces. No physical interface was available to these test processes.

legacy-* captures were completed before the coordinator's instruction to omit
unproven old binaries arrived. They are retained as history only: the 2026-09-06
build cannot prove fixed-source behavior and is excluded from acceptance.

## Acceptance Limits

PASS is limited to these tests, native Linux x86, and their assertions. It is not
AArch64 or dual-device acceptance, Raft certification, or proof of no split brain.
The partition cases assert state after a settling interval; they do not exhaust
the intervening lease/election timing window. An authorized small Stage 2 probe
will separately record role overlap versus effective control/dispatch overlap.

Two physical voters require both votes: a 1+1 split or one failed node must stop
control. A single surviving backup cannot provide majority-based takeover.
Three/five-member simulations do not turn the proposed two-device run into a
three-device high-availability test.
