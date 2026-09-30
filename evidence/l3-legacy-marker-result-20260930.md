# L3 Legacy Marker Admission (2026-09-30)

Base: `c80479ea7c43f5248474fca5c90f48843876f833`; branch `fix/edge-review-r1-20260930`.

Only L3: under the existing `ota_state` lock, before persisting intent, require strict absence of `ota-in-progress`. Linux uses lstat and only ENOENT means absent; Windows uses GetFileAttributes and only ERROR_FILE_NOT_FOUND means absent. Present empty/non-regular/dangling/loop-link markers reject without interpreting their content. Inspection errors throw, not authorize admission. No cleanup, unlock, historical intent/lock removal or rollback of the old job was added. L1/L2/L4 were not changed in this phase.

## Actual Red/Green

Local WSL x86_64 build reused `/var/tmp/edge-review-r1-20260930-build`; both test binaries ran from the source root. Commands used `bash -o pipefail` with tee to preserve raw output and exit status.

- Red build: exit 0. Test-only change against original product code.
- Red `ota_final_state_test`: exit 1, `legacy marker rejection must not persist a new intent`.
- Green build of `ota_final_state_test ota_service_test -j4`: exit 0.
- Green `ota_final_state_test`: exit 0. Populated/empty/directory/dangling-link/loop-link marker cases all reject actual execute without new intent; marker retained, apply/rollback uncalled. Only the fixture removes the marker to simulate existing recovery completing; the SAME new job then actually executes a local no-op apply script and completes with its own intent. Existing lifecycle/final-state regressions also pass.
- Green `ota_service_test`: exit 0. Existing checksum/signature/journal regressions pass. Expected negative signature and curl diagnostic output is retained, not an unreported test failure.
- `git diff --check`: exit 0 (existing LF/CRLF normalization warnings only).

These are local fake-script tests, not official package installation, actual reboot/recovery, device/Broker/DB/systemd/flash-durability or AArch64 qualification. No network build, SSH, push, merge, deployment or realese1.0 reference movement. Unknown historical untracked files were preserved. Coordinator authorized the independent L3 phase commit after reviewing the diff and reported tests.

## Evidence SHA256

Hashes are raw Windows worktree bytes before staging; Git newline normalization may change checkout bytes.

| Log | SHA256 |
| --- | --- |
| l3-red-build-20260930.log | bb910c065209b5cde519294473dd39ccb047980b73802d0d252f6b72aa6b6467 |
| l3-red-native-20260930.log | 64feff8c32b1f7dcb0de0283b4a27f13250e55a573e459e99f154ef99384de79 |
| l3-green-build-20260930.log | 400e12103d78a3c7bda36d5281b011cae9e5ddd8bab09d701e90f5fce9f7c7ac |
| l3-green-native-20260930.log | cad94e58a575126f7dace0210ac858d41d057beec8ffe0eec44518c964e91a2c |
| l3-green-service-20260930.log | ad4be066eb532a57948fca0a809692ce10a882b1e83928a7e56b25f3d9d67b01 |
