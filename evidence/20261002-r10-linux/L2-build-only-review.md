# R10 L2 Build-Only Checkpoint

2026-10-02. LOCAL_ONLY; no SSH, WSL, devices, compiler host, namespace or
compiler invocation in this correction. No product source change. This is a
new proposal, not reuse of the consumed L1/P1 execution authorization.

## Fixed Inputs

Product commit: 68de70fbc37cf3267116624a259a81510b2d74d3.
Source commit: 438507cdb191b5b91c77eca2e672b4e74e06bd1f.
442 Git-byte source files, 8140800 archive bytes; SHA256
dd8bc90081b89cd52ff7fd7abd2e1633a2c646e2590279a9166a42dfb4d2080f.
The existing source.tar is reused only after full equality; the old manifest,
L1 records and remote root are not overwritten.

New input-manifest-L2.json SHA256:
92ab58ad902f63e1742ddb7d133367b3e3435f011b7bc35a6c339aa453a4de06.
Eight supplements remain individually pinned; two board draft files are
provenance only. execute-r10.py has ONLY a build stage, no board execution route.

Fixed SHA256:

- execute-r10.py: 750011726091df3dfb31c5549442a1521f15567d3b7534aa947b6f5054d5314a
- build-child.py: eb8b06d93921e7d80bbc9e3f13e262d6b295a36e6fe8b9094b2f54aaf9527e19
- build-group.py: d8394316f09f28e473b9973b68d73a62d605f9c39bc97b8ad8e35e4a68f09cfa
- build-environment.py: 3d469e73c6f7ba507ecbc93f1e198d288713f4f3e702113b33ac35ce0b4091b4
- prepare-r10.py: 7aadee1ad68b9bb1cf59b64e96db70242d3bd191467089af58b2df7e51aeca91
- L2_runner_test.py: 0e0e3c7d6e0879c4e5b7eb11c432642805211063bcf70ba70bbc675678c4bde3

## Correction And Local Results

Fixed /usr/bin/git runs the three read-only verbs as the verified tronlong
UID/GID1000 with optional locks/configured hooks/fsmonitor/external diff disabled.
Reject configured clean/smudge/process/textconv drivers before shared status.
No safe.directory edits or shared-tree chmod/chown/cache deletion. Shared proof
is HEAD plus tracked/untracked path status, not complete content hashing.

Resolve CMake/bash along the supported build PATH, constrain candidates and bind
realpath/owner/mode/dev/inode/version before and after uploads. CMake >=3.20;
existing Linaro6.3.1 only. Preflight is read-only and creates no namespace.
SFTP checks only available lstat type/uid/gid/mode; real remote os.stat supplies
dev/inode, rechecked by build-child before unpack/build. Actual Paramiko4.0.0
SFTPAttributes without dev/inode passes; symlink/owner/mode drift refuses.

Replace unsupported unshare --kill-child with libc CLONE_NEWPID/fork and verified
PDEATHSIG on launcher and PID1. Host-mounted proc PPid closes the already-dead
parent setup window. PID1 waits GO; parent retains its unreaped exact child
through final signal. Namespace/PDEATHSIG kernel semantics remain unverified.

Preserve primary errors before cleanup/finally, record postcheck/receipt failures
separately, atomic private JSON receipts. Receipt failure blocks compiler admission
before launcher/GO. No uncertain cleanup is accepted as success.

14 local tests PASS, 0skip, exit0 (0.038s); default dry-run exit0/connects=false;
five files Python3.6 grammar PASS; diff-check exit0. See L2-local-validation.log.
An earlier dry-run preceded freeze completion and refused missing manifest with
exit1, before any connection. It is retained, not relabeled as a product failure.
Private real file write/replace tests ran on Windows; directory fsync, Unix
credentials and namespace APIs are mocked. Not Linux/AArch64 runtime evidence.

## Proposed Next Build Scope (Needs New Approval)

Host192.168.22.11 only; fresh root /srv/build/gw-r10-68de70f-build-L2,
exclusive creation, occupied root refuses. Local receipt build-L2-result.json;
returns L2-remote-* and L2-artifacts/. Board execution remains false.

Preflight60s before uploads; MemAvailable >=5GiB, free disk >=2GiB.
Five uploads: source.tar, build-child.py, build-group.py, build-environment.py,
ownership-probe.cpp. Existing script with --jobs 2 builds ONLY
diagnostic_process_test, system_monitor_diagnostic_test, system_monitor_service_test
and their required edge_gateway library; ownership-probe compiled separately.
No business executable/package rebuild, dependency installation, clean or publish.

Remote outer1650s plus timeout kill-after20s; compile1200s and probe compile60s.
Startup included; namespace cleanup wait/reap5+5s per command. AS2GiB per process,
FD256, file128MiB, core0; logs16MiB per command. This is not an aggregate cgroup
memory ceiling. Return four <=32MiB AArch64 ELFs plus bounded logs/metadata;
verify returned SHA and all442 source hashes. On any uncertainty retain evidence,
close SSH and stop; do not replay or signal unrelated processes.

Default review command:
`python -B evidence/20261002-r10-linux/execute-r10.py`
Only a new explicit build scope may add --execute-approved and this manifest SHA.

## Open Gates And Preserved Evidence

Actual Linux UID-drop/Git behavior, Python3.6 interpreter, CMake/bash/compiler
identities and compatibility, namespace permission, pre-GO/parent-death/setsid
closure, timeout cleanup, compilation and artifact hashes remain unverified.
Build execution alone is not the board ownership regression or production proof.
Board execution/deployment/full business/OTA gates remain OPEN.

L1 primary try error remains unknown; finally Git dubious ownership is the visible
error, not proof of the first cause. Existing raw receipt SHA is unchanged:
ccc435a4d6bd13643ed9cfeb6491772bc6fadaa922975c19e74c0e8131f1b13c.
L1 archive unchanged: bcf870783e22e991c39225ecbb7951489a4c6751f3a8cd615ba4c898165355f0.
P1 unchanged: fcbb2e2af51b8227fdb3b174ce9dc59992fdf107d5b0791c773f3e245d427d45.
No live local command session remains; unknown old WSL47230 was not queried,
killed, restarted or used as evidence for these inputs.
