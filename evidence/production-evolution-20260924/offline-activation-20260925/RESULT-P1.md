# Local activation P1 receipts (2026-09-25)

All commands ran as WSL root through `tools/install_isolation_test.py` in its
private mount/net/IPC namespaces. The runner wrote its original unittest
stdout to each `result.txt`; command exits were 0. No device, ARM build, or
Windows package operation was performed. The native fixture binary remained
outside Git under LocalAppData; SHA256
`f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.

Evidence root: `C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/`.

| Directory under root | Scope | result.txt SHA256 |
| --- | --- | --- |
| `offline-activation-p1-first-20260925` | 12 activation draft regressions, 12 PASS | `f3a082e3aa674a0b67e59d19bdbc35f3cf27e37930e19c96f4c99a2e79c9b2fe` |
| `offline-activation-p1-focused-20260925` | paired old/new/old scripts, own prestart/dependency, drift-stop/recover, 3 PASS | `a3c25fe1624f00a053a9c3b4e43cf66b7f9582dcb333e798bd3ba5a2f7ad0dc6` |
| `offline-activation-p1-template-20260925` | additional unapproved template instance prestart, 1 PASS | `f403f4cc7ad4161173e15631a5be658e16d32b13f5604ad7fd28a6cc80b4d0b8` |
| `offline-activation-p1-a-watchdog-20260925` | A unknown watchdog env kept fenced; A dependency/failure and B paired scripts, 4 PASS | `66a1ed51d0438b4d6d7b6b59b1812860a42e83b3db3125770e4cf0874eca4e0f` |
| `offline-activation-p1-a-absent-guard-20260925` | real A old guard absence, unexpected-file refusal, install/activate/recover-to-absence with retained candidate, 1 PASS | `b61abbaafc7ca42b35c2051512564007766570c356e91b4cb66efc7cd2684370` |
| `offline-activation-p1-batch-stop-20260925` | first batch-stop run: 3 PASS, 1 FAIL due old per-unit test assertion; batch stop was present in the raw log | `46aa1bede8454dc9c10ede2039d84bedd011032752a52069ecda449895a2b261` |
| `offline-activation-p1-batch-stop-green-20260925` | batch stop/status failure and drift/recover plus A/B paths, 4 PASS | `2e2f753921a5a5730780c30b83a14bbd250b2d971362b0720e760f49e081071f` |
| `offline-activation-p1-refence-final-20260925` | one batch stop/status for all transaction units; stop/status failure, drift, A/B recover and Qt failure, 5 PASS | `f4e02bfac094b74f046d1594f494f7bb78e47d560b9c3fda898b938ce22f8aa3` |

Invocation form for each receipt was
`wsl -u root -- bash -lc 'cd /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0 && python3 tools/install_isolation_test.py --evidence /mnt/c/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/<directory> --test <listed method> ...'`.
The exact method order is the order in the corresponding `result.txt`.
The first 12-test receipt predates the final P1 test additions; the later
focused receipts are differential verification, not a claim that all 12
were rerun on the final tree.

Refence now makes one 45-second bounded batch stop request containing every
approved non-template unit (including all activated readers), then one
45-second bounded batch status request. It
attempts both before any drop-in drift check. The following daemon reload is
also bounded at 45 seconds, so those three external calls total at most
135 seconds, excluding local file operations. Failed/unknown stop or status
remains `FAILED_STOP_UNCONFIRMED` with per-unit recorded results.

The fake systemctl executes each selected unit's `ExecStartPre` and simulates
Qt's Bridge dependency order, but is not a real systemd/device qualification.
Same-boot and cold-boot checks use a fixture boot-id change, not a reboot.
No actual A activation is approved: installed unit/env and runtime mapping
verification remain external acceptance gates.
