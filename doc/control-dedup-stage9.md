# Ordinary External Control Deduplication

## Scope and Identity

Ordinary MQTT and HTTP Direct physical point controls are registered by trusted
server code. The server decides using the configured route type, not the client's
source string, ID prefix or a JSON durable flag. The shared queue carries the
trusted durableControl bit in one previously reserved byte; highPriority and
controlGeneration retain their existing meanings.

The durable key is (machineCode, cmdId), across all meters, indexes and driver
routes on that gateway. meterCode, index, value, full source, highPriority and
the router-authorized effective controlGeneration are immutable content. Request
and acceptance timestamps are deliberately excluded. A second wire fingerprint
uses the actual shared-memory source prefix (31 bytes); the full source is still
compared at admission. Durable IDs must contain 1..63 non-NUL bytes and cannot be
blank. Internal unflagged controls retain their former ID handling.

HTTP Direct preserves ordered, independent batch items. Missing cmdId produces
a fresh random server ID returned in the response. This is a new operation, not
cross-retry deduplication: callers must retain and reuse the returned ID. An
explicit empty or overlength ID is rejected. Reusing an ID on a different meter
or index is a conflict; use a new ID for a new operation.

## Storage and Outcomes

Every device config must use the same absolute memoryStore.controlDedupPath.
The default is /opt/modbus-gateway/data/control_dedup.db. Router construction
validates the loaded device paths in a linear preflight; Gateway and CAN use the
same device configuration on consumption. Configuration rollout must keep all
producers and consumers consistent. A flagged command with no matching record
fails closed, without reverting to ordinary execution.

The standalone SQLite table uses synchronous=FULL and rollback-journal commits:

- ready: immutable ingress binding, not yet reserved for device dispatch.
- pending: exclusive atomic claim committed before the executor/CAN send call.
- done: saved final outcome. Completed repeats replay that outcome.

Only the transaction that changes ready to pending may dispatch. Pending is never
reclaimed on process restart, exception or elapsed time. A ready entry may be
queued again because no dispatch reservation has been made yet.

Exceptions after reservation, executor failures that cannot establish non-write,
and failure to save a post-write result return success=false and the existing
writeback-timeout stage. No exactly-once device effect is promised: the guarantee
is at-most-once executor dispatch per retained reservation. Protocol-internal
behavior, acknowledgements and physical effects are not an exactly-once system.

Durable callers only accept final receipts from SQLite. While ready/pending, or
if SQLite cannot be read, initial requests keep waiting until their existing
timeout. They never treat an earlier shared-memory busy failure as proof that a
later queued duplicate has not started. Repeated admission of a pending record
returns writeback-timeout without redispatch. Busy/full/unavailable storage cannot
permit an unreserved write. Existing completed records are queried read-only
before attempting insertion, so a full ledger does not prevent receipt replay.

The table is capped at 100,000 operations; machine/meter IDs are capped at 128
bytes, command IDs at 63, source at 256, and saved messages at 127. No safety
records expire or participate in the point-history 30-day policy. Capacity
exhaustion rejects new external operations; internal closed-loop commands do not
create records or perform ledger IO. Changing device identity or ledger path,
deleting the ledger, or restoring an older ledger can invalidate deduplication
and is not a routine maintenance action.

## Preserved Independent Paths

EMS virtual parameters retain their existing atomic local persistence and
local-parameter-committed receipt. AGC/AVC command mailboxes retain their original
source/runtime checks and mailbox-committed receipt. They are not covered by
ordinary physical-control deduplication. MQTT keeps the existing in-memory TTL
behavior on nonphysical paths; ordinary physical repeats reach the ledger.

Internal graph/AGC controls, internal atomic groups, startup writes and callers
that execute outside GatewayDaemon::processWritebackOnce or
CanDriverService::processWritebackOnce are outside this external-control contract.
Local processes with write access to the queue/database remain trusted.

## Release Migration Gate

This source change is NOT directly deployable onto existing v8/v9 segments.
New segments default to v10; older Linux binaries reject that version. New code
continues to read v8/v9 data, but rejects durable enqueue on those segments.
Startup cleanup now preserves v8/v9 segments even when no owner is active.
allowLegacyRemoval exists solely for deliberate cleanup of test-owned segments;
production startup does not enable it.

No automatic deletion, clearing, reset or version promotion is supplied. No
reusable offline preservation migration tool was found in the current tools.
Release requires a separately reviewed, controlled offline migration with all
participants stopped, verified backup/restore of latest/offline placeholders,
history and ownership structures, and matching participant versions afterward.
Do not substitute segment deletion for that migration. Source/test acceptance
and release approval are separate gates.

## Local Verification

tools/run_control_dedup_regressions.sh is run inside a fresh user, mount, network
and IPC namespace. Its /opt bind mount and /dev/shm tmpfs are private; loopback is
the only network interface enabled. New fixtures use explicit temporary ledger
paths. Tests use real SQLite, actual Gateway/Modbus, DLT, DIO and IEC executors
with fake hardware, and the actual CAN UDP test send path. They do not require a
device, external broker, SSH or external database.
