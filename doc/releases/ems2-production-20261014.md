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
- [x] Run full applicable Linux native regression tests and deployment fixtures;
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
  complete 153-case rerun subsequently passed in 1067.924 seconds at `04929d2`,
  followed by 6/6 nonroot credential tests (5.351 seconds) and 7/7 CORE-isolation
  tests (46.452 seconds). The unit finished successfully with a zero exit receipt.
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

## Completed Installer Job

- Host: `192.168.22.7`; unit: `gateway-install-full-04929d2.service`.
- Source: `04929d26ccd46124ae8c0571ce3c0d64118dfa73`, a verified uploaded Git archive.
- Remote evidence root: `/var/tmp/gateway-production-install-6d32fd1/`.
- Complete log/exit receipt: `04929d2-full.log` / `04929d2-full.exit`; per-case
  evidence: `04929d2-full-evidence/`. The job finished at 14:05:24 UTC with
  `Result=success`, `ExecMainStatus=0`, `MainPID=0` and `ActiveState=inactive`.
  All 153 cases passed without skips. The complete log, zero exit receipt and
  170 evidence files were returned locally with remote/local SHA256 checks.
- The test source after candidate `e977b80` changes only test-lab/test fixtures;
  candidate production binary, deploy and UI sources are unchanged.
- Next gate: implement the full-reprovision lifecycle described below, verify
  isolation on its actual services, and prove formal install/start/upgrade/rollback.
  Retain the original backup and supplemental authority closure.
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

## Live-Service Isolation Preflight

- `0a96574` adds a read-only board probe, a bounded transient-systemd harness and
  six negative/positive parser-policy tests. All six passed locally and on 22.7.
  No installed service, runtime binary or point configuration is changed by this
  harness. This is a prerequisite, not formal deployment acceptance.
- Two separate systemd services on 22.7 proved that they share one network
  namespace different from the host and the same pinned private SHM marker inode.
  Only loopback and down `sit0` are visible; all IPv4/IPv6 routing tables contain
  only loopback routes. All capability sets are zero, no-new-privileges/seccomp
  are active, sysfs/cgroups are read-only, and host systemd/DBus sockets are absent.
- AF_CAN/AF_PACKET socket creation fails with EAFNOSUPPORT, raw IPv4 with EPERM,
  and `/proc/1/root/run/systemd` access with EACCES. No packet/control command or
  physical-device open is attempted. All temporary services were confirmed stopped.
- The first probe rejected host `/dev/pts/0`, which `PrivateDevices` alone left
  visible. The harness now also hides `/dev/pts` behind an empty private tmpfs.
  A regression explicitly rejects host PTY character devices and block devices.
  This offscreen profile does not qualify physical display/input or serial PTY
  simulators; their selective device access still needs separate review.
- Evidence: `generated/production-20261014/board7-control-isolation.log`,
  `board7-control-isolation-proof.json` and `board7-control-isolation-unit-tests.log`.
  Run the harness using `GATEWAY_ISOLATION_ROOT` pointing to a root-owned mode-0700
  directory containing the pinned probe and a mode-0700 `shm` directory with only
  the small `gateway-ems2-isolation-marker` file. It never applies this isolation
  profile to the board's installed gateway services automatically.

## Recovery Closure And Replacement Gate

- The original 380-entry backup does not contain the external transaction
  directories referenced by `runtime-upgrade-active.json`. Its byte/metadata
  restore pass therefore was not proof of complete legacy-authority recovery.
- Added a supplemental dependency-closure archive covering the current CORE
  activation, both predecessor transactions and both referenced incoming receipts.
  All three state/approval links and the active ready receipt matched their pins.
  Source inventories were unchanged across archiving; all 208 entries and 197
  regular files were checked against the archive, returned locally and verified.
  A private extraction rehearsal verified content, owners and modes for every
  entry. No program executed and no live SHM was restored or removed.
