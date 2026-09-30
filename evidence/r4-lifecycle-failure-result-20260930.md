# R4 Missing-Failure-Evidence Recovery (2026-09-30)

Base: `49440337355291f87473ec6cbbacf3e0dcf1ebd8`, branch `fix/edge-review-r1-20260930`.
Scope: local WSL x86_64 native build and disposable script namespaces only. Coordinator authorized this phase's commit after reporting an independent successful final native run; that independent output belongs to coordinator evidence, not these agent logs.

## Contract and implementation

- C++ obtains the per-job, never-removed lock inode **before intent**. New Linux intents say only `lifecycle=inherited-flock-v1`, not that a script conforms. The parent FD remains CLOEXEC. Only apply/rollback child processes duplicate the same open-file-description to FD 9 and clear its CLOEXEC. Destruction uses close, never LOCK_UN. The existing global termination behavior is unchanged; parent exit/waitpid is not a writer-death proof.
- The actual new official apply script checks the inherited descriptor's inode, the held lease, intent and active job/version/token, then fsyncs a versioned `official-ota-apply-barrier-v1` prepared record. It refuses registration with an existing in-progress marker, because the legacy interrupted-job recovery branch can itself modify live runtime. Legacy direct invocation without argument 7 remains compatible.
- Both SCADA and manifest live-write routes durably advance the barrier to `runtime-may-change` before their existing applied-job and in-progress writes and before live modification. File and directory fsync failures stop execution. The barrier is never reset or removed by success/rollback.
- New-protocol shell failures no longer write unchanged health themselves. C++ may create missing failed/unchanged evidence only under the state lock and an independently acquired lifecycle lock, with the exact prepared identity and no mutation, rollback, final or health-pending evidence. Invalid files, malformed applied-job entries, missing locks and read errors do not authorize this fallback.
- Reconciliation, synchronous catch and admission guard use the new lifecycle lock even when a health record already exists. Thus a parent exit or trap-written unchanged record cannot admit another job while a child still holds the inherited lease. The existing explicit legacy health/rollback proof paths remain; absence of a new barrier is not absence of runtime change.
- Missing or damaged prepared evidence does not suppress an independently proved synchronous changed-job rollback. The fallback has a narrow catch, preserving existing ownership/token/proof checks. Rollback runs outside the state lock with the inherited lease; competing actual execute is rejected.
- Python import availability is checked **before intent only for the exact default official path** `/opt/modbus-gateway/bin/ota-apply.sh`. This avoids occupying a job on a missing dependency; it is not a health check or a guarantee that later Python invocations will work.
- No script hash distribution/compile-time trust system, public network contract, unlock API, task engine, signing authorization or key rotation change.

## Reproduction and final verification

All final commands below exited 0. These are separate executions, not a single aggregate suite or device qualification.

| Execution | Evidence |
| --- | --- |
| Baseline missing Python, exit 1: intent present, no health/marker, fresh job rejected | `r4-lifecycle-red-python-20260930.log` |
| Baseline early process-group SIGKILL, exit 1: same unresolved outcome | `r4-lifecycle-red-kill-20260930.log` |
| Final incremental build of ota_final_state_test and ota_service_test, -j4 | `r4-lifecycle-build-verified-20260930.log` |
| Final ota_final_state_test | `r4-lifecycle-native-verified-20260930.log` |
| Final official isolation fixture, eight modes | `r4-lifecycle-official-verified-20260930.log` |
| Final ota_service_test, run from source root | `r4-lifecycle-service-verified-20260930.log` |
| Existing sync-failure, async-health and self-upgrade fixtures, all in disposable namespace | `r4-lifecycle-legacy-scripts-verified-20260930.log` |
| Changed shell syntax; PowerShell git diff --check also passed | `r4-lifecycle-static-20260930.log` |

