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