- Supplemental SHA256:
  `98d419d8cf8bb9e93a9f52e78907d769bd19fa9fa29e30d706e58f148ce9acec`.
  Local evidence: `generated/production-20261014/board7-baseline/authority-closure/`.
  Remote: `/opt/gateway-suite-backups/ems2-production-20261014-1bb7f65/authority-closure-20261014/`.
- The unchanged installed historical verifier passed `activated-unit` for both
  `gateway-services.service` and `system-monitor@monitor-service.service`.
  Evidence: `board7-legacy-authority-preflight.log`.
- A second functional rehearsal bound both extracted backups at their original
  absolute paths inside a private mount/network namespace, with read-only mounts.
  The restored historical verifier passed both units there as well. Only the
  read-only Python verifier ran, not gateway business/control binaries. Existing
  host SHM was neither replaced nor rewound; this is not a reboot/restart pass.
  Evidence: `authority-closure/restored-authority-preflight.log`.
- `tar --compare` against the original backup confirmed installed `bin`, `config`
  and `/etc/systemd/system` contents/metadata unchanged after these probes.
- Current lifecycle limitation is explicit: `production-adopt` only admits the
  historical v1 reader scope; ordinary `production-install` preserves the unit
  inventory and disallows introducing KY-EMS/resources. Factory initialization
  refuses existing runtime contents and existing point stores. 22.7 also has no
  provisioned OTA verification public key. These guards must not be disabled or
  given fabricated authority just to install the UI.
- Next implementation work must provide a journaled, verified full-reprovision
  path for the user-authorized replacement, preserving the old authority and
  recovery inventory and maintaining physical-output isolation. Starting the
  existing same-inventory upgrade commands with incompatible inventory is not a
  substitute. The user's full-replacement authorization is already recorded.

## Stopped-Only Reprovision Implementation

- Added a separate whole-image `plan` / `apply` / `recover` helper. It pins the
  helper set, candidate, original immutable files, unit inventory and unchanged
  device identity; keeps the original directory and recovery history; and never
  starts a service or touches SHM. Operational limits are in
  `gateway-reprovision.md`.
- 40/40 tests passed on 22.7 (28.684 seconds), including nine child-process
  SIGKILL boundaries across both replacement and recovery. The test that checks
  the stop condition now rejects any empty `Condition...=` reset, not only
  `ConditionPathExists=`.
- The real-CLI sandbox initially exposed EXDEV across different bind mounts on
  the same filesystem. Preflight now requires matching kernel mount IDs, and the
  CLI test verifies this rejection occurs before STOP/pending publication.
- The unmodified CLI passed inside private chroot/mount/network/PID/IPC
  namespaces: wrong SHA rejection, separate-bind-mount rejection, whole-image
  replacement, repeated restoration and recovery after injected stop failure.
  This fixture uses a fake manager; no business executable is started.
- A separate real-systemd fixture passed template/instance fence inheritance and
  daemon-reload checks using unique runtime-only services that only touch a test
  marker. All fixture units were stopped and removed. Its initial post-success
  check observed an already-collected oneshot; `RemainAfterExit=yes` now retains
  the unit for that assertion. No production service was altered by the fixture.
- Logs: `reprovision-tests.log`, `reprovision-cli.log`, `reprovision-fence.log`
  under `generated/production-20261014/`. These are not power-loss, reboot,
  business activation or sustained-run acceptance.

## Live Reprovision And Stopped Recovery

- Implementation commit: `d71730e46a66d7549759b33d8c5d273626d0d143`.
  The standalone maintenance bundle was built on 22.11 from the identified Git
  archive, with the paired lifecycle's CORE test admission disabled by the existing
  factory staging policy. Archive SHA256:
  `7dbac8cd03acfa36ad1168d80ca3522e3524b38f49ca24dc8d1051beaf6ecdb1`.
  Source archive SHA256:
  `2b8692a99512d1d6e9f16bd2aeb1a9713228672cfaf1db5f42f87af1ad9755cd`.
