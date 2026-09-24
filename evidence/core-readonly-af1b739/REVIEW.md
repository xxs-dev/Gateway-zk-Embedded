# Read-only helper / bridge / strategy review

Reviewed pinned candidate af1b739153603cb2eb80e179ba6e4adb3ad6e70e
(includes 9da496f and the 2d04ca real-socket test). Also reviewed the subsequent
a04b1c453e5679a59490d7e0dd3ae62ffb8c23a9 delta: only startup fixture provisioning.
No product code or another worker's test was edited. Probe and evidence are kept
in the core worker's own evidence directory. No remaining actionable product
finding was confirmed in the requested scope.

## Checked boundaries

- src/cluster_write_authorization.cpp:68: command and snapshot both require local
  kernel boot, matching nonzero authority epoch, exact dispatch sequence, target
  and store scope; both absolute deadlines independently use exclusive now < end.
  Extending the current snapshot cannot extend an already queued command.
- src/ems_cluster_points.cpp:249: publication first invalidates the shared snapshot;
  unchanged dispatch sequence retains the earliest deadline, and invalidating it
  prevents resurrection on a subsequent longer lease. A new publisher clears both
  gates and generates a fresh authority epoch. No new revival path reproduced.
- src/ems_cluster_points.cpp:278 and src/cluster_write_authorization.cpp:61:
  strategy uses an independent leader lease plus local boot, not dispatch validity.
  A leased leader with no target can calculate its first target without granting
  physical command authority. Exact strategy expiry and foreign boot reject.
- src/graph_ems_engine.cpp:5421: configured cluster gate reads the atomic snapshot
  before sampling monotonic time, so delayed reads do not receive a fresh TTL.
  This checks only the delivered strategy/snapshot consumer, not unfinished Graph
  output lineage or driver enforcement owned by other workers.

CORE-STAGE's historical membership blocker is superseded by FIXED-VOTERS-STAGE:
identical complete offline roster/epoch, no automatic bootstrap or online changes.
The tested model still requires trusted non-Byzantine PSK peers, durable non-rolled-
back state, unique process identities and advancing monotonic clocks within the
documented <=100ppm bound. Suspend/clock rollback, contradictory offline rosters
and physical retained outputs are not newly solved or claimed by this review.
The coordinator's supplied main native build is acknowledged; this review did not
rebuild main or perform ARM validation.

## Reproduction and actual results

```powershell
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/core-readonly-af1b739/run.py
```

run.py archives the exact pinned commit into a private native cache, builds only
edge_gateway with two jobs, then compiles/runs the named tests in private
mount/net/ipc/pid namespaces with tmpfs /dev/shm and private loopback. No candidate
worktree/build directory is written. Exact commands and binary/library SHA256 are
in result.json. Source provenance is the immutable archived Git commit.

- Library: exit0 / 30.890s.
- cluster_write_authorization_test: exit0 / 0.009s.
- ems_cluster_output_authority_test: exit0 / 0.033s.
- Original af1b739 ems_cluster_strategy_startup_test: exit1 / 0.009s,
  `KECP/2 requires a complete pre-provisioned fixed voting membership`.
  Its lines 47-52 construct enabled nodes without providing the required roster.
  This was a test integration defect, not a strategy product failure.
- Independent provisioned_startup_probe: exit0 / 0.066s. It includes the candidate's
  original test body unchanged and only supplies identical two-node fixed voter
  files beforehand. It exercises no-target startup through actual core, bridge,
  Graph and back to core dispatch, plus boot/expiry/publisher-restart rejection.
- During review, coordinator commit a04b1c4 added this required provisioning to the
  original fixture. That delta was read and is not reported as an outstanding
  finding. The unmodified a04b1c4 binary was not separately rerun here; the pinned
  probe run above is reported distinctly, not mislabeled as that test's PASS.

The first runner invocation stopped before archive/build because WSL git cannot
interpret the Windows worktree .git pointer. The runner was corrected to use the
existing absolute common Git directory. This was not a product build/test failure.

No Graph-lineage/driver-delivery gap is counted as a new finding; both are outside
this assigned read-only review. This is not a production or ARM acceptance report.