Native coverage retains old final-state/Direct/journal, two synchronous successes, same-job race and rollback success/failure/wrong-token/async checks. Added cases cover exact prepared recovery; missing/corrupt barrier, wrong token, changed barrier, pending health, burned/damaged/non-file applied evidence, existing marker, missing lock, historical intent and damaged health staying blocked; intent-to-fork independent-lock probe and parent CLOEXEC; live children after parent exit and timeout, with and without pre-existing unchanged health; actual execute during rollback; damaged barrier not suppressing valid rollback. The intent-to-fork test is an ordered callback before fork, not killing the C++ service or cutting power.

Official modes: missing-python, early-kill, invalid-manifest, missing-helper, registration-kill, pending-kill, scada-marker-kill, manifest-marker-kill. The first four verify a fresh **actual execute** after dependency/gate repair, which reaches official malformed-manifest rejection and obtains its own durable failed/unchanged terminal. It is not merely validateRequest accepting. Natural malformed-manifest failure removes its disposable health_pending on normal exit; SIGKILL after pending deliberately does not.

Official execution command (from WSL):
```sh
unshare -Ur -m sh -c 'mount -t tmpfs tmpfs /tmp && mount -t tmpfs tmpfs /opt && mount -t tmpfs tmpfs /run && mount -t tmpfs tmpfs /dev/shm && sh /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0/tools/ota_lifecycle_isolation_test.sh /var/tmp/edge-review-r1-20260930-build/ota_final_state_test'
```

Earlier failures are retained: native-first failed because the fixture tried to obtain the exited parent's PGID; the orphaned test-only group 767 was identified and killed, then the fixture was fixed to record PGID and await child readiness before parent exit. Service's first run failed its source-content checks from the wrong working directory. Official-final failed restoring a helper with rename across /tmp and /opt tmpfs mounts (EXDEV); the fixture now copies it. These were fixture/invocation issues, not silently counted as product successes. Final verified logs are authoritative; intermediate final/final2 names are not the release result.

## Limits

- **Not the complete intent-to-marker window.** Registration-before-proof crashes remain unresolved. SIGKILL with health_pending present remains blocked even if the live-write barrier is still prepared. Historical intents and nonconforming old/custom scripts without sufficient existing evidence remain blocked, including missing Python on a non-default/custom path.
- A default-path preflight that passed cannot prove future interpreter or filesystem availability. If registration cannot complete, no absence-based recovery is granted.
- Prepared proves no modification to the live runtime by the conforming job, not zero side effects in staging, backup or watchdog paths.
- SCADA marker testing uses a **blocking installer replacement**, not the real SCADA installer. Manifest marker testing uses **Python trace** to pause the actual embedded manifest code after its durable marker and before live copy. These prove the OTA gate's ordering/refusal, not real installation/restore.
- Parent/child lifecycle and rollback concurrency tests use controlled mock scripts. Official crash tests kill the apply group while the C++ service survives, followed by a new service object; no actual daemon crash, reboot, power loss, real systemd/cgroup handoff or flash-durability experiment was done.
- Async helper success/rollback requirements are unchanged. Existing async fixtures use fake systemctl. No MQTT health claim, device, SSH, Broker, production DB, remote/AArch64/Qt build, push, merge, deployment or realese1.0 reference movement.
- This conformance protocol does not defend against malicious local root forging records/closing descriptors. Root compromise was explicitly outside this defect's threat model. No generic script trust-distribution mechanism was introduced.
- Post-run inspection found no /tmp/ota-lifecycle-* directories or matching live test children. Disposable namespace fixtures remove their own installation/temp trees. Unknown pre-existing evidence and deploy/__pycache__ were preserved.

## Exact phase commit scope

Only these six source/test/report paths and the 24 logs below belong to this phase:
- `deploy/ota-apply.sh`
- `include/edge_gateway/ota_service.hpp`
- `src/ota_service.cpp`
- `tools/ota_final_state_test.cpp`
- `tools/ota_lifecycle_isolation_test.sh`
- `evidence/r4-lifecycle-failure-result-20260930.md`

Log SHA256 values refer to actual Windows worktree bytes before staging; Git's existing newline normalization can affect later checkouts. Failed/intermediate logs are deliberately included.

