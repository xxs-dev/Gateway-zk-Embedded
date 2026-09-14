# Realtime Admission Feedback (GW-20260914-003)

This additive candidate reuses `mqtt.statusTopic` (platform convention:
`edge/status/{machineCode}`). It does not change telemetry shapes, configuration,
full upload, storage, or command execution. No release/deployment is approved.

## Status Contract

- `realtime-session-started`: the session has been inserted or renewed. Emitted
  before the first PointStore read and before realtime publication. Existing
  `sessionId`, `meterCode`, `indexCount`, `intervalMs`, `expireAtMs` remain.
- `realtime-session-stopped`: unchanged; affects only the matching realtime key.
- `realtime-session-rejected`: a pre-admission validation/capacity refusal, with
  exactly the common envelope plus a validated explicit `sessionId` and one
  bounded `reasonCode`: `INVALID_REQUEST` or `CAPACITY_REACHED`.

Example rejection:

```json
{"service":"mqtt-driver","event":"realtime-session-rejected","machineCode":"GW_TEST","ts":1770000100101,"sessionId":"RT_example","reasonCode":"CAPACITY_REACHED"}
```

The envelope machine identity comes from the configured gateway, never from an
untrusted request. Raw requests, exception text and arbitrary reason strings are
not returned. Status delivery is best effort: publication failure does not undo
admission. A sampling/publication exception after insertion cannot become a
rejection, including an `invalid_argument` exception. Renewals acknowledge
admission without forcing an extra sample or delaying the pending due time.

## Validation And Compatibility

The existing strict `json::JsonParser` validates the whole envelope before the
existing realtime field reader runs. A 256 KiB envelope limit, depth 32 and 16384
nodes bound this extra validation; these are transport safety bounds, not an
increase to the 4096 raw selector-item limit. Duplicate root keys (including
decoded escaped duplicates) are ambiguous and discarded without status or
session mutation. Nested/quoted identity text is never used for correlation.
Malformed envelopes likewise cannot be recovered into associated errors.

Correlated invalid-request feedback additionally requires a nonempty explicit
1..128 visible-ASCII session ID accepted by the existing reader, and an omitted
machine code or one matching this gateway. Invalid/duplicate machine identity,
cross-device requests and unsafe IDs produce no correlated rejection. The old
reader's escape/identity rules are unchanged. An absent explicit session ID
keeps the legacy one-shot/default-session behavior, including exact synthetic
default-key stop handling, without inventing a rejection recipient.

All previous limits remain: 16 sessions, 250..60000 ms interval, 5..300 s explicit
TTL (30 s default), 4096 raw selector items and checked deadlines. Capacity and
request validation happen before initial point reads. Refusing a new request
does not clear accepted sessions or change periodic full upload.

`sessionId` is correlation, not publisher authentication. A valid command with
a known ID cannot distinguish its legitimate sender from someone with the same
broker publish permission. Broker ACLs remain required. Platform consumers must
also validate topic/device identity, match an unexpired lease, and ignore
rejection/started events that cannot transition its current state.

Old platforms may ignore the added event. New platforms may use started or a
valid scoped sample as admission evidence; MQTT delivery alone is not evidence.
No ACK/data means unconfirmed, not rejected or active.

## Local Verification

`mqtt_driver_service_test` contains actual `runScanOnce` request regressions:

- Admission callback updates a stored point; the first sample must see the new
  value, proving started precedes the real PointStore read.
- First realtime publication and admission-status publication throw; admitted
  sessions survive and no rejection is generated.
- Invalid fields/selector overflow with safe identity return bounded rejection;
  duplicate, escaped duplicate, nested, malformed and cross-device identities
  cannot stop or reject the real session.
- Capacity returns `CAPACITY_REACHED`; renewal, stop, expiry and no-ID behavior
  remain covered. Full upload and other-session isolation retain their tests.

Build target: `mqtt_driver_service_test`, native WSL Debug. Run both
`--realtime-admission` and the complete executable inside separate mount, IPC,
PID and network namespaces. Mask `/opt`, `/tmp`, `/dev/shm`, `/run` with tmpfs;
the default control ledger is absent at entry and cannot reach host data.
Evidence is local under `tmp/realtime-feedback-stage10/` in this worktree.
No SSH, broker/database connections, deployment or push is involved.

Verified 2026-09-14: native Debug build passed; the nine-case realtime admission
selection passed (exit 0), and the complete `mqtt_driver_service_test` passed
(exit 0). The existing default-timing assertion now locates the started event
by event/session instead of assuming it follows the sample. No AArch64 build or
device test was performed in this stage.
