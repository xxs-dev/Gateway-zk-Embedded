# realese1.0 Production Readiness and EMS 2.0 Integration

## Objective

Bring the current `realese1.0` edge software to production usability, including
the complete EMS 2.0 Lanhu UI and dynamic parallel topology. Validate on the
authorized 192.168.22.7 and 192.168.22.6 test devices. Passing only a UI preview,
compilation, or a subset of tests does not satisfy this objective.

## Source Baseline

- `origin/realese1.0`: `845499d5a3264096ef1b046ff435c00f61d3bf14`, fetched before work.
- Integration checkout: `edge/production-ems2-20261014`, branch `realese1.0`.
- The old directory named `realese1.0` is on `fix/edge-review-r1-20260930` and
  contains unrelated uncommitted work. It is not changed by this effort.
- UI commits: `baac9ed`, `e753be3`, `cfa3da6`, `8b8b137`.
- UI package: `2.0.14-lanhu`, 56 routes. Missing vendor interfaces remain
  explicitly unavailable, not simulated or optimistically reported as successful.

## Acceptance Gates

- [x] Fetch latest release source and preserve unrelated local changes.
- [x] Integrate all four EMS 2.0 commits without dropping release changes.
- [ ] Review production startup/configuration, data collection, control safety,
  IPC/SHM, persistence, upgrade/rollback and display paths; resolve verified defects.
- [ ] Run full applicable Linux native regression tests and deployment fixtures;
  investigate failures rather than excluding them to obtain a green result.
- [x] Run UI generator, package, authentication, navigation, value-quality,
  multi-vehicle and legacy compatibility tests on the integrated source.
- [x] Build production binaries and Qt runtime on 192.168.22.11 from an identified
  isolated snapshot. Verify architecture, dynamic dependencies, provenance and hashes.
- [ ] Inspect and back up both devices; prove rollback artifacts before replacement.
- [ ] Validate the candidate on 22.7: startup/restart, real configuration loading,
  read-only acquisition, freshness/failure behavior, screen rendering/navigation,
  authentication, bounded resource use and no unintended device writes.
- [ ] Validate multi-vehicle behavior and appropriate compatibility/failure cases
  using isolated fixtures on 22.6/22.7. Do not treat simulated data as field telemetry.
- [ ] Validate install/upgrade/rollback with exact production-format artifacts and
  document recovery commands; then perform sustained operation checks on both devices.
- [ ] Verify all complete UI designs against Lanhu and perform physical-display,
  touch/scaling and performance acceptance; >=95% fidelity remains unproven.
- [ ] Produce a fixed-commit release candidate, SHA256 inventory, test evidence,
  remaining interface/support boundaries and an accurate edge status record.

## Initial Device Observations

- 22.7: Allwinner/aarch64 Linux 5.10.198, 14 GiB root filesystem, 38% used;
  gateway launcher active/exited, no driver process found by the initial inventory.
- 22.6: Allwinner/aarch64 Linux 5.10.198, 28 GiB root filesystem, 91% used;
  Modbus read-only, MQTT and system-monitor services running; old test units failed.
- Both test devices share the same SSH host key. This is a cloned-image security
  concern to resolve in the production provisioning process. Do not automatically
  rotate keys or credentials during testing and break the operator's access.
- 22.11: x86_64 build host, about 100 GiB free. Shared source is dirty on an older
  feature branch; it must not be reset, cleaned, overwritten or used as this candidate.
- Test credentials are supplied interactively and must not be committed or shipped.

## Evidence

New local evidence is stored under `generated/production-20261014/`. Every remote
build must record its source SHA256, commit and live process/unit identity. Build
artifacts return to this checkout with matching remote/local SHA256. Remote test
fixtures use isolated directories and SHM names; hardware actuation is not inferred
from permission to use the boards as test devices.

## Progress (2026-10-14)

- Integration: `1bb7f65`; Qt 5 QStringList compatibility and ARM UI test build:
  `1164aa9`; GCC 7 filesystem test compatibility: `4e47289`; standalone OTA
  dynamic-loader dependency: `2b68b61`.
- `dc1a388` rechecks the operator session when submitting single/group commands,
  including confirmations left open past expiry. Previously accepted pulse cleanup
  retains its existing transport path. Submission is no longer described as device
  execution. The new expiry regression passed on 22.7 with the real AArch64 Qt 5
  renderer, together with login/navigation/topology tests.
- `9128f6e` includes the required SHM migration executable in default production
  builds. Recovery tests preserve rejection of occupied baseline IDs and use new
  IDs for retries; old records/receipts are checked unchanged. `ddcbd27` completes
  the private boot-health fixture dependencies.
