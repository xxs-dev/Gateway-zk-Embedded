# Stage 10: Legacy Event History Retention

Baseline: `f1d886279f362ba20f70b19fd371ba37b4037f9c`.
Scope: production legacy alarm history and confirmed MQTT outbox history only.
This is not whole-device disk governance or an EventStore retention rollout.

## Effective Policy

- Reuse `pointHistory.retentionDays`, default 30, clamped to 1..3650 days.
- `EventEngine` passes the global days to `SqliteAlarmWriter`.
- `ConfigLoader` derives `mqtt.eventOutboxRetentionDays` in memory from the same
  global policy. This is not a second JSON setting. EventEngine, MqttDriver,
  both MqttForwarder lanes and publisher-owned legacy outboxes receive it.
- Existing `mqtt.offlineBuffer.eventOutbox.retentionMonths` remains parseable
  and the old C++ constructor argument remains source-compatible, but neither
  overrides global days. Rebuild all binaries; binary ABI compatibility is not
  promised. No deployed configuration/template was edited.
- Processes load their own app configuration. Deployments must distribute the
  same global value to all participating app configs; this change adds no live
  cross-process config service or hot reload.

Expiry is strict `timestamp < now - retentionDays * 86400000`, not calendar
months and not time since ACK. Exact boundary and future timestamps stay.
An old offline event becomes eligible only after confirmation. An alarm history
row is a local query copy, not the reliable delivery queue. Deleting that copy
does not ACK, delete or modify its outbox event.

## Production Maintenance

- Alarm history: `alarm_events`, index `(ts,id)`, at most 512 rows per invocation.
- Legacy outbox: only `sent=1` in `mqtt_event_outbox`, index `(sent,event_ts,id)`,
  at most 512 rows per invocation. All targets use the same policy.
- Full batches continue after one second. Alarm idle checks use 60 seconds;
  outbox idle checks retain `cleanupIntervalHours` (default 24 hours).
- Both use a monotonic maintenance clock and five-second failure backoff,
  measured after database work completes. Timestamp subtraction is saturated.
- Maintenance errors are visible to the caller and do not acknowledge writes.
  EventEngine catches each maintenance error independently and continues
  processing. Idle scans run maintenance even when there are no new alarms.
- Existing outbox replay callers keep their maintenance hooks. No new thread,
  service, vacuum pass or background framework is introduced.
- Existing databases gain indexes on open without deleting data on open.
  Index creation can take time on a large existing database and must be planned
  during rollout. Bounded row count is not a hard I/O latency guarantee.

## Reliability and Capacity Boundaries

- Retention never deletes `sent=0`, including claimed rows and third-party
  pending copies. Event state tables are not cleaned by this policy.
- Existing legacy `enforceDiskLimit -> prunePendingRows` is intentionally kept:
  only unclaimed/expired-claim `event_type='change'` rows can be evicted, outside
  the newly enqueued protected IDs. Ordinary change replay remains lossy under
  pressure. This is not a promise to preserve every unconfirmed telemetry row.
- Alarm, OTA, command reply and unknown event types cannot be selected by that
  pruning query. If they fill a target budget, new enqueue fails and its
  transaction rolls back; old critical records are not deleted to report success.
- The legacy `maxDiskBytes` name is misleading: implementation accounts for
  per-target SQLite text lengths of pending topic/payload, not actual DB, index,
  history, WAL or filesystem usage. It is not a shared disk-byte budget.
- Optional EventStore already has `minFreeBytes` and `maxStoreBytes` admission
  thresholds covering its configured event/history databases and sidecars.
  They are pre-append soft gates, not reservations or a device-wide hard limit;
  ACK, reads and recovery still work when new append is rejected. Defaults of
  zero disable those gates. They were not silently copied to legacy storage.
- SQLite freed pages may be reused, but DELETE does not guarantee that files
  shrink. Physical ENOSPC remains a possible write error in legacy storage.

## Explicitly Not Expired

- `control_dedup` ordinary-control ledger and `MqttControlResultStore` retain
  their separate safety lifecycles. Neither their code nor data was changed.
- Optional EventStore journal, producer/sender receipts, state versions,
  delivery identities and projection state are retained. Legacy cleanup checks
  for the `event_store_identity` table and returns without scanning/deleting
  outbox rows. ACK does not authorize removal of immutable eventId identities.
- Optional projected alarm history remains on its existing lifecycle. It is not
  opened by the production legacy alarm writer when IPC mode is enabled.

## Remaining Work

Optional EventStore ACK does not reclaim immutable-record capacity. Safe expiry
requires a separately specified persistent dedup/tombstone contract, including
management retries and journal replay. The accepted payload ceiling (256 KiB)
versus claim budget ceiling (32 KiB), and persistent sender metadata updates on
empty claim polling, remain unresolved. None is fixed by deleting ACK rows.

No shared budget/free-space gate was added for legacy alarm history, point
history, both control ledgers, offline telemetry ring, logs, OTA downloads and
backups, recordings, packaging artifacts, or other filesystem occupants. Their
combined eMMC footprint and overload policy need a separate bounded scope.
This phase does not claim 30-day retention or automatic physical reclamation
for those categories.

## Verification

WSL local GCC/CMake build, no SSH, device execution or external services.
`EventEngine`, `MqttDriver`, `MqttForwarder` and all eight selected test targets
build and link. Tests run in a private mount/network/IPC namespace with tmpfs
for `/tmp`, `/dev/shm`, `/opt` and `/run`; SQLite databases are test-owned.

Selected CTest suite: `storage_retention`, `config_loader`,
`mqtt_event_outbox_target`, `sqlite_writer_failure`, `sqlite_outbox_failure`,
`event_engine_service`, `event_store_capacity`, `event_store_delivery`.

The new real-SQLite suite covers default/custom/clamped days, exact cutoff,
timestamp underflow, indexed query plans, 512-row budgets, idle/backlog/failure
intervals, rollback and write recovery, fanout target protection, critical-event
capacity rejection, IPC journal/receipt identity preservation, and idle
production maintenance. Existing delivery tests now require retained IPC ACK
identities instead of unsafe legacy expiry. Existing capacity tests cover
configured file/free-space admission and preserved recovery paths; the optional
`--enospc` fill-the-filesystem test was not invoked in this phase.

Local evidence: `tmp/storage-stage10/results.xml` and the isolation harness
`tmp/storage-stage10/run-tests.sh` (intentionally uncommitted evidence).
No AArch64 build, deployment, Git push, performance/capacity qualification or
whole-repository regression is claimed.