- The returned bundle's full inventory, file hashes, sizes and root-owned modes
  were verified before upload. Its exact deployed helpers passed all 40 tests,
  CLI sandbox checks and real-systemd fence checks again on 22.7. This is a
  separate stopped-only maintenance bundle, not a rebuilt runtime factory archive.
- Immediately before replacement, both original baseline and supplemental
  authority-closure archive hashes were rechecked against the accepted values.
- Transaction `ems2-d71730e-install-20261014` performed the real service stop,
  whole-image/unit replacement, verification and recovery of the original image.
  Approval SHA256:
  `3b0d1027040b389b3007ee70e5cfb15ef8d40b208fce03af906de765f4af17ce`.
  Installed 18 component hashes matched. Recovery preserved the original directory
  inode, entire after-stop tree and original service file inventory. It did not
  start the old programs or restore runtime/SHM authority.
- Transaction `ems2-d71730e-final-20261014` then installed the candidate again.
  Approval SHA256:
  `86a73434f946331c7ea017d422f3507600e96193efaa5b6b01b4d4ffa29682fc`.
  Current state is **INSTALLED_STOPPED**. All 18 binary hashes, complete image
  inventory, retained original tree, stopped services/MainPID=0, manager-loaded
  stop fences and absence of residual runtime participants passed verification.
- Existing `/dev/shm` names, device/inode identities, sizes, modes and owners were
  unchanged through both transactions. No SHM content was restored, deleted or
  rewound. This metadata check is not a runtime data-consistency acceptance test.
- 24 transaction/plan/proof/log files were returned and SHA256-verified under
  `generated/production-20261014/reprovision-live-evidence/`; its `manifest.json`
  lists every remote evidence path, size and digest. The verified maintenance
  archive is alongside it in `generated/production-20261014/`.
- Recovery authority for the current installation is retained at
  `/opt/gateway-reprovision/transactions/ems2-d71730e-final-20261014/`; original
  programs/configuration are in its `previous-home`. Paired tools and approval
  remain at `/opt/gateway-reprovision-tools-d71730e-20261014/`.
- 22.6's running installation was not replaced. Its three existing acquisition,
  MQTT and monitor services were checked active/running with NRestarts=0 after
  the 22.7 operations.

## IPC Isolation And Installed Runtime Smoke

- The legacy display bridge uses System V SHM key `0x1234` as well as POSIX
  point stores. On 22.7, systemd 245 rejects `PrivateIPC`; an explicit IPC
  namespace is required. The test harness now creates it with a fixed
  `unshare --ipc /bin/sleep` anchor, enters it with `nsenter`, then drops the
  two setup capabilities before any probe or business executable runs.
- Both probe services proved private/shared IPC and network identities,
  initially empty SysV SHM, and zero inheritable/permitted/effective/bounding/
  ambient capability sets. No physical devices or external network routes were
  exposed. Evidence: `board7-ipc-isolation-v2.log` under the generated directory.
- Installed-program smoke v4 ran the approved SystemMonitor, QtDisplayBridge
  and KY-EMS for 45 observations. All exited zero without forced termination.
  A private tmpfs, rather than a disk-backed directory, was required for libc
  `shm_open`; the harness now checks that API and backing-device identity.
- This test proved a release blocker: QtDisplayBridge rewrites packaged
  `ky-ems/VarList.xml`. Original SHA256 was
  `9eb62eebad2bdcc3a63c96fd97618eaf62ff33717bb1e40faadbbccd5b881a21`;
  generated SHA256 was
  `4f6bbbd7d9bf79d43a1dd9c41750e9a2d57fd8c867225fa91a0a8d401a5e54c1`.
  Only a private per-file copy was writable during that experiment. The live
  installation and recovery pins were not weakened or changed.
