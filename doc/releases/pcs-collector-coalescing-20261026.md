# PCS Collection Planning and Selected-Task Coalescing

## Scope

Follow-up to the 2026-10-26 scheduler deployment on COMM202600108.
The preceding full-point 20 ms test achieved 29.26 requests/s, a 5.061 s
median complete 440-point sweep, and 85.73% median CPU usage of one core.
Those are gateway polling measurements, not the PCS internal sampling rate.

The reconstructed plan matched the captured block set: 69 blocks (20
realtime, 49 background), including 36 blocks of one or two registers/bits.
Increasing the batch cap from 32 to 64 would remove only one block. Address
gaps and priority separation matter more than the configured size cap.

## Implementation

- Cache readable point indices in function/address order for each collector.
  Filter due points using borrowed definitions, copying only into output tasks.
- Remove redundant priority splitting and unconditional offline-candidate
  copies. Move selected tasks instead of copying them again.
- Compute backoff, priority and oldest-value ranking once per task, then
  partially sort only the approved budget.
- Coalesce adjacent/overlapping selected tasks of the same function, within
  the existing batch cap. Only due, budget-approved points participate; no
  unapproved background work, address-hole bridging or interval changes.
  The earliest selected execution slot is retained, including the reserved
  background slot after a deferred slice.
- A mixed read consumes the background interval. Failed combined reads
  record each original task's failure/backoff and disable selected-task
  coalescing for that collector's lifetime. Individual reads can recover
  without repeatedly probing a rejected combined range. Existing within-
  priority batching and adaptive handling of ordinary tasks remain intact.

No configuration schema, transport pacing, electrical write policy, shared
memory ABI, control lease or network-service changes are included.

## Regression Coverage

The added adjacency regression failed on the prior implementation before
the source change, then passed. Coverage includes normal/focused scheduling,
background intervals and zero budgets, disabled/not-due points, address gaps,
function boundaries, batch caps, aliases sharing a register, decode offsets,
reserved background execution and failure recovery using original identities.
Existing slow-request, adaptive split, offline probing and completion-deadline
regressions pass unchanged.

Native tests use the release checkout in WSL. The migration test requires
`GATEWAY_MIGRATION_TEST_BACKUP_DIR` on persistent local storage; an unset
variable or tmpfs path is rejected as intended. Its persistent-directory
retest passed. The complete 110-test run had 108 passes, one environment
failure resolved by that retest, and one skipped legacy migration test:
109 passes and one skip after the focused retest. Four collector/daemon/
Modbus regression targets were also rerun successfully after the final edits.
Field deployment and comparison results will be recorded below
after the isolated AArch64 build and stopped/read-only verification.
