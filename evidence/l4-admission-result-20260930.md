# L4 Duplicate Request Admission (2026-09-30)

Base: L3 commit `c3e643e`, branch `fix/edge-review-r1-20260930`. Coordinator reviewed this report and scoped changes, independently matched manifest hashes 36/36 and source hashes 10/10, and approved the scoped local L4 commit only. Coordinator reports its independent default native run exited 0 in 9.69s (tool chunk `9c78dc`); that raw evidence belongs to coordinator, not these implementation logs. Approval does not authorize push, merge, product realese1.0 reference movement or deployment.

## Contract

- `OtaRequestRejected` distinguishes `Duplicate` from `NotAdmitted`; no exception-text matching. A bounded structured JSON root (depth 8, nodes 128) supplies both identity and complete OTA request. Duplicate root keys/wrong identity types reject without guessing correlation; nested identities cannot override root fields. Size/timestamp integers outside +/- (2^53 - 1) reject instead of silently rounding. Direct omitted package/size/upgradeMode defaults remain and are tested by actual execute.
- After checking the local target and safe root ID, recognize already-used IDs or live same-ID leases before validating other fields. Linux execute acquires the existing per-ID lifecycle inode before accepted/download. An unused, released lock alone does not burn the job ID; intent remains the single-use boundary. No lock files are deleted.
- MQTT proven duplicate, including malformed SHA/URL with an identifiable original ID, is audit-only: no positive/negative reply and no newly synthesized task status/history. The shared reply only has jobId; a duplicate negative reply would fail the original platform task and may revoke its grant. Existing trustworthy status replay remains permitted; no new success/accepted status is invented for a duplicate.
- New invalid requests and different-ID pre-admission busy refusals still produce negative MQTT reply, not task failed. Accepted reply/status originate from the owned execute callback, never the ingress preflight. Worker pre-admission NotAdmitted still replies negatively; worker Duplicate only audits. Generic worker catches never synthesize task failure from request ID.
- Genuine owned signature/download/application failures still use OtaService's durable failure paths. Pre-intent failure retains its lease through failure history, status and callback; failed-callback same-ID reentry is Duplicate. Owned post-accepted distinct-job admission loss reports its OWN durable failed while holding that lease, not the original job's failure; it is not rethrown as a pre-admission typed refusal that could trigger a second post-lease negative reply.
- For persisted intent, close the parent's apply lease before independent failure recovery/reconcile; success also closes it before reconcile. Close only, never LOCK_UN. Inherited child leases, barriers and old uncertain-job blocking remain unchanged. Applied/history scans stay streaming with existence/type/open/read errors checked, not whole-file buffering. Windows absence accepts FILE_NOT_FOUND/PATH_NOT_FOUND; other inspection errors reject (Windows branch not executed here).
- MQTT reply/status and audit publication errors are isolated from task results; worker RAII clears busy even on caught failures. Direct synchronous duplicate can return HTTP error to that request. Direct background Duplicate only audits, never appends original-job failed; its generic catch also does not invent task failure.

## Remaining Limits

**MQTT duplicate submissions may receive no reply and time out.** Query the original task; do not blindly retry, change ID, infer success, or treat an empty query as permission to retry unknown delivery. No requestId/wire change, platform/Windows change, automatic reconciliation/cleanup/unlock, L1/L2 retention change or historical intent/lock deletion.

**Direct HTTP 202 means queued, not execute admission.** If its background worker is rejected before execute's owned accepted stage (Duplicate or NotAdmitted), only request audit is guaranteed, not a new queryable task terminal. This pre-accepted status gap remains. Once accepted/owned, the distinct-job late refusal tested below is durably failed for its own ID and visible in the Direct status file; the other task still completes. Fixing pre-accepted 202 ambiguity requires separately reviewed admission/response semantics, not forging original-task failed.

Local WSL x86_64 only, reusing `/var/tmp/edge-review-r1-20260930-build`. MQTT uses a publisher fake, Direct uses the in-process HTTP handler, and late download uses a gated fake curl (no network request). Scripts are controlled no-op apply/rollback fixtures. Official lifecycle tests use disposable namespaces, not devices/reboot/real systemd/flash durability. No SSH, Broker, DB, network build, push, merge, deployment or realese1.0 reference movement. Unknown historical untracked files were preserved. No new test framework or test target.

