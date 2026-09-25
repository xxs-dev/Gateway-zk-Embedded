# Successor boundary regressions

## Red

2026-09-25; product base d4cb9348cece82d694e049d3af3bce31d9c77c13.
Two regressions ran once in the existing WSL private mount/net/IPC fixture.
Both failed; runner exit 1. Native fixture reused, not rebuilt, SHA256
`f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.

Raw directory:
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/offline-handoff-boundaries-20260925-red/`.
`invocation.json` records arguments, UTC start/end and exit; stdout/stderr and
per-test CLI logs remain in that directory. No prior evidence was overwritten.

- `result.txt`: `d62fa319b476b3e7c46d1e258ca5fc5a932cb1b0167d2297ce8b2d87034c8157`.
- `test_offline_recovered_handoff_ancestor_id_reuse_refused.log`:
  `960f7d5e04d7b867381fa8ace8c3a50627e81a56f63741b5e9335e4dfdf113d9`.
- `test_offline_recovered_handoff_ancestor_id_reuse_refused-observed.json`:
  `03de8510732cc636e9448265e22e462b1f6825dd1c2e22e84c9fe5c541347146`.
  T1 -> T2 -> T1 apply exited 0; original T1 recover exited 2 but had already
  changed T1 state. New state directory and targets did not prevent ID replay.
- `test_offline_recovered_handoff_interrupted_after_partial_fence_write.log`:
  `50b51831340d432bc97c9ce455cde1dc93afc1a53864157911830e3332d51151`.
  Injection persisted the first 8 bytes of the final successor fence and then
  raised. New official recover exited 2 on fence drift; old recover refused
  marker ownership. Continuous fencing alone did not provide recovery.

No device execution, ARM build, Windows packaging, or push. These are local
file-operation fault injections and fake-systemctl tests, not power-cut or
real systemd qualification. A e161306 artifacts remain independent.

## Green

Red-only commit: `a436e973cbb7f67dc093c698b3db457df72bee24`.
Both green runs exited 0 with a complete runner and nonempty result receipts.
Raw directories share the LocalAppData parent above:

| Directory | Result | result.txt SHA256 |
| --- | --- | --- |
| `offline-handoff-boundaries-20260925-green` | two red regressions, 2 PASS | `7f09a7405023535b8a87703448a62a7faa9464c856b77c6d5fb212c5110ff6a7` |
| `offline-handoff-boundaries-20260925-focused` | associated regressions, 9 PASS | `195820edcca3fb873a22bc6cecb9107b4c934cc4c08c1051e6400da4cade4c81` |

`receipts.json` pins every original result, invocation, stdout/stderr,
per-test CLI log, interruption snapshot and fixture provenance. Generated ELF
fixtures stay outside Git. No red log was replaced. The test file gained the
publication-boundary tests between the two green runs; product bytes did not
change between them. Tested upgrader SHA256:
`a4c1f31ec8876133e3268ba7b26874c06235481f0c3e7aa3aee9dd601e57debb`.
Unchanged runtime guard SHA256:
`fee15b210fff4200932fa0a907126eb6df40f33900cd866b3701d239f8810997`.

The lineage walk checks only existing pinned ancestor approvals, not ancestral
SHM bodies or a new global registry. T1 -> T2 -> T1 now refuses before creating
the third state directory; original T1 recovery cannot change T1 state or T2
marker ownership. The supported three-distinct-ID interrupted-recovery chain
continues to work.

Publication uses a unique, hidden non-.conf stage plus atomic Linux no-replace
rename. Half-written stages remain intact; official recovery publishes a fresh
complete fence and finishes RECOVERED_STOPPED. Tests also cover interruption
after full publication, unavailable publication before marker transfer,
foreign destination appearing after staging, dangling destination symlink,
and ENOSYS without any overwrite fallback. The libc-wrapper and native x86_64
syscall fallback paths both executed; no AArch64 syscall/device execution is
claimed. Final names remain single-link files, matching existing regular-file
checks. Recovery never adopts a partial or foreign final fence.

Associated coverage: B schema2/no startup pins and A initially absent guard
apply/activate/recover successor roundtrips, drift rejection, and interruption
before marker, before fence creation, and during old-fence archival. Each
interruption test also repeats official recovery, rejects apply replay, and
successfully applies/recovers a further independently approved successor.
Python 3.6 AST/compile and NUL checks passed for upgrader, guard and test runner;
git diff whitespace checks passed. This is grammar/API-compatible source,
not an execution on an actual Python 3.6 interpreter.

Remaining boundaries: no hardware power-cut/durability test, real systemd
execution or device kernel/filesystem qualification. Unsupported renameat2
fails closed during pre-transfer archival. Deliberately introduced foreign
final paths still refuse and remain preserved; recovery does not overwrite
them. Existing source/backup/retained-target, profile and activation gates are
unchanged. No device readiness, physical production approval or change to the
independent A e161306 maintenance qualification is implied.
