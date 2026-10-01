# R10 Build-Only Review Checkpoint

2026-10-02. LOCAL_ONLY. NO execution permission, SSH, WSL, devices or compiler
host access. Runner defaults to local pin verification; board execution is
explicitly disabled by boardExecutionReady=false before any connection.

## Fixed Identity

Product68de70fbc37cf3267116624a259a81510b2d74d3 is unchanged. Source snapshot
438507cdb191b5b91c77eca2e672b4e74e06bd1f adds ONLY the POSIX private-directory
test compatibility correction and its note.442 source/build dependency files,
8,140,800 archive bytes, SHA256
dd8bc90081b89cd52ff7fd7abd2e1633a2c646e2590279a9166a42dfb4d2080f.
Git archive explicitly disables autocrlf and uses LF. Each regular member equals
its actual Git blob; get-tar-commit-id equals438507c. No evidence/history tree,
config, sysroot or existing binary is transferred as source. Seven supplemental
files are pinned separately in input-manifest.json; board files are drafts.

An initial local freeze rejected the default Windows Git archive's CRLF output;
no malformed archive was sent or executed. Valid archive generation is local.
No fabricated HEAD or history bundle is created.

## Build Route For Later Approval

Host192.168.22.11 only. Fresh exclusive root:
`/srv/build/gw-r10-68de70f-build-L1`. No retry or overwrite on occupancy.
Reuse the pinned credential helper without printing secrets. Short read-only
architecture/boot/toolchain/space/memory preflight occurs BEFORE remote writes.
Require x86_64, existing /srv/build/Gateway-zk and the installed Linaro6.3.1 path,
free disk2 GiB and MemAvailable5 GiB. RAM policy allows two2 GiB per-process
compiler ceilings plus1 GiB overhead; it is an admission policy, not measured
compiler peak or an enforced aggregate memory cgroup.

Exact existing entry, with isolated cwd/source/build, no clean/publish/package:

```sh
bash tools/build_edge_aarch64.sh --build-dir /srv/build/gw-r10-68de70f-build-L1/build --jobs 2 diagnostic_process_test system_monitor_diagnostic_test system_monitor_service_test
```

No toolchain or dependency installation/override. Allwinner toolchain selection
comes from AGENTS, tools/build_edge_aarch64.sh and its frozen toolchain file.
The script configures the original CMake project; only these targets and their
matching edge_gateway dependency are built, not18 business ELF or SCADA.
The supplemental ownership-probe.cpp is compiled with the same installed
aarch64-linux-gnu-g++, -std=c++1z/-pthread and diagnostic TEST_HOOKS, using that
exact source's helper. It is not linked into product edge_gateway.

Each build command uses a fresh PID namespace and explicit startup handshake.
Outer records unreaped unshare PID and its exact PID1 namespace before GO.
PDEATHSIG kills unshare if outer dies; --kill-child=KILL kills namespace PID1;
kernel namespace teardown also reaches setsid descendants. TERM/INT/error and
deadline enter cleanup, wait/reap and inspect namespace absence; uncertainty
is a failure, not permission to kill unrelated processes or retry. No global
kill/shutdown, shared cache deletion or dependency installation.

Build command1200 s, ownership compile60 s, each startup included in its deadline;
outer remote timeout1500 s. Reserve240 s for source validation, two15 s Git
snapshots, metadata (four10 s), cleanup (two10 s), logs/fsync and overhead.
Compiler processes:AS2 GiB, FDs256, individual file128 MiB, core0; jobs2.
Per-command log16 MiB; only four<=32 MiB AArch64 ELFs plus bounded logs/metadata
return. Keep root/source/build/logs on any failure. ELF class/endian/e_machine,
size/hash/readelf metadata are recorded; target runtime-library compatibility
is NOT proved by the build host.

Post-build rehash all442 source members. SharedBefore/After compares HEAD and
tracked/untracked path-status hash, NOT all shared file contents; the runner
does not intentionally write shared sources. Artifacts are returned explicitly
and SHA-verified. Any transfer ambiguity keeps receipt/logs and stops, no replay.

## Review Commands (Local Only)

```powershell
python -B evidence/20261002-r10-linux/execute-r10.py
python -B evidence/20261002-r10-linux/execute-r10.py --stage board
```

Only AFTER an exact NEW build scope is approved, coordinator can supply the
frozen manifest SHA with --stage build --execute-approved --manifest-sha256.
No such command has been run. Board scope cannot execute this checkpoint.

## Board Draft And Open Gates

Draft root /tmp/gw-r10-68de70f-23-L1,23 only, no automatic24. Reuses B2's fixed
business/parent-namespace baseline, not its old scope. Proposed isolated PID,
mount and network namespace, chroot, read-only runtime/input, private /tmp,/run,
/dev/shm and capability removal. Three actual C++ suites then fd,parent-death,
supervisor-death experiments, total outer240 s; cleanup-after baseline and raw
outputs. Old fixed SHM names remain ONLY inside a fresh private tmpfs.

The supervisor-death experiment starts a real command, kills its exact owned
supervisor, checks poison/command liveness, and labels any private harness rescue
separately; orphaned command means exit1/product gate OPEN, never product PASS.
Parent-death uses actual _exit of the owner, not destructor; FD test checks
closure in the command and preservation in parent. None has executed here.
No outer namespace cleanup is evidence that product can clean a dead supervisor.

Board draft still needs a separate fixed audit/scope, artifact/dependency pins,
review of boundary/error/cleanup behavior and baseline/output budget. No Linux
flock/fork/namespace/capability/SHM or AArch64 compile/behavior is proved by
Python3.8 grammar or the Windows-excluded C++ syntax branch. Production R10,
transport, full business deployment and owner-death repair remain OPEN.
