# Pre-Stop Owner Handoff: Local Stage

Date: 2026-10-01. EDGE role, single agent. Parent: 3c04fe85a32a582a2db4c6e3e5cfa268d0f45997.
This is an intermediate product checkpoint, not completed runtime OTA or production approval.
No device/build-machine/VM/Broker connection, product deployment or business service action occurred.
Coordinator's existing board hierarchy records were read locally, not re-probed.

## Scope And Protocol

- Trusted local applyScript basename offline-runtime-upgrade.py selects systemd-handoff-v1.
  It requires a signed/checksummed/sized full artifact. Default ota-apply.sh remains legacy.
- C++ persists job/token, artifact SHA/size and original lease dev/inode in the intent.
  Exact three helper bytes are staged and pinned. Artifact hashing reads at most64 KiB
  per iteration, checks exact size and rechecks inode/stat identity.
- Dispatch is serialized with cancellation under a job mutex; READY is durable before
  donor lease release. The independent transient unit proves exact argv, PID/start ticks,
  invocation, boot and manager cgroup. No caller-cgroup or nohup fallback.
- Owner rechecks intent/active/artifact/helper pins after acquiring the original lease.
  Current implementation always ends OWNER_BLOCKED: production baseline not implemented.
  No participant is stopped and no installation/fence/SHM operation is performed.
- Durable cancellation tombstone prevents waiting/future dispatch. Unchanged proof needs
  original lease, exact manager stop/absence, empty selected cgroup including descendants,
  and exclusion of mutation evidence. Missing record alone is not such proof.
- Health publication uses temp write/flush/fsync, atomic rename and directory fsync.
  Retry validates exact existing failed/unchanged health and repairs uncertain directory
  sync. Conflicting completed final, partial health or ambiguous proof stays unresolved.
- Reconcile/loadFinal perform bounded record reads, never Python/systemctl waits.
  Waiting/recovery stays in execute on the existing asynchronous OTA worker.
  Restarted-process management scheduling beyond explicit helper recovery is not added.

## Cgroup Proof

Verified unified v2 and named v1 systemd hierarchies are supported locally. Selection uses
mountinfo plus exact caller/PID1 controller membership and positive cgroup.procs binding.
On the observed hybrid layouts, name=systemd maps to /sys/fs/cgroup/systemd; the secondary
/sys/fs/cgroup/unified tree cannot authorize a no-writer decision. Missing, duplicate,
wrong-root, wrong-membership or substituted primary mount fails closed.
Mount filesystem/controller/hierarchy and dev/inode are pinned. Same-boot drift refuses;
a new boot may change mount identity only with exact unit absence and the original lease
plus tombstone. The current verified hierarchy is still inspected for residual tasks.
Traversal is bounded128 directories/4097 bytes per task read. Enumeration/permission/EIO
or task-read errors fail closed rather than treating an unreadable subtree as absent.

Hybrid fixtures reproduce the hierarchy/controller ordering of both local coordinator
runtime-hierarchy-23/24-20261001.json snapshots. Real systemd execution here is WSL v2;
neither hybrid unit execution on boards nor a real reboot has been validated.

## Verification

Exact commands/exit codes and developmental reds are in tests.json; final raw test logs
(WSL launch warning removed) and incremental native build output are retained alongside.

- Final integrated Python/native suite:26 tests, exit0,3.411s. Includes real systemd/RSA
  signed local-copy handoff, donor death, uncertain dispatch response, lease replacement,
  post-READY drift, missing-record recovery, cancellation races, file write/file fsync/
  rename/directory fsync faults, hybrid selection and reboot-layout substitutes.
- Actual MQTT scan + Direct GET availability:exit0; full120/realtime120,20 historical
  finals +1pending; no slow helper call or fabricated pending final. Assertions bound
  telemetry section to2s and3 GETs to1s; not a throughput/latency production benchmark.
- Privileged CTest registration:coordinator reproduced the nonroot fixture refusal.
  Product root ownership is retained; nonroot branch returns77 and CTest recognizes
  SKIP_RETURN_CODE77. Default WSL UID1000 native exit77/CTest SKIPPED, not PASS;
  root CTest actually ran and passed0.10s. Logs record both registrations. wsl.exe's
  direct wrapper mapped the nonzero child status to1; subprocess metadata proves77.
- Affected legacy native final-state suite and ota_service_test:exit0. These ran before
  the last new-protocol-only duplicate/newline proof hardening; no legacy edits followed.
- Final Python3.6 grammar and diff-check:exit0. API runtime on Python3.8 is still untested
  in this stage. No old core40/21, maintenance D2/D4, ARM or MQTT/Broker suite repeated.
- No local transient ota-runtime units or ota-owner processes remained. All local exec
  sessions completed. This stage opened no SSH session; no remote operation to close.

The earlier14-test draft PASS is not counted as coverage for the new negatives. Later
product reds and one fixture unloaded-unit exit5 error are distinguished in tests.json.
The missing-set compile error and following stale-binary run are recorded, not accepted;
the final successful build and final26-test run use the corrected source.

## Files And Remaining Gates

Only CMakeLists.txt; deploy/offline-runtime-upgrade.py; deploy/ota-health-result.py;
src/ota_service.cpp; tools/ota_final_state_test.cpp; tools/ota_replay_isolation_test.hpp;
tools/ota_runtime_handoff_test.py; tools/ota_runtime_handoff_service_test.cpp and this
small evidence directory enter the stage commit. manifest.json pins working bytes and
Git-normalized LF bytes. Unknown/untracked files are retained and not staged.

runtime-upgrade-guard.py, ordinary runtime ELF/fence policy, historical core pins/state,
configuration models, Windows and platform source remain unchanged. Ordinary manifest
OTA remains unable to cross the persistent fence or perform runtime ELF installation.
No claim is made about all OTA types or the SCADA special branch.

Production baseline, same-ABI paired install/recovery, participant stop/start lifecycle,
real HTTPS download, reboot/cold-start recovery and serial dual-board business deployment
remain required. OWNER_BLOCKED/failed-unchanged is an intentional admission refusal,
not runtime OTA success. This checkpoint is for independent source/test review only;
no push, stable-ref movement, coordinator status or shared-review edit.
