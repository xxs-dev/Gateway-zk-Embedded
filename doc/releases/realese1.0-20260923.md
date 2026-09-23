# realese1.0 Local Stabilization - 2026-09-23

Status: LOCAL_CANDIDATE_VALIDATED; target acceptance and deployment approval pending.
The spelling `realese1.0` is intentional. No push, SSH, device control or deployment
was performed. Original dirty worktrees and historical sealed packages are unchanged.

## Source Selection

Base: `9077e97034e11739768ad033cdae4007eb6f74e6`, not historical master.
This includes the accepted MQTT session/full isolation, persistent control dedup,
history retention, storage admission, forwarding, SHM migration and OTA chain.
Fourteen accepted commits beyond the original worktree HEAD `febaa888` are retained.

| Historical master-only change | Reconciliation |
| --- | --- |
| SCADA dual topology | `15ef5f113a6d96507b359dc2c0701e360ffcadec` is an ancestor of the selected base |
| EMS logic editor merge | Trees at `9688221e9722f03ad586bc4a205effb3582571fd` and merge `9d22dee5652dfc5d75b1f320f8f3becaeadaf3c1` are identical; accepted source retained |
| GCC6 optional API test fix | Explicit bool conversion is already present in the selected base |
| Kiosk boot deadlock fix | Missing `local-kiosk@*.service` nonblocking start restored in `1b4153b`; regression reproduced before fix |
| README | Preserved from master `25718d09895cafa515022bf2ce7985d4ce4a8881`, with this release note linked |
| Old compiled programs/packages | Not transplanted; stale tracked output removed from the release tree |
| OTA manifest guard | Full accepted implementation retained, not replaced with the older master script |

Commit `5929b177215b9c917890801573ef12145403b774` imports the seven exact sealed06
generic initialization scripts, the generic test harness and fixed-input builder.
The scripts preserve read-only uncommissioned Qt, protected runtime directories,
mode-aware smoke checks and the existing 85-percent disk threshold.

## Original Dirty Delta

The original `D:/workspace/Embedded/Gateway-zk` had 21 tracked modified files.
Untracked entries include thousands of build, field and evidence files; they are
not a reviewed release source inventory. Nothing there was reset or overwritten.

| Category | Decision |
| --- | --- |
| Storage/CMake additions | Use the newer accepted committed chain, not older dirty duplicates |
| Factory/runtime MQTT changes | Retain accepted defaults and sanitized generic-package construction; do not import field runtime credentials |
| Installer/idempotency/upgrade helpers | Use accepted isolation fixes and exact sealed06 generic scripts; defer unknown dirty helper deltas |
| Cellular/bootstrap/network failover | Defer unverified field-specific 4G changes |
| EMS local controls, generator/release helpers | Defer unverified dirty control and generation changes |
| COMM202600103/104 point tables and field changes | No new field changes imported; existing curated project sources are not generic defaults |
| Documentation, captures, field tools and binaries | Preserve in the original worktree; do not bulk-stage them |

Runtime JSON remains as historical examples because parser tests reference it;
three inherited MQTT password values are cleared in this branch. This is not a
complete secret-history purge. Old Git commits still contain the old values;
the responsible owner must assess rotation. No history rewrite was performed.
Fixed maintenance authentication and example protocol credentials are separate
known limitations, not claimed to have been eliminated by clearing these fields.

Only exact generated paths were removed from the Git index: `build-aarch64/`,
the root factory tarball, redundant `config/samples/messages.zip`, four generated
Windows simulator executables/libraries and their obsolete checksum list.
Physical local copies remain; PowerShell launcher sources remain tracked.
No accepted external program archive, manifest or sealed candidate was removed.
New packaging uses a pinned external AArch64 archive, never these stale files.

## Paired Package

Directory: `../realese1-artifacts-20260923/candidate-01` relative to this worktree.
Assembly source is clean `5929b177215b9c917890801573ef12145403b774`.
Binary source remains `5d40272e61e708e4694b4bdc6f15e2886efcd73e`.
All 19 AArch64 program hashes are unchanged. No new target compilation occurred.
Later README, credential-example and index cleanup does not retroactively change
the assembly source or claim new binaries. AGC is reused byte-for-byte, retaining
its original assembly provenance.