- A separate native-direct probe ran installed SystemMonitor and KY-EMS without
  the bridge. All 45 observations passed, both programs exited zero, SysV SHM
  remained empty, and VarList stayed byte-identical on the read-only image.
  Evidence: `generated/production-20261014/board7-native-direct-probe-v3/`.
  The source manifest identifies the test snapshot; returned evidence hashes
  were checked. Earlier failed preparation/probe attempts are retained.
- These are bounded offscreen process tests, not canonical service activation,
  simulated acquisition, restart, running upgrade, or sustained-operation passes.

## Explicit Native Qt Startup

- Factory packaging now accepts `--qt-data-source native-point-store` together
  with an explicitly selected `--qt-binary`. It stages the native unit template
  as `ky-ems.service` and records `qtDataSource` in the component manifest.
  The native service uses `gateway-qt-native-run.sh`, with no
  dependency on the legacy bridge. Installed resources remain immutable.
- Default packaging remains `legacy-bridge`; old SysV/XML clients retain their
  previous startup dependency. The bridge executable remains available in the
  package. No mode is guessed from the renderer name, file name, or version.
  Selecting native mode for an incompatible third-party Qt binary is unsupported.
- The legacy launcher stays byte-identical: historical recovery pins its hash.
  An initial shared-launcher change was rejected by legacy activation regression
  tests; the native path is now a separate script instead of relaxing those pins.
- Role resolution pins the selected unit and launcher and models native Qt's
  actual point-store set, including monitor, device and MQTT-driver stores. It
  also accepts the installer's `nativeQt` spelling, matching launcher case and
  whitespace normalization. Existing admission, trust and recovery guards stay
  enabled. This change does not authorize activation of the current stopped image.
- The user confirmed that 22.7 has no connected display and asked to prioritize
  software/deployment verification. Physical display/touch and visual fidelity
  remain separately deferred acceptance gates, not failed software tests.

## Native Startup Candidate Verification

- Product commit: `3e0c45b1d77cd49bd0ca9a542f73b0839d717c3c`. The full Allwinner
  and Qt 5 build ran on 22.11 in a new clean checkout, leaving the existing build
  source untouched. Both specified cross-build scripts passed, including the
  matching Qt access and Lanhu test executables.
- Returned factory package SHA256:
  `b9e7d2c583a99e4cb9f1676c2de12e47e7ae58f48522f5b93109dee177ab489d`.
  All 18 component hashes/sizes/AArch64 ELF headers, root ownership and modes
  passed local validation. The staged service uses the new native launcher;
  the old launcher still has its historical SHA256
  `9c0bf58d8002f76407b1a40bbf26a8957e63dc5bfc2df2fe13f9eb04332d5f52`.
- Evidence and package: `generated/production-20261014/artifacts-3e0c45b/`.
  These build checks alone do not establish replacement or activation on 22.7.
- Matching AArch64 Qt access and Lanhu tests passed on 22.7 in a zero-capability,
  private-network/IPC/device transient service. Eleven synthetic screenshots and
  the isolation proof were returned and hash-verified in
  `generated/production-20261014/board7-native-qt-3e0c45b/`. The unit exited zero
  and was collected. These results are not physical-screen acceptance.
- Targeted Linux results: launcher 5/5, role policy 32/32, source loader 2/2,
  isolation probe 7/7, installed-smoke supervisor 10/10, capacity 14/14.
  Five final installer regression methods passed in 93.910 seconds, including
  legacy/native factory packaging and stopped installation, runtime-mode package
  compatibility, historical Qt observe/recover, skipped-bridge enforcement and
  timeout process-group cleanup.
- The expanded factory test needs a 120-second bound on the ARM board; other
  script tests keep their 30-second bound. The test runner now retires its own
  process group before removing fixtures after timeout. An explicit late-child
  write regression passed. This is a test-harness change, not a runtime change.
