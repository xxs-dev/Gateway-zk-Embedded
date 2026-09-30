# R4 synchronous OTA failure result (2026-09-30)

Branch: `fix/edge-review-r1-20260930`, starting HEAD `bb8ee81`. Local WSL x86_64 native and isolated script fixtures only. No device, Broker, production DB, AArch64/Qt package, push, merge or deployment.

## Reproduction and change

- Initial `ota_final_state_test` red after adding an execute-level synchronous rejection case: `ota final-state test failed: proved unchanged rejection must allow a new job` (native exit 1). The original catch attempted rollback without a runtime-change proof, then left the intent unresolved. The initial red output was observed in the command result; the real script rejection output is retained in `r4-sync-script-20260930.log`.
- `ota-apply.sh` now writes a fsynced `failed/unchanged` health decision under `ota_state.lock` only when its jobId, version, token and current intent/active marker match and neither `ota-in-progress` nor `applied_jobs.txt` shows that the job entered runtime modification. A missing helper does not prevent this pre-change decision. The intent remains; the original jobId is single-use.
- `OtaService::execute` compares its own generated token with the durable intent and active marker. A changed synchronous failure is rolled back only with an owned in-progress marker, a burned jobId and no `health_pending.txt`, health result, prior rollback marker or proof. The rollback shell call occurs outside `ota_state.lock`; it must return successfully and leave an exact, fsynced jobId/version/token proof before C++ records `rollbackSucceeded=true` and finalizes. Existing verified health results are not overwritten. Unknown, corrupt, mismatched and async-owned outcomes retain the intent and block another job.
- `ensureNoUnresolvedJob` allows verified healthy completion, direct synchronous completion without health, and only safe failed outcomes (`unchanged` or rollback `succeeded`). Failed/skipped rollback may be terminal for status reporting but remains blocking. Intent is checked again under the state lock before write, preventing a delayed duplicate from replacing an earlier final intent.
- The active marker is retained after a safe final. This is intentional: the helper requires the exact active token and a health file may be created only once; the next job's activation replaces the marker after validating the prior safe final. A late old helper cannot update a newer version.
- `OTA签名说明.md` now says intent occupation is the retry boundary. A missing status query alone never authorizes re-send; safe final then fresh jobId and new signature are required. The stale `activating` and pre-health `completed` descriptions were corrected.

## Verification

All commands below exited 0 unless noted. Raw outputs are in the named evidence logs.

- Incremental native build: `cmake --build /var/tmp/edge-review-r1-20260930-build --target ota_final_state_test ota_service_test -j4`; `r4-sync-build-20260930.log` (the separate earlier `ota_service_test` build is in `r4-ota-service-build-20260930.log`).
- `ota_final_state_test`: `r4-sync-native-20260930.log`. Its fake apply/rollback scripts cover unchanged failure and next actual job, Direct terminal lookup after MQTT journal clear, two consecutive synchronous successes, an ordered delayed same-job race, changed failure with matching proof, wrong-token proof, failed rollback, and `health_pending` preventing duplicate synchronous rollback. These are C++ service-path tests with mock shell scripts, not proof of live official-script restoration.
- `ota_service_test`: `r4-ota-service-test-20260930.log`; signature/checksum and pending journal behavior retained.
- Official `ota-apply.sh` in disposable `/opt` mount: `tools/ota_sync_failure_test.sh` under `unshare -Ur -m` with tmpfs `/tmp`, `/opt`, `/run`, `/dev/shm`; `r4-sync-script-20260930.log`. Malformed manifest and missing `ota-health-result.py` both fail before runtime modification, persist matching `unchanged` evidence, keep the installed sentinel unchanged, and leave no burned jobId or in-progress marker.
- Official apply/rollback/helper existing fixture: `tools/ota_async_health_test.sh` in the same isolation; `r4-async-script-20260930.log`. The unhealthy restart path executes the official `ota-rollback.sh` with the sixth token argument; the fixture checks its exact jobId/version/token proof against `health_pending.txt`. It also retains prior success, activation-timeout, interrupted recovery and late-helper checks. This is fake-systemctl local verification, not device health evidence.
- `sh -n` for changed shell scripts, Python source `compile()` for the helper, and PowerShell `git diff --check`: exit 0. The combined WSL static command returned 1 only because Windows worktree `.git` cannot be resolved by WSL `git`; the tests before that last Git command passed, and `git diff --check` was rerun successfully in PowerShell.

## Limits and cleanup

- No real systemd, MQTT connectivity, hardware health or cross-compiled package verification. Synchronous failure after `health_pending.txt` without a verified health result intentionally stays unresolved; there is no automatic retry or open-ended unlock. An operator recovery/reconciliation procedure for insufficient evidence remains to be designed separately.
- The script fixtures removed their own disposable install and temp trees. A post-run `/tmp` search found no `ota-final-state-*`, `ota-sync-*` or `ota-async-health.*` fixture directories. Existing untracked history/evidence/cache in the worktree was left untouched.