| Artifact | SHA256 |
| --- | --- |
| gateway-factory-defaults.tar.gz | `070973be2b9cd12e0cf72a259114a59316fc02c88056b99c0a2fe2955a3ea701` |
| gateway-agc-avc-runtime.tar.gz | `d3aeda962f481ff32167cc6df72fc2603127cc37f57cb3056d8285be964ef4b4` |
| component-provenance.json | `b51f2437159949c0631b15a87fc19f66bc67a5b4da274a372eb22a8bbf6db8d4` |

Independent comparison verified all 71 factory and 5 overlay files against
provenance, and all 47 external deploy files against their embedded counterparts.
Compared with sealed06 only `ota-apply.sh` and `gateway-services.sh` changed.
In particular, install-factory and production-init remain the sealed06 versions.
Windows must embed this entire pair and provenance, not just an external script.
Historical candidate06/candidate07 packages must not be modified.

## Verification

Evidence root: `../realese1-artifacts-20260923/evidence`.

| Check | Result / evidence |
| --- | --- |
| WSL native Debug build, GNU 15.2, x86_64, Qt5 Widgets | Completed; original session83405 exit0, subsequent incremental build log retained |
| Native CTest, private mount/IPC/network namespaces | 88 passed, 1 skipped, 0 failed, 254.34 seconds; `ctest.log` |
| Qualified SHM migration rerun | 1 passed with private SHM and explicit persistent temporary backup directory; `migration-qualified.log` |
| Kiosk/Qt startup regression | RED before fix, GREEN after fix; `boot-red.log`, `boot-green.log`; now registered in CTest |
| Installer isolation | 22/22 passed; `install/result.txt` |
| OTA manifest guard and partial rollback | 9/9 passed; `ota/result.txt`, isolation and candidate-integrity receipts |
| Exact new generic candidate | 41/41 passed; `generic/result.txt`; candidate files unchanged after tests |
| Binary/configuration scan | 19 pinned AArch64 binaries; no site identifiers or nonempty credential fields in generated config |
| Pairing validation | PASS; `pairing-check.json` |
| Additional native checks | Qt value mapping and watchdog lifecycle passed; config loader passed after credential clearing |

The initial CTest skip was a deliberate fixture gate, not a pass. Its separately
qualified rerun passed. Installer tests use mocked services and do not execute
AArch64 binaries. Native Qt compilation is not a target display acceptance test.

## Compatibility And Remaining Gates

No MQTT topic, API schema or cross-end contract version changes. Existing protocol
and EMS behavior is retained; new initialization packages are generic/uncommissioned,
not ready-to-control site configurations. Do not deploy raw historical runtime or
factory examples as a substitute for commissioned configuration.

- A fresh Allwinner AArch64 build at the final source revision is NOT RUN.
  Its approved toolchain remains on 192.168.22.11; this round authorized no SSH.
- No new target Qt/X11 screen, kiosk boot or physical protocol acceptance.
- No new power-loss recovery, long soak, target 2/4-GiB RAM budget or flash-wear test.
- Offline MQTT storage defaults can reserve up to 1 GiB; disk capacity is not
  proof of RAM suitability. Evaluate configured queues, retention and concurrency
  on the actual hardware, rather than infer capacity from native unit tests.
- Fixed maintenance credentials and checksum-only OTA remain known security
  limitations. Checksums do not establish publisher authenticity. Deployment
  requires a controlled network, trusted artifacts and restricted maintenance/OTA
  access. Public exposure is not accepted by this candidate.
- Windows embedded-pair and installer acceptance belongs to the Windows role;
  full-system and production acceptance belongs to coordination/implementation.

Remotes: origin `https://github.com/xxs-dev/Gateway-zk-Embedded.git` and
gateway-suite-gitlab `git@192.168.22.104:gateway-suite/gateway-zk.git`.
No push performed; SHA and explicit authorization are required before publishing.