## Actual Evidence

All final tests below exited 0. The final source/build was frozen before coordinator's independent run. Routine default `ota_final_state_test` invokes all new regressions; special flags are optional diagnostics only.

| Final execution | Log |
| --- | --- |
| Incremental build: native/service/MQTT/Direct-contract/standalone-checksum targets, -j4 | l4-green-build-verified-20260930.log |
| Default native: duplicate ingress, pre-intent lease, root/integer boundaries, Direct late refusal and legacy lifecycle/final-state | l4-green-native-verified-20260930.log |
| Service from source root: checksum/signature/journal and real execution regressions | l4-green-service-verified-20260930.log |
| CTest 6/6: mqtt_driver_service, ota_service, ota_final_state, ota_checksum, ota_checksum_standalone, system_monitor_direct_maintenance_contract | l4-green-ctest-first-20260930.log |
| Official existing eight lifecycle modes in disposable namespaces, final binary | l4-green-official-verified-20260930.log |

Native cases assert in-flight/historical duplicate suppression with MQTT or Direct owner; invalid duplicate and nested identity; ambiguous root-key rejection; new invalid/different-ID busy negative replies without task failure; unchanged original intent/history; original completed once; audit/accepted-reply exceptions followed by a real next task; actual Direct omitted-default execute; pre-intent failed-callback reentry; actual Direct accepted download held while another job enters apply, then own durable failed with no new intent. Existing synchronous apply/rollback, descendant lease and unchanged-health guards still pass. Tests overlap; do not sum native, CTest and standalone runs as independent cases.

### Red and Diagnostics (Not PASS)

- `l4-red-mqtt-valid-20260930.log`: baseline product red, exit 1. Captured same-ID rejection and composite assertion `in-flight duplicate must not emit negative reply or failed task state/history`. Built test-only changes against L3 product code using `l4-red-build-ready-20260930.log` (exit 0).
- `l4-red-direct-invalid-20260930.log`: additional baseline composite failure, exit 1; old parsing discarded correlation for missing URL. This log alone is NOT proof of an original-ID negative reply. Final malformed-duplicate tests verify the required suppression explicitly.
- `l4-red-build-20260930.log`, `l4-red-build-repaired-20260930.log`: fixture compilation diagnostics (truncated test tail, then include ordering), not product behavior red. Moved only added test bodies to the existing target's companion header and restored the tail; no framework was added.
- `l4-green-build-refined-20260930.log`: failed compilation from a truncated source tail during editing, repaired before final build; NOT green despite filename.
- `l4-green-service-20260930.log`: failed/aborted checksum fixture, intentional regular-file staging yielded ENOTDIR under strict checks. Only fixture paths changed to valid isolated staging in both existing checksum variants; checksum assertions were not relaxed. Final service/CTest pass is authoritative.
- `l4-green-native-first-20260930.log`: FIFO fixture hung in legacy path-existence preflight, manually terminated the identified test process PID 790. NOT PASS. Replaced it with the existing fake-curl pattern; no generic production filesystem/downloader logic was relaxed. Subsequent process inspection found no matching test children. The test-owned FIFO directory was already absent at cleanup inspection.
- Other first/final/intermediate logs remain preserved, but are not evidence for unchanged final source unless listed in the final table. Expected injected/signature/curl diagnostics in successful runs remain visible.

## Hashes and Review Scope

`l4-admission-sha256-20260930.json` contains all 26 raw logs and the 10 source/test files, SHA256 of Windows worktree bytes before staging. Existing Git newline normalization may change checkout bytes. Report/manifest files are not included in their own hashes. `git diff --check` exited 0 (existing LF/CRLF warnings only).

Scoped product/test paths: `include/edge_gateway/{mqtt_driver_service.hpp,ota_service.hpp,ota_request_json.hpp}`, `src/{mqtt_driver_service.cpp,ota_service.cpp}`, `system_monitor_direct_maintenance.cpp`, `tools/{ota_checksum_test.cpp,ota_service_test.cpp,ota_final_state_test.cpp,ota_duplicate_ingress_test.hpp}`. Plus this report, the manifest and named 26 L4 logs. No other files should be staged for this phase. Coordinator review and local-only commit approval are complete; all limits and the Direct pre-admission 202 status gap above remain unchanged.
