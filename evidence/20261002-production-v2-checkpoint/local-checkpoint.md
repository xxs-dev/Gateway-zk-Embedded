# Production v2 local checkpoint (2026-10-02)

Base: `57fe9e9236267d088a3224c33c7eecb884948b7b`. This is source and
private-fixture evidence only. No ARM build, board reboot, real systemd, or
business deployment was performed.

## Implemented in this checkpoint

- v2 approval binds an exact old/new configuration write set; empty write sets
  allow code-only v2-to-v2 upgrades. The existing v1 path remains separate.
- Config backups, registered partial copies, mixed-state checks, install,
  restore, and promotion use the existing production transaction journal.
  Approved new config parents are root-owned and synchronized through the
  parent binding before each live copy. Unknown directories/files are rejected.
- The guard executes SHA-pinned role-policy source bytes directly, without
  importing a cached pyc or writing one under `bin`.
- Role authority participates in the v2 boot proof. V2 health checks every
  admitted role's service state and, for qualified in-tree executables, its
  MainPID, executable digest, and required/optional SHM mappings. Roles needing
  an unpinned external executable or unmanaged child proof fail closed.
- Factory staging/installer and OTA allowlists include the role policy helper.

## Focused local results

The original command outputs are retained in this task's tool records and
coordinator review records; this file is an index, not a raw-log substitute.

- Independent guard child process: 2 PASS, `174600`. Timestamp pyc negative
  control executed OLD; pinned-source path executed NEW and kept runtime
  inventory unchanged.
- Config helper/state: root 3 PASS (`0c0030`) plus unknown empty directory
  negative 1 PASS (`7bb7d4`). An earlier non-root invocation skipped all 3.
- Formal v2-to-v2 code-only parser rollback and foreign-parser refusal:
  1 PASS each (`f94ef9`, `ba58dc`).
- Formal config add/delete/replace promotion and first-write rollback:
  2 PASS (`afed0e`); registered partial install/recover copies and foreign
  config temp refusal: 2 PASS (`64cdff`).
- Completed addition, completed deletion, and new-parent fsync failure with
  pending recovery: 3 PASS (`6fb7c0`). The directory retry then proved a
  root sync before the first new live file copy: 1 PASS (`1c9b8a`).
- V2 required-SHM health negative: 1 PASS (`5deb93`); v2 private newboot
  proof plus post-gate health: 1 PASS (`be888c`).
- Focused v1 install/health rollback: 2 PASS within `899ffd`. Factory profiles
  and paired-source cases: 2 PASS within `506928`; missing parser before a
  private systemctl shim's stop: 1 PASS (`512032`); package origin and missing
  paired member: 2 PASS (`ee9ad1`). The first factory negative asserted an
  overly specific message and failed in `506928` before correction.
- After the final directory-mode change, affected success/retry tests were
  rechecked: 2 PASS (`913f77`). Static OTA syntax without fixture setup:
  1 PASS (`048180`). Side-effect-stubbed isolation guard negatives/positives:
  2 PASS (`9ff92d`); initialized but shared namespace and private namespace
  with missing/non-tmpfs mounts: 2 PASS (`1f8f4f`).

Earlier fixture errors (`31edd0`, `43f6fc`, `e6a590`, `8b9c69`) were repaired
and are not counted as product failures or passing executions. `96a291` first
found a source indentation error; corrected run is `6fb7c0`.

## Safety incident and limits

An attempted legacy `ota_manifest_guard_test` syntax selection (`41fbfe`)
failed because its module-global `APPLY` was unset. Its `setUp` nevertheless
deletes an existing `/opt/modbus-gateway` in the default local WSL and creates
a synthetic `bin`/`config` tree. The pre-test contents were not recorded, so
this checkpoint cannot establish whether prior contents were affected. No
cleanup or further invocation of that destructive fixture was performed.
Read-only checks found current and PID 1 mount namespace both
`mnt:[4026532217]`; `/opt/modbus-gateway` is on the WSL root ext4 mount,
not a private tmpfs. Its current `bin/sentinel` and
`config/runtime/apps/sentinel.json` were created at 22:00:53 local time by
the fixture. No pre-call path inventory was found in the available checkpoint
records. The wrong-class selection immediately before it (`899ffd`) produced
two selection errors and did not reach `setUp`.

The fixture now checks initialized runner state, a distinct mount namespace,
and exact `/opt` plus `/run` tmpfs mounts before `TemporaryDirectory` or any
delete. The CLI also rejects a forged `--parent-ns` while still in PID 1's
mount namespace. A syntax-only test lives in a separate class without the
destructive setup. The guard tests use mocked namespace/mount input and
stop at a stubbed initialization boundary. Additional negative tests cover
initialized-but-shared namespaces and missing/non-tmpfs mounts. They do not
prove an actual isolated OTA run. The current WSL `/opt` scene was not cleaned
or restored.

The v2 baseline in the formal install/newboot fixture is synthetically
established after a v1 adoption; it does not prove a real v1-to-v2 migration.
The tested business role set is Monitor plus launcher. External interpreters,
stunnel, camera child ownership, watchdog, full 2/4 GiB role profiles, actual
systemd/cgroup behavior, normal MQTT/direct config publication, ordinary logic
delete-then-restore, ARM build, board behavior, and platform/Windows deployment
remain open. No final asset refresh or remote operation was authorized here.
