# R10 Children-Interface Compatibility: Local Checkpoint

2026-10-02. This checkpoint is local-only. P2 was consumed previously; no
device/build-host connection, B1 replay, cleanup, deployment, or push occurred.
The archived P2 raw receipt is `P2-raw-evidence.zip`; `raw-archive-manifest.json`
distinguishes its 101238-byte original CRLF member SHA256
`1a590feb6a54b05622d311ea704a62a01d45b83426ca07b38df18179bb643c0b`
from the 100094-byte Git LF blob SHA256
`a836a63caae7eb0d0d1877f71de9d5bc85307945905ada9a30ae444247a7b0ff`.
The ZIP member was independently hashed against the working raw receipt.

## Change Boundary

The diagnostic process retains its private subreaper, exact unreaped leader
PID/PGID, bounded output/status channels, cancellation and poison latch. An
outer supervisor and inner guardian independently adopt descendants if the
other dies. The existing `/proc/thread-self/children` path remains primary;
when absent, a bounded procfs-only scan accepts only direct children whose
kernel PPid is the current reaper, signals through pidfd, and waits for ECHILD.
Fallback capability is checked before creating a command. AArch64 pidfd
syscall numbers are defined for the old fixed headers, but actual runtime
ENOSYS/EPERM still fails closed. No arbitrary PID discovered by a scan is
passed to `kill(pid)`.

The board runner's launcher now reports its own still-unreaped PID1 child via
a bounded private pipe. The outer runner checks exact PPid, NSpid and three
namespace links before GO; cleanup requires the private subreaper's ECHILD.
It does not turn rescue into product PASS. Archived B1 failure and old manifest
remain historical; changed runner files need a new fixed board scope.

## Local Verification

- Windows `board_runner_test.py`: 13 tests PASS, exit 0, including wrong PPid
  and wrong NSpid rejection. `diagnostic_process_source_test.py`: 3 PASS,
  exit 0.
- WSL x86_64 `g++ -std=c++17 -Wall -Wextra -Werror` production-object compile:
  exit 0. Test-hook diagnostic executable compile: exit 0; bounded 30-second
  run: exit 0. It exercised forced proc/pidfd fallback, detached descendants,
  post-launch outer-supervisor death, and a distinct post-launch inner-guardian
  death case that kills the exact guardian after `CHILD=` is reported and checks
  the escaped descendant is gone. `private-channel-owner-fault-1` separately
  exercises `dieBeforeCommand`; it is not counted as post-launch coverage.
  Pre-command outer death, FD/output/status bounds, timeout, cancellation and
  destructor cleanup also passed locally. None of these is board evidence.
- WSL ownership-probe `fd`, `parent-death`, `supervisor-death`: each exit 0;
  supervisor-death reported `productCommandAbsent=true` and
  `outerHarnessRescued=false`. These were run before the final proc-stat
  bounded-read hardening; the final diagnostic executable run above covers
  that parser and the same outer-death fallback path.
- WSL x86_64 compilation/run with simulated missing AArch64 pidfd header
  macros: exit 0. This only covers the preprocessor fallback and local kernel,
  not actual Linaro GCC 6.3.1 or board syscall behavior.
- Controlled old-`getppid()` variant: first trial was falsely green because
  the test polled and closed cancellation early. Strengthened test waits for
  the exact adopted guardian to exit with cancellation unchanged and checks
  an independent command-start marker. That old variant then exited 1 with
  `pre-command reparenting launched command`; restored fixed-owner source
  exited 0. The old variant was never committed.

Final working-file SHA256: `src/diagnostic_process.cpp`
`afb3dcc4992c3a4cf6fd66ffc5932dfb533af2d431c909323ec4914a052139ba`,
`tools/diagnostic_process_test.cpp`
`acd5b61bc2bec9e0d5c057de018e8763638610a2fec680789e14096eea126650`,
`evidence/20261002-r10-board/board-run.py`
`86340ba76f6b70afd6a9da68ac51a82f70d107f5e44d169428622edcedbc4b4a`.

## Remaining Gates

No actual AArch64 GCC 6.3.1 build, board test of missing children interfaces,
systemd service integration, or product deployment was run. Simultaneous
unrecoverable death of both private reapers is outside this local proof;
systemd's final cgroup kill behavior is not represented by these fixtures.
The proc scan is bounded/non-atomic and may refuse work under hidepid, procfs
drift, excessive process count or unsupported pidfd. Those are availability
limits, not permission to weaken ownership proof. Full business deployment,
signed OTA, matched client and three-end acceptance remain open.