- Initial test attempts were not acceptance passes: the build host's Python 3.6
  and filesystem did not meet the production fixture requirements; the board's
  `/var` is root-owned mode 0777, correctly rejected by kiosk path checks; and
  the expanded package fixture exceeded its old timeout. Retained logs identify
  each failure. Final installer backing uses a dedicated root-owned 0755 path
  under `/srv`; no global directory permissions or protection checks were changed.
- The complete updated `production_*test.py` suite passed all 193 tests on 22.7
  in 726.895 seconds. The test-harness correction is committed as
  `8daf8b5629f6bec3a29591700130a9e5ae1136b7`; product binaries remain from
  `3e0c45b`. Full output is retained in
  `generated/production-20261014/board7-native-startup-regressions-v2/production-all.log`.
- The read-only postcheck passed after those regressions: installed and retained
  original trees are unchanged, STOP and manager-loaded fences remain, no
  gateway runtime process or active smoke unit remains, and host SHM metadata
  matches the before-reprovision inventory. Evidence: `postcheck.json` in the
  same directory, SHA256
  `ab49377fb1ed879dbd36d16807ad1032559802f11356427b42426d6599e3b360`.

## Native Candidate Private Install And Process Restart

- The exact `3e0c45b` package passed the real production installer and EMS 2.0
  project installer in a private `/opt/modbus-gateway` bind mount on 22.7.
  All 18 installed hashes and dynamic-library dependencies matched. Manifest
  `qtDataSource`, staged native unit, historical launcher hash and the installed
  `2.0.14-lanhu` project were verified.
- The unchanged native launcher passed preflight, then ran with SystemMonitor
  against read-only program/configuration/resources and private writable data,
  run and tmpfs SHM. Two 20-observation UI runs passed, with a graceful UI stop
  and process restart between them. Monitor and both UI processes exited zero;
  the image stayed byte-identical and no legacy SysV segment appeared.
- Both installer and runtime proved private network, IPC, PID and procfs scope;
  private devices; zero capabilities; seccomp; and blocked CAN, packet and raw
  sockets. No physical interface or host service-manager socket was accessible.
- Observed RSS: monitor 124012-125892 KiB, first UI 65544-118296 KiB, restarted
  UI 66392-118884 KiB. These short startup windows are not a steady-state memory
  bound, leak test, page-switch benchmark or sustained-operation acceptance.
- The two earlier fixture attempts were correctly rejected: the private home
  had mode 0700 instead of the required 0755, then zero-capability installer
  processes could not inspect host procfs. Only the private fixture permissions
  and namespace setup changed. Product guards and host directory modes did not.
- Evidence: `generated/production-20261014/board7-native-package-3e0c45b-v3/`.
  Twelve remote files were returned with SHA256 verification; source pins and
  the final stopped-image postcheck are retained alongside them. Failed v1/v2
  evidence is preserved. This is an isolated installation and process-restart
  pass, not canonical service activation, acquisition or a running upgrade.

## Native Candidate Stopped Deployment

- Both original backup archives were SHA256-verified again locally and on 22.7
  before replacement. The unchanged paired `d71730e` maintenance helpers were
  verified against their previously approved hashes. Device identity bytes and
  mode were preserved; the candidate contained no saved runtime work.
- Transaction `ems2-native-3e0c45b-trial-20261014` installed the exact native
  image and 20 service files, then restored the previous `e977b80` installation.
  Approval SHA256:
  `be6460bac787363b8a2be4e3176fb97976702b38713257871767d80a177d4dcd`.
  Both installed and recovered states passed complete image/unit verification,
  stopped-service/fence checks and unchanged host SHM metadata checks.
- Transaction `ems2-native-3e0c45b-final-20261014` installed the same candidate
  again and remains **INSTALLED_STOPPED**. Its plan differed from the trial only
  in transaction ID and expiry. Approval SHA256:
  `ef86e13a04e8624e5cc361a31d257fd09782c0a3a02b7174d15fedabcbde1126`.
