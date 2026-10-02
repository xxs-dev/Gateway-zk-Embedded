# R10 Private Board B1 Proposal

2026-10-02, LOCAL_ONLY. NOT execution authorization. No device, build host, VM,
WSL or Broker access during this preparation. No C++/product/source change.
L2 evidence-only commit:9f581ac4155fa7ad27f18313fb846af5f9058fc4, archive at
evidence/20261002-r10-linux/L2-execution/raw-evidence.zip. No new build required.

## Fixed Identity

Manifest input-manifest.json SHA256:
432bed8c304fb2c53d8dac25ab9a5da0855979e9640a46bd412c960d29dfc890.
Entry execute-board.py SHA256:
d5e958619f806d90f947f0c8a5f84656cb8ef1a9bf7ae16ed17525f8a9813168.
Default command: `python -B evidence/20261002-r10-board/execute-board.py`.
It verifies Git-held L2 archive, all eight remote inputs, two local files,
ELF requirements and existing helper pins; no authentication import/connection.

Only a future exact B1 scope may add --execute-approved and this manifest SHA.
23 only:192.168.22.23/COMM202600999; fresh root
/tmp/gw-r10-68de70f-23-B1, occupied root rejects. No automatic24 or alternate host.
The old board-child.py/namespace-child.py remain unchanged L2 provenance; neither
is executed by this proposal. No dependency on util-linux --kill-child support.

Eight uploads (all mode0500, root/bin or root/source under0700):

| Input | Bytes | SHA256 |
| --- | ---: | --- |
| diagnostic_process_test | 47584 | 5c106996977066de749c236eada781ecbc52cfbb103a1c00b55bab68008f5d56 |
| system_monitor_diagnostic_test | 3381256 | b1998ee50ae4fddd3f2d6c6ed5289c2ef070597bd3b071febb7f0c7446b9028b |
| system_monitor_service_test | 3504368 | 6b5651c08fe3f1ff4bbf17a92c2f1d5043c43d03f8fcf80ed3bc8542e189e332 |
| ownership-probe | 53960 | eff1af135f84221b7b7401e58647f1607c2df8621214e1620dafdeb15dd3b3b7 |
| environment.py | 6499 | 25314a64846123cf88aca97283359e6ae2c0cd5176e02a85c8cf3be15ee9140c |
| board-group.py | 2190 | efd77f5b12b1266ace5bb2a2fb2217072e6f13e5a4679406771ad9f2b3dfab4b |
| namespace-child.py | 8013 | 1c3ef7f98dd66226c9d617c945e90b20964ecf7c6010c33c7c9d90f81365b158 |
| board-run.py | 9458 | 893f662e437f8baafbcd8b131445dc0566aad8e34210fb6011036319d690cf47 |

All four ELF bytes come directly from the pinned L2 Git archive; source438507c,
product68de70f. Local board_runner_test.py SHA
242682e3dc903214383a67fadbf687f8c6c18ce084d61a140dc79cad5cc9be97.
Earlier manifest drafts are local history only, not authorized inputs.

## Admission And Isolation

Before upload: aarch64/root/Python>=3.6, libc/os namespace APIs, effective
KILL/SETPCAP/SYS_CHROOT/SYS_ADMIN/MKNOD, canonical root-owned sticky/tmp,
fresh root, disk>=1GiB and MemAvailable>=2098200576B (2001MiB).
Require trusted existing Python/readelf/AArch64 loader/sh/sleep; no install.
PDEATHSIG-resetting suid/sgid/file capabilities or unsafe tool permissions reject.
/etc/ld.so.preload must be absent/empty. Capture environment plus immutable B2
business sampler raw commands/JSON and parent namespace/mount/SHM inventory.

After fixed uploads, before any test: repeat identity/capacity/tool checks; loader
--verify/--list on each pinned ELF, clean environment, no test main executed.
Resolve only trusted system/lib or usr/lib libraries; readelf -W --dyn-syms must
cover each strong undefined/versioned symbol (72/267/261/63 respectively).
Requirements include GLIBC2.17, GLIBCXX through3.4.22 and CXXABI through1.3.9;
these are ELF requirements, NOT observed board library availability.
DT_RPATH/RUNPATH/FILTER/AUXILIARY are rejected by actual local ELF parsing.
No package/alternate loader/fallback. Missing/unknown dependency fails closed.

