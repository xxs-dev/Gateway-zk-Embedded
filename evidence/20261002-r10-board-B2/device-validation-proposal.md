# R10 23-Only B2 Diagnostic Board Proposal

LOCAL_ONLY checkpoint, 2026-10-02. This is not device execution authority.
No connection, upload, test, cleanup or new business service occurred while
preparing it. B1/L2/P2/L3 execution scopes are consumed. The failed B1 root
`/tmp/gw-r10-68de70f-23-B1` and all its evidence must remain untouched.

## Fixed Identity

- Candidate manifest: `input-manifest-B2.json`, SHA256
  `bde4dcf1e1ae077a13617873f036c2916c4eed0cb984a8f961165b035a4b2e3b`.
- Candidate entry: `execute-b2.py`, SHA256
  `01c4c6ceee91b3378c37738abec34caa4a3c42769a6ef38fece074d0f7349de3`.
- Product/source: `eb6cc533ce730d6ff929d05a362070c18f455f6e`.
  L3 evidence commit: `6bb2895de6c5ed9ad350e0ab7cf06b490b5b4dcc`.
  L3 ZIP: `a2417696c5edd980528eec631a6177c661a35a3d6aab8bbb239d49aba89453cb`.
- Only `192.168.22.23` / COMM202600999, only fresh
  `/tmp/gw-r10-eb6cc53-23-B2`. No automatic 22.24 route.
- Default local command is `python -B evidence/20261002-r10-board-B2/execute-b2.py`.
  It checks pins and prints `connects=false`, without importing authentication.
  A separate approved exact scope would be required before passing
  `--execute-approved --manifest-sha256 <fixed SHA>`.

The entry reads the committed L3 ZIP, verifies all 14 members against its
committed summary, then selects only the following ten fixed uploads. All are
mode 0500 in new root-owned `bin/` or `source/` directories under mode 0700.

| Remote input | Bytes | SHA256 |
| --- | ---: | --- |
| `bin/diagnostic_process_test` | 57008 | `00dbc1679334651f799c4240e97c69e213245eee8025d64c4ecef272fb6599e1` |
| `bin/system_monitor_diagnostic_test` | 3385760 | `3c7f63b23d0bba114aaf8449519bf7aeb9ea19077cb24e96b3ba9351c214306a` |
| `bin/system_monitor_service_test` | 3508856 | `61e1d7024bef7f6f416738206a9b76f9fc02980d782bb75586b530611bc881df` |
| `bin/ownership-probe` | 59272 | `51286334936b0d1a5066a64e23bcc763127066bfa137e99a68b9d4fb1c127c35` |
| `source/environment-base.py` | 6499 | `25314a64846123cf88aca97283359e6ae2c0cd5176e02a85c8cf3be15ee9140c` |
| `source/environment.py` | 404 | `bc999da43f5bcb8a31a6a2ba3cfb6adbc056fcd5a29802cf1f98fe0696270d35` |
| `source/board-group.py` | 2863 | `dd850523a7db41766efad6f811e716d0e8e1fdab1ce394577a18ff655bb6398c` |
| `source/board-run.py` | 10076 | `86340ba76f6b70afd6a9da68ac51a82f70d107f5e44d169428622edcedbc4b4a` |
| `source/namespace-child-base.py` | 8013 | `1c3ef7f98dd66226c9d617c945e90b20964ecf7c6010c33c7c9d90f81365b158` |
| `source/namespace-child.py` | 3395 | `76cd2018c6585f639f216ab92e3553ff23553aff051f3fe4cf56ef72b781b801` |

The L3 `SystemMonitor` ELF (SHA256
`843c7ea274935e064f7c0dfc8e9c8f7d5756f8d39685966c13190f383b44bf15`)
is held for a later paired business install; it is neither uploaded nor run
by B2. There is no second business daemon or actual actuator path here.

## Admission And Execution

Reuse B1's read-only AArch64/root/Python/capability, memory >= 2001 MiB and
disk >= 1 GiB checks. Preflight additionally records current bounded
`/proc/thread-self/children` availability or errno; historical B1 errno 2 is
not substituted for current fact. Upload only after preflight and fresh-root
check. The already-pinned loader `--verify`/`--list`, trusted-library and
strong/versioned symbol checks run for the four uploaded ELFs before GO; no
test executable main is run by the dependency gate. No dependency install or
alternate loader is allowed.

The fixed launcher retains private PID/mount/network namespaces, exact
unreaped host PID1 identity (PPid, NSpid and namespace links), PDEATHSIG,
fsynced proof before GO, read-only host runtime mounts, private tmp/run/dev/shm
and proc, chroot, capability removal and NoNewPrivs. B1's missing
`/proc/.../children` lookup is absent from the pinned `board-run.py`; its
owned-child cleanup now requires ECHILD. None of this local preparation proves
those kernel operations on the actual target.

Run serially only: `diagnostic_process_test` <=30s,
`system_monitor_diagnostic_test` <=30s, `system_monitor_service_test` <=60s,
then `ownership-probe` modes `fd`, `parent-death`, `supervisor-death` <=5s each.
The diagnostic executable includes pre-command outer death, post-launch outer
death and distinct post-launch inner-guardian death; these are internal test
assertions, not six additional standalone board invocations. A board PASS of
its forced proc/pidfd fallback is required for actual target capability;
Windows/WSL PASS alone is insufficient.

The B2-only PID1 adapter waits up to 3s with `waitpid(WNOHANG)` for adopted
children to exit and records their exact PID/raw exit or signal status as
`naturalReaped`. It then checks remaining private-namespace `/proc` status,
NSpid and namespace identity. Any still-present process needing SIGKILL is
recorded as rescue and forces failure, even if cleanup succeeds. Unknown
identity or residual process also fails. It never treats a PID number or
fixture rescue as product descendant cleanup proof.

Reuse B1's case file/AS/CPU/FD/core budgets, private tmpfs caps and 240s
outer deadline (remote call 420s plus existing kill-after bound). The extra
natural/recovery waits are each bounded by 3s per case and fit within the
outer deadline policy; that is not a measured peak or aggregate cgroup limit.
The first error is persisted before independent cleanup/postcheck/receipt
errors. Stop on the first failed/rescued case; do not replay or move to 24.

## Completion And Open Gates

Before/after proofs compare the same 16 business fields plus parent PID/mount
namespace, mountinfo and bounded SHM inventory; all ten inputs are rehashed.
Only exact owned processes can be signalled. Cleanup requires ECHILD and no
private namespace survivor; the new work directory is removed only if its
recorded dev/inode and unchanged parent mountinfo allow the existing safe
cleanup. Retain source/bin/log/result and all old B1 evidence. Return bounded
raw receipt/log, close SSH, stop on uncertain closure. No systemd/Broker/DB,
production install, OTA or platform action is in this scope.

Local checkpoint: eight targeted B2 tests PASS, exit 0; final default dry-run
exit 0 with `connects=false`. These are Windows local parser/mock and byte-pin
results, not Linux namespace, board pidfd, native service or production proof.
After independent review, B2 still needs a new one-invocation 23-only device
scope. Factory helper closure, full business replacement, signed OTA and
Windows/platform matched acceptance remain open.
