# Restricted read-only SCADA transaction

Base: 488c3f78370f0187b93041baea7001812252d920. Isolated worktree/branch:
`edge/scada-readonly-20260925`, `codex/scada-readonly-20260925`.
The active realese1.0 worktree is not changed.

## Red

2026-09-25: three new tests ran in the existing WSL private mount/net/IPC fixture.
All three failed, exit 1: the baseline ignored the new project approval, did
not switch current, and did not reject unsupported map/non-A approval scope.
This is a missing feature reproduction, not proof that the baseline opened an
unsafe nonempty map: its existing empty-map gate still applies.

Raw directory:
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/scada-readonly-20260925-red/`.
The original result.txt, per-test logs, stdout/stderr and invocation.json are
retained. Native migration fixture reused without rebuilding, SHA256
`f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.
No device, ARM, Windows packaging or push operation.
