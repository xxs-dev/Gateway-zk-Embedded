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