Private libc CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWNET/fork launcher; verified
PDEATHSIG on launcher and PID1, host-proc parent identity, pre-GO namespace
identity recorded and fsynced before admission. PID1 makes mount propagation
private before any mount. Chroot exposes read-only /usr,/lib,/lib64,/bin and
optional loader cache, not host/etc secrets/sys/opt/srv or serial devices.
Fresh tmp/run/dev/shm tmpfs, private read-only proc, only null/zero/random/urandom
device nodes. Bind-runtime dev/inode/mode/size checked inside chroot.
Clear bounding/effective/permitted/inheritable/ambient capabilities and set
NoNewPrivs. Network namespace remains isolated; no host systemd bus or Broker.
Real namespace/kernel/capability/loader behavior is still UNTESTED by local tests.

## Six Serial Invocations

diagnostic_process_test (30s), system_monitor_diagnostic_test (30s),
system_monitor_service_test (60s), ownership-probe fd/parent-death/supervisor-death
(5s each). They include actual inherited FD closure/preservation, owning parent's
_exit without destructor, and supervisor SIGKILL after command identity is known.
The fixed ownership-probe returns failure when the command remains alive even
if its own harness rescue succeeds. Inner PID1 sweep records rescued processes;
any rescue/residual/failure stops further cases and cannot yield product PASS.
No summing internal modeled suite assertions into independent real-death results.

## Source-Directed Resource Budget

| Case | File ceiling | SHM segments | Address-space ceiling | CPU |
| --- | ---: | ---: | ---: | ---: |
| diagnostic_process_test | 1MiB | 0 | 512MiB | 20s |
| system_monitor_diagnostic_test | 1MiB | 0 | 512MiB | 20s |
| system_monitor_service_test | 32MiB | 3 | 512MiB | 20s |
| each ownership-probe mode | 1MiB | 0 | 512MiB | 20s |

Service source writes5MiB+1 (plus512KiB+1 and small/config files), and holds
store/systemMonitorStore/lockedStore concurrently. Archived B2 native ABI11 is
15667032B per default segment; layout and constructor blobs equal cb45.
Three segments47001096B, plus16MiB SHM allowance fits128MiB; mapping bytes plus
256MiB code/stack/heap policy headroom fit512MiB AS. This is not a measured peak.
Private /tmp128MiB, /dev/shm128MiB, /run16MiB, /dev1MiB (273MiB total caps).
Parent launcher FSIZE hard/soft32MiB and AS768MiB allow child reductions after
capability drop; no attempt to raise a1MiB inherited hard ceiling.
Log budgets remain independent:1MiB/case, aggregate returned fixture log256KiB;
oversized output is a harness failure, not falsely a product regression.

MemAvailable admission sums three512MiB fork-related address-space policies,
273MiB tmpfs caps,128MiB outer/interpreter allowance,64MiB system reserve. It
deliberately double-counts shared/COW pages; no aggregate cgroup is enforced and
this cannot certify resource exhaustion immunity or production capacity.
Known test geometry and negative budget regressions prevent the identified
FSIZE/SHM conflicts; actual target success remains required.

## Completion And Evidence

Preflight60s; remote fixture420s plus timeout kill-after20s; tests outer240s,
startup10s, cleanup wait/reap5+5s. FDs256, core0; process inventory cap128
(a fail-closed inspection bound, not an enforced PID cgroup).
First error is atomically saved before cleanup; later cleanup/postcheck/receipt
errors remain distinct. Stop only exact owned Popen/PID namespace; never global
kill. Require no namespace survivors AND no unreaped adopted children, including
pre-GO failure where namespace proof may be unavailable. Unknown closure stops.

Only initially-empty exact dev/inode-matched work may be removed, after owned
process absence and unchanged parent mountinfo, with symlink-safe rmtree. Preserve
source/bin/log/result, do not remove old evidence or any business objects.
Final acceptance uses B2 business/parent-namespace baseline AFTER work cleanup,
including identity/boot/state/approval/active/guard/config/marker/readers/PID start
ticks/maps/segments/lock and bounded parent SHM inventory. Rehash8 inputs.
Return raw result/fixture log, preserve local board-23-B1-result.json, close SSH.
Any uncertain call/cleanup/drift stops; no replay, extra retrieval or automatic24.

## Local Checkpoint

Final12 tests PASS0.117s/0skip/exit0; default dry-run exit0/connects=false; six
Python files plus embedded bootstrap Python3.6 grammar PASS, not interpreter proof.
An earlier Windows mock missed WNOHANG and errored before the mocked sweep;
fixed test adapter only, no Linux or product failure. See local-validation.log.
Private file/replace logic is real Windows I/O; namespace/kill/wait APIs are models.
No board fixture or production native boot/full business/signed OTA has run.
Candidate runner remains uncommitted for independent review; L2 archive is committed.