- Final verification passed for all 18 installed component hashes, native Qt
  manifest/unit selection, native launcher dependency preflight, historical
  launcher hash, both retained previous images, loaded STOP fences and absence
  of business processes or active temporary test services. No SHM was restored,
  deleted or rewound. No service was enabled or started by these transactions.
- The immediate previous `e977b80` image is retained in the new transaction's
  `previous-home`. The original pre-maintenance image remains in
  `ems2-d71730e-final-20261014/previous-home`; its inventory is unchanged.
  Keep both transaction directories and the paired helper bundle.
- Evidence: `generated/production-20261014/board7-native-reprovision-3e0c45b/`.
  Final postcheck SHA256:
  `d2c454fba05ff9780396c2872c5fa16acb4586bb3be17d94dfa2a6df2becae02`.
  22.6's installed programs and configuration were not modified in this stage.

Stopped-only recovery to the immediately previous `e977b80` image uses the
retained paired helper, not an arbitrarily newer script:

```sh
python3 -B /opt/gateway-reprovision-tools-d71730e-20261014/gateway-reprovision-tools/deploy/gateway-reprovision.py recover \
  --transaction ems2-native-3e0c45b-final-20261014 \
  --approval-sha256 ef86e13a04e8624e5cc361a31d257fd09782c0a3a02b7174d15fedabcbde1126
```

## Installed Service And Acquisition Fixture

- `tools/board_native_service_acceptance.py` runs the installed Modbus driver,
  SystemMonitor and native Qt launcher in separately supervised systemd transient
  services. A fresh private config overlay selects three Modbus TCP simulators,
  disables device writes and external MQTT, and exposes the monitor endpoint
  only on isolated loopback. Canonical units and their STOP fences are untouched.
- The repository's `GatewayTestLabSim` was cross-built on 22.11 from the clean
  `3e0c45b` source and returned with SHA256
  `ba40bae8be4b9c2b2d11d6b9e953b764f971d359e4cde2fbb43710d212d311c6`.
  This is a test-only simulator, not an added production package component.
  Initial build invocations used the wrong owner and output name instead of
  the CMake target; both refusals are retained, with no global Git trust change.
- Normal collection, changed input (register 4321 -> scaled point 432.1), slave
  2 timeout, and recovery all passed through the actual driver, shared-memory
  router and monitor HTTP point endpoint. During the injected timeout, index
  920002 had quality 0 while indices 910002 and 930002 stayed fresh at quality 1.
- Deliberate SIGKILL of the driver and then UI caused systemd to restart each:
  both obtained a new PID and NRestarts=1. The subsequent acquisition checks
  passed. A 181.15-second observation returned 36 good-quality, nonstale samples
  with advancing device timestamps. No additional service restart occurred.
- All business/observer entrypoints proved private network/IPC/SHM, private
  devices, zero capabilities, seccomp, blocked raw/CAN sockets and inaccessible
  host control sockets/root. Installed binaries/resources remained read-only.
  Cleanup stopped all fixture units and unmounted only the owned private tmpfs.
  The final installed-image/original-backup/STOP/fence/host-SHM postcheck passed.
- Tests: three fixture assertion tests passed locally and on 22.7; seven
  isolation-policy tests passed on 22.7. Evidence and per-file return hashes:
  `generated/production-20261014/board7-native-services-v1/`.
- This covers simulated acquisition and transient-service recovery, not
  canonical service admission, production configuration, physical device I/O,
  visual binding of fixture indices to EMS screens, MQTT/compute/cluster
  integration, long-duration stability, reboot or running upgrade/rollback.

## First Runtime Bootstrap Gap

- The existing production adoption path requires a verified historical active
  baseline. It cannot legitimately adopt this whole-image reprovision's empty
  runtime state. Stopped preparation is now implemented as described below;
  an explicit journaled first-runtime activation transition is still needed;
  do not manufacture historical activation records or remove STOP manually.
