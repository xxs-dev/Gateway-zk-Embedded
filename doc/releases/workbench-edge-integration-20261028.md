# Workbench edge integration — 2026-10-28

Integrated the workbench edge changes with upstream `realese1.0`
`d274666092daf3106f0438281a92c957158d8923`. Merge commit: `b02d142`.
Final tested source: `9b4272e` (subsequent change is this report only).
The independent Qt desktop repository and `feat/maint-v2` are outside this merge.

## Changes

- Bounded serial/TCP channel tracing using the existing maintenance authentication.
- Authenticated observation of loaded strategy graphs, effective execution settings,
  and original command receipts; manual recovery and confirmed-control safeguards.
- Read-only production configuration inspection without exporting protected contents.
- Snapshot expiry and truncation metadata, optional monitor point publication,
  bounded MQTT realtime batching, and large point-table parsing improvements.
- OpenSSL 1.1 compatibility for signed OTA queries and read-only query support when
  the gateway has no device routes.

The three textual merge conflicts were additive: CMake test registrations,
maintenance includes, and maintenance test entry points. Both sides were retained.
Upstream strict full-telemetry store checks and runtime discovery remain intact.

## Verification

On 192.168.22.11, an isolated source snapshot was used; the shared checkout was
not modified. All 3,082 tracked source files matched the final local source hashes.

- Native Linux and Allwinner AArch64 GCC 6.3.1 builds passed for SystemMonitor,
  ModbusRtu, IecDriver, MqttDriver, and ComputeEngine.
- Nineteen selected native regression groups passed: channel trace/cache API,
  command receipts, confirmed control, manual recovery, realtime snapshots,
  maintenance contracts, monitor service, MQTT service/publisher, numeric JSON,
  collector partial failures, Modbus codec, OTA query crypto, compute service,
  configuration parsing, loaded graph observation/HTTP, and production inspection.
- Production inspection covered 29 positive and rejection cases, checking that
  runtime files remained unchanged.
- Tests ran with private network, PID/proc, temporary-file and SHM namespaces.
  The test harness initially omitted a private proc mount; that was corrected.
- The coverage fixture now supplies its own identity, matching upstream strict
  discovery. The production inspection fixture preserves the host syscall ABI
  during atomic file publication while simulating ARM admission, including on
  older glibc; its hard-link check supports Python 3.6.
- Five ARM64 artifacts were copied back and every member SHA256 was verified.
  Artifact archive SHA256:
  `0a542b3d1657d83dd02657d87755c216aa026228151dc38e571a7cf485e7f63f`.
  Validation log archive SHA256:
  `89f323ee52f8a6559602e8c0380f6a050292f66f6cd61dab9aec5981f790f6fd`.

Local evidence: `outputs/edge-merge-20261028` in the redesign workspace, outside
this repository. Initial failures and corrected runs are both retained.

This is targeted merge verification, not full production acceptance. No edge
device was contacted or deployed in this task. ARM64 binaries were compiled,
not executed. The upstream known `ota_final_state` limitation documented in
`branch-consolidation-20261028.md` was not retested or claimed resolved here.