- `63c16e7` joins the serial simulator before asserting response counters and
  records executable modes for the Linux test-lab launch scripts.
- `7832081` restores a deleted logic file only from a regular, single-link,
  timestamped backup inside its authorized logic tree. Missing backups, foreign
  roots, traversal, symlink parents and symlink backups are regression cases.
  This fix is committed but full verification is still pending.
- The first production fixture run on 22.7 completed 191 tests with 4 errors:
  an outdated AST dependency and three obsolete baseline-ID reuse expectations.
  A complete rerun from `ddcbd27` passed all 191 tests on 22.7 (688.887 seconds).
  No safety assertion was removed.
- The first native run executed 108 tests with 16 failures and 2 skips. Repeating
  with private SHM removed several stale-segment failures. A fully isolated `/tmp`,
  SHM and network run is required; root-only migration/handoff tests are included
  with an explicit persistent backup directory. Remaining failures are not waived.
- A clean Git bundle at `63c16e7` was verified and cloned on 22.11 for the candidate
  build. Later fixes must be synchronized and rebuilt before candidate acceptance.
- Native x86-64 builds with older headers now use the same kernel pidfd syscall
  ABI fallback as AArch64; unsupported kernels still fail closed. The boot-order
  fixture explicitly mocks kiosk projection and checks refusal on projection failure.
  Test-lab maintenance admits only loopback, and historical OTA query samples now
  carry the required intent hash and reconciliation record. Verification pending.

## Installer And Regression Follow-Up

- `70b751b` aborts gateway apply on service enumeration/stop failures. It preserves
  the manual-stop fence and does not disable/reset units or start replacements.
- The full 191 production Python fixtures passed on 22.7 at `ddcbd27`.
- Native runs at `70b751b` and `2817afb` each passed 107/108 tests. The earlier
  OTA assertions used outdated delivery state, JSON field order and incomplete
  device identity. After those corrections the OTA final-state test passed.
- The remaining protocol E2E failure was reproduced with diagnostics: a repeated
  fixed CAN command ID replayed a prior successful receipt from the default
  installed control ledger. `da86239` assigns one ledger inside each test-lab
  root to all its device routes; `608646d` makes fixture JSON reads UTF-8 under
  older Python/C locales. At `608646d`, three consecutive protocol runs passed
  (90.30 seconds), followed by all 108 native tests (327.82 seconds). Complete
  logs and zero exit receipts were returned to `generated/production-20261014/`.
- The first expanded installer run was not accepted: 153 tests had 36 failures
  and 71 errors. Missing pinned historical assets, tmpfs-backed durable state and
  group-writable source extraction prevented many tests reaching their assertions.
  `6d32fd1` supports explicit evidence roots and private persistent backing for
  the test home/state. The original historical SHA256 assertions remain intact.
- Four targeted installer tests subsequently passed on 22.7: isolated factory
  install, real ARM migration/stopped recovery, runtime modes and historical
  20-component preflight. The full installer rerun at `df7102b` completed
  150/153 successfully in 1207.032 seconds. Two tests correctly refused because
  a concurrently running isolated Qt process was still visible through host
  `/proc`; the third lacked the explicit ARM `scada_runtime_test` binary.
  `04929d2` gives the installer fixture its own PID namespace/procfs and supports
  an explicit SCADA test binary. The binary was cross-built on 22.11 and returned
  with SHA256 `9f238fbc148452357be1e765b08748b1e9f080d425e66626b0aa4d4f83668d21`.
  All three formerly failing cases passed in the targeted rerun (22.895 seconds),
  followed by the six nonroot credential and seven core-isolation cases. The
  complete 153-case rerun is still required before accepting the installer gate.
  Credential tests must run nonroot; the first root invocation was rejected by
  their own guard and is not counted as successful verification.
- Nonroot credential tests passed 6/6 at `9cd83af` after supplying the fixture's
  required generic widget resource and verifying it remains unchanged. Factory
  core-isolation tests passed 7/7 at `df7102b`. The Lanhu 56-page and legacy
  XMind 34-page generators were rerun locally at `9cd83af` and both passed.
- `df7102b` fixes a real packaging defect: nonroot builds retained the build
  user's UID/GID, causing the root-owned kiosk installation policy to reject
  them. Factory archives now normalize owner/group to root and remove group/world
  write permissions, with metadata assertions and an untrusted-owner fixture.
  The earlier `70b751b` package is superseded and must not be deployed.