- That transition must bind the installed reprovision approval, reviewed role
  profile, exact config/unit/image inventories and trust policy; create fresh
  native SHM without touching retained host segments; and retain stopped-only
  recovery through failures before a proved healthy production promotion.
- An approved OTA verification public key and allowed HTTPS artifact origins
  have been requested from the user. No temporary test key has been installed
  as production trust. Missing trust is separate from the bootstrap code gap.

## Monitor Shared-Memory Admission

- Bootstrap investigation confirmed that both SystemMonitor and native Qt use
  `GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME`. The native default is not an
  immutable binding: omitting this variable can select the old retained store.
  The configured-role authority previously described the expected store but
  did not prove the effective service environment before process creation.
- V2 production admission now checks SystemMonitor and native Qt against the
  protected `monitorShmName`. Both must have an explicit nonempty binding,
  including when selecting the native default. It rejects missing/stale names,
  unreviewed environment sources/removal/PAM and ambiguous assignments.
  V1 historical admission and the byte-pinned legacy Qt launcher are unchanged.
- Native Qt may load only its pinned `config/runtime/qt-display.env`. Literal
  assignments, comments, plain or quoted values and LF/CRLF are supported;
  duplicate keys, escapes, continuations, malformed quoting and files over
  64 KiB refuse admission. Environment-file values override unit environment
  values, matching systemd. The guard does not execute or source the file.
- Checks run during effective-unit verification and before a V2 candidate
  transaction is created. Candidate checks read the approved staged config,
  while checking the manager's live environment-file path. No default global
  manager environment is treated as explicit approval.
- This is a prerequisite fix, not a bootstrap implementation or activation.
  The installed `3e0c45b` package and paired recovery helpers are not patched
  in place. A new candidate package is required before deploying this change.
  No C++ binary or SHM layout changed in this step.
- On the intermediate source snapshot, the full production Python suite on
  22.7 passed 211 tests in 722.075 seconds; three opt-in systemd tests were
  skipped by discovery and passed separately. After tightening explicit default
  binding and hashing the actual environment bytes being parsed, the final
  snapshot passed 17 binding tests, six lifecycle admission/promotion/recovery
  tests, one native-boot test and all three real-systemd tests (27 total).
  This is not a claim that the full suite was rerun after those final two changes.
- Real-systemd tests run only uniquely named, sandboxed `/bin/sleep` units.
  Their executed process environments proved explicit binding, file precedence
  and missing-binding rejection. The first probe sampled MainPID before exec
  and failed its environment assertion; the corrected probe waits for the actual
  sleep executable. Failed v1 evidence is retained, not counted as a pass.
- Evidence: `generated/production-20261014/board7-monitor-binding-v2/` for the
  broad suite and `board7-monitor-binding-v3/` for final-source tests. Source
  snapshots, per-file hashes, test logs and before/after postchecks are retained.
  Final guard SHA256:
  `5185bcef5ba934109e11fe5e9d85f2d9dcf4a97396082542f1a2c68914e90e60`.
- Final before/after postcheck SHA256 is unchanged:
  `d2c454fba05ff9780396c2872c5fa16acb4586bb3be17d94dfa2a6df2becae02`.
  All 18 installed components, both retained images, STOP/fences and host SHM
  metadata remain unchanged; no gateway business process or probe unit remains.
  No AArch64 binary rebuild, package replacement or physical output occurred.

## First Runtime Stopped Preparation

- Added `pointctl create-empty --shm <new-name>` using native exclusive creation
  before any config/default-store construction. Arguments and names are strict;
  existing initialized, zero, partial, legacy, linked or directory objects are
  refused without changing their contents or attaching them.
- Added `runtime-bootstrap.py` with plan, prepare and stopped-only recovery.
  It binds the stopped reprovision origin, installed/recovery images and units,
  identity/package/network/TLS, installed signature trust, reviewed simulator
  roles, helper/creator hashes, boot/expiry and retained SHM metadata. It only
  stages config externally and creates fresh empty native v11 objects.
