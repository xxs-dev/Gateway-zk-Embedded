# R10 Diagnostic Compatibility: Next Build Proposal

Status: LOCAL_ONLY proposal. No SSH, build-host command, board command, or
deployment is authorized by this document. P2 is consumed. Do not reuse the
old L2 archive, B1 manifest, or board execution scope.

## Fixed Inputs

- Source commit: `eb6cc533ce730d6ff929d05a362070c18f455f6e`.
- Transfer an isolated `git archive` of exactly that commit with Git LF bytes;
  verify `git get-tar-commit-id`, archive SHA256, member paths and blob hashes
  after transfer. Do not modify `/srv/build/Gateway-zk` or its caches.
- Supplemental ownership probe is the same commit's
  `evidence/20261002-r10-linux/ownership-probe.cpp`. It is test-only.
- Build host: `192.168.22.11`, installed Allwinner AArch64 Linaro GCC 6.3.1
  toolchain/sysroot only. Select a new exclusive `/srv/build/...` root after
  read-only occupancy and resource checks; never overwrite an old root.

## Requested Build

From the verified isolated source, use the existing
`tools/build_edge_aarch64.sh --build-dir <new-root>/build --jobs 2` with only
`SystemMonitor diagnostic_process_test system_monitor_diagnostic_test
system_monitor_service_test` as targets. Record the exact command, compiler
identity, full bounded log and exit code. Compile `ownership-probe.cpp` as a
separate test executable with that toolchain and the diagnostic test hook; it
is not a production library or service. No SCADA, Qt, package, install,
service start, or additional business ELF build is proposed.

Before transfer, freeze a manifest for the full commit SHA, archive SHA and
size, archive member/hash list, test-source hashes, expected toolchain and
unique root. Reject an occupied root, changed source, missing pidfd compile
support, compiler failure, or ambiguous transfer. Do not silently retry.
Return only the four selected AArch64 ELFs and the supplemental probe, each
with size, SHA256, ELF class/endian/machine/interpreter and dynamic dependency
metadata, plus bounded logs and a source-integrity/shared-workspace receipt.
The build-host result alone does not prove board kernel, procfs, pidfd,
systemd, or descendant-reaping behavior.

## Later Board Gate

Board execution needs a separate exact scope. It must distinguish the
post-launch outer-supervisor and inner-guardian descendant cases from
`dieBeforeCommand`, and validate missing-children-interface behavior on the
target. No business binary replacement, OTA, or production acceptance is
inferred from this proposal.