- Candidate `e977b8060c58923278797184e87a4679720fb9d3` was rebuilt on 22.11 from a
  clean identified Git checkout and returned to
  `generated/production-20261014/artifacts-e977b80/`. Its package SHA256 is
  `d7d4e23dec80ae7d0bbde9659b6958a75cc7fcec65cd16626f3bd405a931c358`.
  All 18 component hashes, sizes and AArch64 ELF identities, resource hashes,
  archive paths, root ownership and non-writable group/world modes were checked.
  The selected KY-EMS is `2.0.14-lanhu`, SHA256
  `bff01f6c61a681659112b71e296e1cc0f73fd7a79b2af97e5d2905db6df71929`.
- No OTA verification key is provisioned by this candidate. OTA remains fail-closed
  until the operator provisions the production key; this is not a provisioning pass.
- Exact-package installation with the generated EMS 2.0 project passed
  in a private persistent `/opt` on 22.7. The service has a private network and
  devices, read-only sysfs, no host `/run` or `/tmp`, restricted socket families
  and no SYS_ADMIN/NET_ADMIN/NET_RAW/SYS_RAWIO capability. This is isolated test
  installation, not replacement of the board's installed gateway. All 18
  installed binary hashes and dynamic dependencies passed, the `2.0.14-lanhu`
  project activated successfully, and the installed Qt renderer stayed running
  for the bounded offscreen startup test. The generic-package-only
  `--local-qt-display` flag is not used for this EMS-specific package: native
  display is enabled by the validated SCADA project installation.
- The matching AArch64 access and Lanhu test executables both passed on 22.7
  against the installed project. Eleven offscreen fixture screenshots cover
  0/1/2/3/5 members, offline combinations, second-page topology, stale state and
  the logged-out gate; the five-member second-page image was visually inspected.
  These are simulated values, not field measurements or 95% visual acceptance.

## Current Verification Job

- Host: `192.168.22.7`; unit: `gateway-install-full-04929d2.service`.
- Source: `04929d26ccd46124ae8c0571ce3c0d64118dfa73`, a verified uploaded Git archive.
- Remote evidence root: `/var/tmp/gateway-production-install-6d32fd1/`.
- Complete log/exit receipt: `04929d2-full.log` / `04929d2-full.exit`; per-case
  evidence: `04929d2-full-evidence/`. Do not replace source while this job runs.
- The test source after candidate `e977b80` changes only test-lab/test fixtures;
  candidate production binary, deploy and UI sources are unchanged.
- Next gate: retrieve the completed result, investigate any failures, then prove
  live-service control isolation and the formal install/start/upgrade/rollback
  sequence before replacing installed 22.7 files. Retain the original backup.
- A user question about an attached touchscreen/resolution remains unanswered.
  Offscreen tests do not establish physical screen/touch acceptance.

## Device Recovery Evidence

- 22.7 baseline: `/opt/gateway-suite-backups/ems2-production-20261014-1bb7f65/baseline.tar.gz`,
  SHA256 `deaea755524e84a5193236cd9256499c41f5278467aa682d8cad7f5461c9daf0`.
  A copy is now at `generated/production-20261014/board7-baseline/baseline.tar.gz`;
  its hash matched and all 380 archive members were inspected/read successfully.
  A private restore rehearsal on 22.7 verified all 380 restored entries,
  including the byte hashes of 235 regular files, ownership, modes and symlink
  targets. No restored program was executed and no live gateway path changed.
- 22.6 code/config baseline: same backup directory, `code-config-baseline.tar.gz`,
  SHA256 `1e61f769ea18d1ca592655ecfb4e1fdc9b4118bbe80120579c7dc8343f7fde2c`.
  Its initial live-data archive was inconsistent and is not accepted for rollback.
- User approved archiving and deleting only the old 22.6 recovery directory
  `/opt/gw-recovery-new-device-20260917T133737Z-6976ac91`. Archive completed locally:
  `generated/production-20261014/board6-retired-recovery/`, 5,766 entries,
  912,017,314 compressed bytes, SHA256
  `903e0d89a6d84b11206f314fffabdaf919627cbe412cc53c4c4b29e7c24401c3`.
  Stream hashes matched, all archive members were read, source inventories matched,
  and active process/mount/symlink references were checked before deletion.
  Root usage fell from 91% to 70% (8.4 GiB available); all three original services
  remain active with zero restarts. Current programs/configuration were not replaced.
- User explicitly permits complete replacement of programs/configuration on 22.7
  for isolated deployment and rollback testing. Physical actuation is not authorized.

Status: in progress, not production-ready. No installed device program has yet
been replaced. Incremental Qt validation binaries are not the final release artifact.