| File in evidence/ | SHA256 |
| --- | --- |
| `r4-lifecycle-build-20260930.log` | `bb910c065209b5cde519294473dd39ccb047980b73802d0d252f6b72aa6b6467` |
| `r4-lifecycle-build-final-20260930.log` | `bb910c065209b5cde519294473dd39ccb047980b73802d0d252f6b72aa6b6467` |
| `r4-lifecycle-build-final2-20260930.log` | `bb910c065209b5cde519294473dd39ccb047980b73802d0d252f6b72aa6b6467` |
| `r4-lifecycle-build-testfix-20260930.log` | `72471ba656d80b63967304f4d94ae7fb73bba79875f32b0e72465d65ac094b50` |
| `r4-lifecycle-build-verified-20260930.log` | `bb910c065209b5cde519294473dd39ccb047980b73802d0d252f6b72aa6b6467` |
| `r4-lifecycle-legacy-async-20260930.log` | `153a1f500d02a81130bb69f2077073d00fb2c5b14037aa9714c079592f63927c` |
| `r4-lifecycle-legacy-scripts-verified-20260930.log` | `44bd2ddfb33dc3c403bd0567361b01663ed7242a6c1fed8f4797668c9c313543` |
| `r4-lifecycle-legacy-selfupgrade-20260930.log` | `38f02ef600b40b1652a321078442d66aa3fd03bb649eb95e33f41a8493030a91` |
| `r4-lifecycle-legacy-sync-20260930.log` | `e44957b2b1d4d2faf92bb629fd972850c5646eb1e9c0b08fc429b965d400b953` |
| `r4-lifecycle-native-final-20260930.log` | `ca7c2728dcca97c588d27cdc10f6af18bd64b23d290974fdbfcec93d4da66189` |
| `r4-lifecycle-native-first-20260930.log` | `e08ac64206c079794c03f9151d74bc18917b837a5602e1b01b966f17c1a4face` |
| `r4-lifecycle-native-second-20260930.log` | `cc09ed9602820c06bae216ff7d7b5bdf7c4ae756ac76f5234cb795861dae8a1a` |
| `r4-lifecycle-native-verified-20260930.log` | `ca7c2728dcca97c588d27cdc10f6af18bd64b23d290974fdbfcec93d4da66189` |
| `r4-lifecycle-official-final-20260930.log` | `62bee877f83ece687cb07310d320fb4860d79d9d8e91be8ecc246eda925c6ce6` |
| `r4-lifecycle-official-final2-20260930.log` | `11341607dbe90fc15ee650af38d40b21d092ff40fdfcd6c1da18c4745774212c` |
| `r4-lifecycle-official-first-20260930.log` | `98e1e3edc2a15e788a25a677d4f195a407c60fb81ce5a16d6aceae7319e74588` |
| `r4-lifecycle-official-verified-20260930.log` | `4db97d693e8021f1daa9840c255178734dec950bd3831d2f46e127f9e8de62db` |
| `r4-lifecycle-red-build-20260930.log` | `72471ba656d80b63967304f4d94ae7fb73bba79875f32b0e72465d65ac094b50` |
| `r4-lifecycle-red-kill-20260930.log` | `ce5ebd4c1fa4c28c8f5a65c02517c5aaec1782c8349968b36d89288e5c04411d` |
| `r4-lifecycle-red-python-20260930.log` | `f11bf931cd60f2e1e036bc76a439137e30f5ed0d75e5cd2b7041fe003fd1ea8f` |
| `r4-lifecycle-service-20260930.log` | `ae95f02eb682cd1da92eb73ea7dfd2cdd2122fb84c7886191d36bf4fc9c627b7` |
| `r4-lifecycle-service-final-20260930.log` | `8461524f0c4cf0cc3536c0980e0546aa7d0e449229b5ed88b78ff4a4a927f61c` |
| `r4-lifecycle-service-verified-20260930.log` | `fe1012dc8d3ec3f4504516ee3a31cec8cf7cbb5dd3a6327eaa6ced831c8eb41c` |
| `r4-lifecycle-static-20260930.log` | `6f2282ed23a95650723681e86a98494f6b64267fe05f5d7036de7a0cf6b490fe` |
