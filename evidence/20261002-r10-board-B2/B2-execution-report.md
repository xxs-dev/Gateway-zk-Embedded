# R10 B2 board 23 execution

The approved entry was invoked once against `192.168.22.23` (`COMM202600999`),
with manifest SHA256 `bde4dcf1e1ae077a13617873f036c2916c4eed0cb984a8f961165b035a4b2e3b`.
The local command returned exit 0 in 14.95 s (`chunk_id=fe0074`); it left no
ongoing local session handle. Preflight and remote exit codes were both 0,
remote stderr was empty, and the entry recorded `sshClosed=true`. This consumes
the single B2 scope; there was no retry or 22.24 continuation.

All six private-namespace cases exited 0:

| Case | Natural adopted-child reaps | Rescue actions / live remainder |
| --- | ---: | --- |
| `diagnostic_process_test` | 1 | 0 / 0 |
| `system_monitor_diagnostic_test` | 0 | 0 / 0 |
| `system_monitor_service_test` | 0 | 0 / 0 |
| `ownership-probe fd` | 0 | 0 / 0 |
| `ownership-probe parent-death` | 0 | 0 / 0 |
| `ownership-probe supervisor-death` | 1 | 0 / 0 |

The fixture reported `productCleanupProved=true` for each case. The diagnostic
suite output includes pre-command outer death and post-launch outer and inner
guardian death checks. The remote result reported private PID/mount/network
namespace proof, no remaining adopted children or namespace processes, owned
`work` removed, and no actual product lifecycle call. The held SystemMonitor
business ELF was neither uploaded nor executed.

All 10 uploaded inputs passed the remote post-run size/mode/SHA256 validation
against the fixed manifest (`allInputHashesUnchanged=true`):

| Input | SHA256 |
| --- | --- |
| `board-group.py` | `dd850523a7db41766efad6f811e716d0e8e1fdab1ce394577a18ff655bb6398c` |
| `board-run.py` | `86340ba76f6b70afd6a9da68ac51a82f70d107f5e44d169428622edcedbc4b4a` |
| `diagnostic_process_test` | `00dbc1679334651f799c4240e97c69e213245eee8025d64c4ecef272fb6599e1` |
| `environment-base.py` | `25314a64846123cf88aca97283359e6ae2c0cd5176e02a85c8cf3be15ee9140c` |
| `environment.py` | `bc999da43f5bcb8a31a6a2ba3cfb6adbc056fcd5a29802cf1f98fe0696270d35` |
| `namespace-child-base.py` | `1c3ef7f98dd66226c9d617c945e90b20964ecf7c6010c33c7c9d90f81365b158` |
| `namespace-child.py` | `76cd2018c6585f639f216ab92e3553ff23553aff051f3fe4cf56ef72b781b801` |
| `ownership-probe` | `51286334936b0d1a5066a64e23bcc763127066bfa137e99a68b9d4fb1c127c35` |
| `system_monitor_diagnostic_test` | `3c7f63b23d0bba114aaf8449519bf7aeb9ea19077cb24e96b3ba9351c214306a` |
| `system_monitor_service_test` | `61e1d7024bef7f6f416738206a9b76f9fc02980d782bb75586b530611bc881df` |

The 16 protected business snapshot groups matched before/after, including
three readers and seven segment entries; `baselineChanges=[]`. Parent namespace,
mountinfo and SHM inventory also matched exactly before/after (73 SHM entries;
mountinfo SHA256 `f0d7de9e767621116037d5e3c402932893bffa69b29d067a58de3391a4ed09ca`).
Free disk and MemAvailable were sampled but intentionally are not invariant
business fields. No dependency was installed.

`B2-raw-receipts.zip` contains the exact local outer receipt (188246 B,
SHA256 `30383d03438f761b8c1d4c3421f12ceb929581af74f4bc64c40bf5a377c3b651`),
remote receipt (81913 B, SHA256
`69c33641c16d1d565d617bffc591b0754f948bad00bb5fb59c33e3414acf92d0`),
and fixture log (4785 B, SHA256
`dd27be4af2ede45298d03e673495abd56b812680321cdcf3f0a48fe793357788`).
Each ZIP member was read back and SHA-checked against its original; ZIP SHA256
`07db3f78557b01c91b5e983bf97a67335e836c0298149818dde76ac259fcc5f2`.
The outer `execution` and decoded `stdout` are structurally identical to the
downloaded remote receipt.

This is compiled diagnostic test evidence on one board only. It is not a
SystemMonitor business deployment, production approval, restart test, or signed
OTA result. `NO_REFERENCES_OBSERVED`-style observations cannot prove absence of
every kernel/task/mount reference.
