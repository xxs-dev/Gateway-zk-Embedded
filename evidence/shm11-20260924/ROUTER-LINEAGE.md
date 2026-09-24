# Router, configuration and Graph lineage stage

Status: local native regressions passed; production integration remains pending.
Branch: fix/ems-cluster-shm11-egress-20260924.
Base: bf9c364 (fixed-roster startup fixture). Core commits through 862f1b5 have
been imported without manually editing core-worker files.

## Behavior

- controlTargetIndexes parses at most 256 unique positive uint32 integers;
  malformed, duplicate, fractional and overflowing values are rejected. Empty
  scope while enabled for control means all graph-ems writes stop.
- An enabled authority store name is 1..63 ASCII letters/digits/underscore/hyphen.
- Router checks current authority before scalar/queue work and again before
  enqueue. Any attached token is checked regardless of source, zero value or
  highPriority. MQTT controlGeneration remains independent and is preserved.
- A batch containing an expired command is rejected before enqueue. Actual send
  must still recheck each command after any waits: ingress is not a send fence.
- Tagged commands cannot pass through virtual parameters or mailbox paths that
  would discard authorization. Untagged ordinary MQTT behavior is unchanged.
- clusterDispatch attaches the atomic dispatch token only to its six phase values.
  Validity, reason and station-strategy boolean outputs are not write tokens.
- In-scan PointSnapshot lineage survives stateless formula (including clamp) and
  powerConstraint only when every dynamic input has the identical token. Literal
  constants need no token. Missing/mixed input lineage produces no output token.
- controlWrite copies its actual value input token, never a permit/global token.
  Each scan clears lineage; SHM scalar reads, persisted/restored values and other
  processes cannot acquire a token by being reread in the current scan.
- Other/legacy/stateful nodes do not propagate tokens in this stage. Their
  protected writes fail closed. Dynamic untagged sensor bounds also stop lineage;
  deployments must use an explicitly supported graph, not assume old graphs work.
- Authority-store read failures report an error and clear both Graph gate outputs,
  rather than leaving the previous scalar station-strategy gate active.

## Verification

Final evidence: green-router-lineage-final-result.json; all 16 suites exit 0.
Native tests run in private mount/net/IPC/PID namespaces with no device access.

| Suite | Seconds |
| --- | ---: |
| ems_cluster_strategy_startup_test | 0.367 |
| cluster_router_authority_test | 0.033 |
| cluster_config_authority_test | 0.017 |
| cluster_write_authorization_test | 0.009 |
| ems_cluster_output_authority_test | 0.034 |
| config_loader_test | 0.066 |
| graph_ems_cluster_dispatch_test | 0.117 |
| graph_ems_v2_loader_test | 0.316 |
| graph_ems_voltage_quality_test | 1.469 |
| compute_engine_service_test | 0.066 |
| point_store_router_retention_test | 0.116 |
| point_store_router_normalize_test | 0.034 |
| memory_point_store_shm11_test | 0.067 |
| memory_point_store_v11_migration_test | 0.217 |
| memory_point_store_migration_test | 1.620 |
| priority_control_lease_test | 0.010 |

Use raw JSON receipts for exact measured durations and source/binary hashes.
RED receipts cover router missing-token rejection, ignored config scope, missing
Graph lineage, and stale strategy gate after authority-store failure. The earlier
green-router-lineage-stage label is not fully green: V2 loader lacked the tools
testdata path in the isolated fixture. The runner fixture was fixed, that suite
was rerun, then the final full set above passed. No product change hid that error.

The startup suite runs real fixed-roster core nodes, four-argument bridge, actual
Graph station strategy, dispatch, multiplication/clamp, queue, and a fake actuator
whose call is preceded by ClusterWriteGuard. It checks previous-scan scalar plus
fresh permit, mixed scalar/authorized arithmetic, between-send revocation, delayed
drain after coordinator stall, boot mismatch, monotonic deadline and restart.

## Remaining Gates

Transport worker owns driver/main config binding and actual-send guards; these are
not implemented or verified by this stage. Coordinator must merge and test that
combination. No ARM/Qt release compilation, device tests or deployment were done.
All point-store consumers must be rebuilt together; see ARM-AFFECTED-TARGETS.md.
PCS watchdog/retained setpoint behavior still requires field acceptance. Sealed
realese1 and fixed realese1.0 directories were not modified.
