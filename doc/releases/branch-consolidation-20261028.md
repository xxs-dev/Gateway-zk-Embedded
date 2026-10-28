# Release Branch Consolidation (2026-10-28)

## Scope

User-approved scope is both `origin` (GitHub), `gateway-suite-gitlab` (GitLab),
and local branches. Retain exactly `master`, `realese1.0`, and `feat/maint-v2`.
Keep the existing release spelling. Do not merge maintenance v2.

Starting release: `dfca2eca4b6c8d0c3191b0ff559cd6d7b2576d09`.
Excluded maintenance tip: `ab1a0dbc6fc3497e642be668704a1cfb329b6b03`.
All 17 commits exclusive to that maintenance tip versus the starting release
remain outside the integrated release history. Every original branch tip
scheduled for deletion is an ancestor of the integrated release.

Original inventory: 51 local branches, 9 GitHub branches, 15 GitLab branches,
and 45 worktrees. Cleanup removes 49 local, 7 GitHub, and 12 GitLab branch refs.
The existing maintenance tip is also retained locally and on GitHub. Master
tips are preserved, not rewritten to make their old local/remote values equal.
Worktrees on removed branches are detached at the exact same commit, with
status and staged/unstaged binary diffs checked before and after. No worktree
directories, evidence, tags, stash, or untracked files are deleted.

## Integration Decisions

- Original dirty work was committed separately: HGM6110CAN protocol catalog
  (`f00c51d`) and mobile inventory/control bindings plus EMS platform-control
  permission handling (`85917a3`).
- Twenty maximal unmerged tips were integrated. Seven already patch-equivalent
  lines use explicit history-only merges: driver send guards, transport bounds,
  lease worker, storage governance, SHM migration, realtime feedback, and current
  MQTT forwarding. The original `git cherry` evidence is retained locally.
- Preserve the current sealed generic installer and native Qt startup, current
  SHM v11 lifecycle and authorization, bounded background history maintenance,
  PCS realtime scheduling, and AM5/recording behavior. Do not restore obsolete
  tracked cross-build binaries, factory archives, fixed-site installers, or
  synchronous SQLite cleanup from older branches.
- Integrate optional offline probe throttling (default zero), register bool
  decoding, full-runtime point-index audit, robust mutex/multiwriter tests,
  explicit DLT645 write credentials, generic-release credential audit, strict
  full-telemetry store reads, runtime sibling discovery, network resilience,
  and offline event-store tooling.
- Keep archived XMind local controls opt-in and restricted to the XMindV2
  layout. The current Lanhu default layout and its control safeguards remain.
- Resolve duplicate declarations/helpers and a missing discovery include.
  Keep bool whole-register reads distinct from explicit bit and coil reads.
  Update legacy test fixtures for SHM v11, explicit synthetic operator codes,
  and isolated device identity discovery without weakening production checks.
- Historical evidence is retained as history, not represented as a fresh field
  acceptance. No Grok or super-power workflow was used.

## Verification

Native C++ and Allwinner AArch64 GCC 6.3.1 builds were run on 192.168.22.11
from a SHA256-verified isolated source snapshot. The shared build checkout was
not modified. Final tested runtime source is `c2a7caa91b6448625f1d6137c89893443a68496b`;
subsequent changes are this report and whitespace-only catalog output cleanup.

- Native full default build and the EMS control-mode target passed.
- All production edge targets and Qt `KY-SCADA` compiled for ARM64. Qt access
  and Lanhu test executables also compiled; they were not executed on ARM.
- Twenty-two selected ARM64 artifacts were downloaded and reopened locally;
  every SHA256 matches the remote artifact. No deployment was performed.
- Native CTest runs use private network, mount, temporary-file, and SHM
  namespaces. Migration tests additionally use root and a dedicated persistent
  backup directory. This does not provide an on-device timing certification.
- Final native results across 114 registered tests: 113 passed, 1 known OTA
  failure. The main non-root run covered 112 tests (110 passed, 1 failed,
  1 root-only skip) in 327.54 seconds. A separate root run passed both migration
  cases and the skipped handoff-availability case (3/3, 6.19 seconds).
- Linux Python matrix: 161 tests, 159 passed, 2 skipped (optional captured schema
  and compiled migration probe unavailable). Screenshot pipeline, EasyTier
  legacy-systemd, and PCS label checks: 23 passed. Catalog checks: 7 passed.
  Windows-only failures from unavailable `chown`/Unix sockets did not recur in
  Linux; those Windows runs are not counted as successful acceptance.
- PowerShell Lanhu page generation, controls, inventory, and legacy local-control
  package checks passed. Route and generic credential tests passed.
- Historical offline mobile scripts: 21 cases attempted, 1 passed, 17 skipped,
  3 errored because the exact historical site snapshot is absent. No substitute
  snapshot was invented and no site data was fetched to satisfy these fixtures.
- Known remaining OTA assertion: `mutation faults must not prevent reading other
  trusted finals` in `ota_final_state`. The same failure was previously reproduced
  on baseline `dca6578`, as documented in `pcs-collector-scheduling-20261026.md`.
  It was not suppressed or reported as passing in this consolidation.

## Recovery And Audit

Local audit root: `D:/workspace/new/edge-branch-consolidation-20261028`.
`before.bundle` (with separate SHA256), original refs/worktree inventories,
binary dirty patches, and untracked-file archive predate the changes.
`merge-inspection.json` records merge and patch-equivalence inspection.
`remote-*.log` contains native/cross-build and test output.
`verified-arm64-artifacts/manifest.json` records copied artifact hashes and source.
Cleanup receipts record original ref OIDs, verified remote release heads,
same-HEAD worktree detach checks, and the final three-branch inventories.

Push the release to both remotes and verify both heads before deleting any
remote branch. Use explicit old-OID leases and atomic deletion per remote;
abort if a remote ref has advanced. Delete only merged local branches after
their worktrees are detached. This task grants no field deployment or control
authorization and does not touch EasyTier, 4G, or device configuration.