- New approval, pointer and ready receipts use atomic no-replace publication;
  progress checkpoints use atomic replacement. Recovery handles interruption
  after durable approval but before initial progress only when no pointer or
  native object exists. It retains partial/completed new stores and evidence,
  never rewinds old stores and only removes matching owned bootstrap pointers.
- `PREPARED_STOPPED` and `activation: not-authorized` are deliberate limits.
  No production baseline is manufactured, no live config/unit changed and no
  STOP fence removed. See `deploy/RUNTIME-BOOTSTRAP.md` for input/CLI/recovery
  details. Full first-runtime activation and activation failure recovery remain
  unimplemented; preparation is not end-to-end bootstrap completion.
- The new pointctl was cross-built on 22.11 in an isolated checkout based on
  `9318418` with exactly one reviewed dirty source, `tools/pointctl.cpp`, SHA256
  `1ccdbec3f88d99fa40d1853fb78bcd4fd962bb90bb75e6d5e4755c3d8e8b3fe6`.
  Returned AArch64 artifact SHA256:
  `6cc35fc834fc6be46bc7bb2ba22fedb97fdb4909d1865959b2ea83e35d489d3d`.
  It is a test artifact, not an installed component or a new factory package.
- Final snapshot validation on 22.7: 25 preparation fixtures passed (one native
  case skipped by ordinary discovery), 33 role-policy and 40 reprovision tests
  passed. In a private mount/network/IPC namespace, all six native CLI tests
  and the opt-in preparation/recovery test passed: 105 successful executions
  overall. The integration uses the real creator and kernel mount/capacity
  checks, but fixture service-manager, lock and mapping-discovery substitutions;
  it is not a canonical CLI/service activation test.
- Native integration first exposed strict-file rejection of `/proc/self`, then
  an overmounted-path false rejection. Capacity now reads numeric-PID procfs and
  selects the visible directory's mount ID. The regression also rejects a
  visible non-tmpfs layer even if a hidden tmpfs has the same path. Failed v2/v3
  evidence is retained. Interruption tests for this preparation helper inject
  exceptions, not SIGKILL/power failure; reprovision's existing SIGKILL tests
  must not be counted as preparation crash qualification.
- Separate known reboot gap: `ProductionRuntime.prepare_boot` still reads
  `/proc/self/mountinfo` through strict `regular()` and uses a path-count mount
  check. It needs a dedicated fix and real-kernel regression before reboot
  qualification. This preparation-only change does not fix that shared path.
- Evidence: `generated/production-20261014/board7-bootstrap-fixtures-v4/` and
  `bootstrap-pointctl-artifacts/`. Final before/after stopped-image proofs both
  retain SHA256
  `d2c454fba05ff9780396c2872c5fa16acb4586bb3be17d94dfa2a6df2becae02`.
  All 18 installed components, previous/original images, loaded STOP/fences and
  host SHM metadata are unchanged; no business process remains. The private
  tmpfs was unmounted. 22.6 was untouched; no physical output occurred.
- Approved production OTA public key/origins remain outstanding. No fixture
  trust was installed. New package construction/stopped deployment is required
  before this creator or the earlier monitor-binding admission fix can be used
  against the real installed image. Screen/touch and visual acceptance remain
  deferred because 22.7 has no connected display.

Status: in progress, not production-ready. 22.7 now contains the `3e0c45b`
native-startup candidate under its own stopped reprovision transaction. All
canonical gateway/EMS business services remain stopped; the installed UI is
not running outside bounded isolated tests.
Controlled canonical activation with fresh SHM, runtime
upgrade/rollback, reboot/power-loss, sustained operation, physical display/touch,
multi-vehicle/HIL and >=95% visual acceptance remain open. Do not remove STOP or
legacy guards to force startup. Incremental Qt validation binaries are not the
final release artifact.
