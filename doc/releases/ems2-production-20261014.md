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
  This is a built candidate, not a replaced or activated installation on 22.7.
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

Status: in progress, not production-ready. 22.7 has the new candidate installed
but all gateway/EMS business services remain stopped; its new UI is not running.
Controlled activation with fresh private SHM, actual startup/restart, runtime
upgrade/rollback, reboot/power-loss, sustained operation, physical display/touch,
multi-vehicle/HIL and >=95% visual acceptance remain open. Do not remove STOP or
legacy guards to force startup. Incremental Qt validation binaries are not the
final release artifact.
