# Recovered standalone successor lifecycle

Red receipt: 2026-09-25, WSL private mount/net/IPC fixture, 2 tests failed at
`another offline operation holds stop fence` on the second transaction apply.
Both predecessors are schema2 standalone without activationStartupScripts;
the A fixture has no installed runtime-upgrade-guard.py.

Raw directory:
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/offline-handoff-20260925-red/`.
`result.txt` SHA256:
`3fa270563b0762a03113ff51d60d4de2c94d54fed65ccb67acf6178884b912d1`.
The two per-test `.log` files retain original CLI stdout and exit codes.
The runner exited 1. Methods:
- `test_offline_recovered_b_handoff_activation_roundtrip`
- `test_offline_recovered_a_absent_guard_handoff_activation_roundtrip`

Command: `wsl -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0/tools/install_isolation_test.py --evidence /mnt/c/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/offline-handoff-20260925-red --test test_offline_recovered_b_handoff_activation_roundtrip --test test_offline_recovered_a_absent_guard_handoff_activation_roundtrip`.

Native fixture reused from the preceding P1 evidence, SHA256
`f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.
No device execution, ARM build, packaging, or push.

## Green receipts

The red-only commit is `9c8950f`. Green runs exited 0. Raw directories use
the same LocalAppData parent above. `receipts.json` pins every original result,
per-test CLI log, fixture provenance and injected-interruption state snapshot.
No raw receipt was overwritten.

| Directory | Result | result.txt SHA256 |
| --- | --- | --- |
| `offline-handoff-20260925-green-initial` | A/B roundtrips, 2 PASS | `c8bc7b78f1d94747ab1b150b14be06d3bf874e8f625384c5913795f68282d382` |
| `offline-handoff-20260925-interruptions` | drift/interruption and focused compatibility, 7 PASS | `0e9a7287a08fe43bc1cea83f3b9bbf5a6b1a5ad4c8b05ee95901dd319e6445cc` |
| `offline-handoff-20260925-final` | final focused set, 9 PASS | `888dcd9e4089be3e3ad1f590b8d1145dd95d5f552d057e716385e7cec0ab9ec7` |

Final invocation: `wsl -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0/tools/install_isolation_test.py --evidence /mnt/c/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/offline-handoff-20260925-final --test test_offline_recovered_b_handoff_activation_roundtrip --test test_offline_recovered_a_absent_guard_handoff_activation_roundtrip --test test_offline_recovered_handoff_drift_refused_before_ownership --test test_offline_recovered_handoff_interrupted_before_marker --test test_offline_recovered_handoff_interrupted_before_new_fence --test test_offline_recovered_handoff_interrupted_during_archive --test test_offline_a_joint_monitor_qt_observe_and_recover --test test_offline_real_r4_roster_and_app --test test_offline_real_s2_roster_and_app`.

The implementation adds optional, pinned `recoveredFrom` only to standalone
A/B apply. It preserves predecessor state/approval, original SHM, retained
targets and dedup. Both predecessor fixtures have no startup-script approval;
the guard is absent until successor apply and absent again after recovery.
New targets are distinct. Old recover is refused once marker ownership moves.
The new transaction archives only exact old owned fences, always retaining
the marker and an inhibition file for every unit throughout transfer.

Fault injection uses unittest.mock at existing file operation boundaries,
not product hooks: before marker replacement, before first new fence, and
partway through archival. Each failed apply is recovered via the new official
recover CLI, repeated recovery succeeds, apply replay refuses, and a further
independently pinned transaction can apply/recover. This verifies recoverable
exceptions at the chosen persisted checkpoints; it is not a hardware power-cut
or a real systemd/service execution test.

Negative coverage includes marker/state/receipt/source/retained-target/backup/
owned-fence drift, extra units, fixed-voter mode, pre-existing foreign successor
fence, and omission of a completed target from a newly pinned receipt.
Existing A monitor/Qt and real fixed-voter R4/S2 input fixtures still pass.
Python 3.6 grammar, NUL-byte checks and git diff whitespace checks passed.
The existing runner remains complete and wrote nonempty result receipts.

Remaining boundary: source SHM and every completed predecessor target must
still exist; no cold reconstruction, source deletion, arbitrary service scope,
or physical control is authorized. Actual A/B readiness, target inode mappings,
systemd behavior and deployment approval remain implementation acceptance.
The independently fixed A e161306 maintenance artifacts were not changed.
