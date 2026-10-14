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
- [ ] Run UI generator, package, authentication, navigation, value-quality,
  multi-vehicle and legacy compatibility tests on the integrated source.
- [ ] Build production binaries and Qt runtime on 192.168.22.11 from an identified
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

## Device Recovery Evidence

- 22.7 baseline: `/opt/gateway-suite-backups/ems2-production-20261014-1bb7f65/baseline.tar.gz`,
  SHA256 `deaea755524e84a5193236cd9256499c41f5278467aa682d8cad7f5461c9daf0`.
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
