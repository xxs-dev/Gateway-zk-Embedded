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

Red test commit: `ed279aee243d683336353b41a57b65d794c488c2`.
Red result SHA256:
`7cc1d53b9765afba6b26ca8dd3691bf74e9b529e8c095f8b4d8a1ed45226d167`.

## Results

Raw directories use the same LocalAppData parent as above. `receipts.json`
pins all original files (including failures), commands, UTC times, exits and
private native artifact hashes. ELF files are not committed.

| Suffix after scada-readonly-20260925- | Result | result.txt SHA256 |
| --- | --- | --- |
| green-initial | 3 PASS, exit 0 | `7cbe19e6ea92af326d3ff144861c964a660e992653245724e1fdb1422a693a99` |
| focused | 10 PASS, exit 0 | `db00660bd51a5574178f330054104458fba76b41aa1355d46adbe44d549d00cb` |
| observe-drift | 1 PASS, exit 0 | `65e13ab25ab74f07ed1b1530840fee40e20e57996297edb11cda478d639bca67` |
| recovery-final | 4 PASS, exit 0 | `e57854eeb1988173c3d927f89e9065ed4d7ad725446df51a28ed4026ee222459` |
| successor | 1 FAIL, exit 1 | `36de330a5667385022dbe1ead05e9c0cfcb48ffffa0b35601002a6678976c34d` |
| successor-green | 2 PASS, exit 0 | `f960c3039ea20f3a097fc27969674ae2533b4708bf0fa05dffcd3bda30034477` |

The initial green preceded the added full A-profile preflight. The focused
run included that check, the real native read fixture, both switch interruption
boundaries, per-file/schema refusals and old A/B compatibility. Recovery-final
ran after moving project restoration after the existing backup validation loop.
Successor failed because the pre-start fixture hardcoded the first transaction's
drop-in filename. The fixture now selects the persisted marker owner, executes
the real guard command, and the last run verifies both the nonempty successor
and denied-unit/template/bridge pre-start refusal. No product guard was relaxed
to fix that fixture failure. Original failed output is retained.

Final script byte pins:
- offline-runtime-upgrade.py:
  `930b2a83a1c962c4c6af3d47ce976526badc1f13029344c06490541206583edd`
- runtime-upgrade-guard.py:
  `363183c813bd956c487dd8785a555fdd9f46fe5fdbc08f046046b4e82dfe7e87`

## Native Read

Only a native test executable was compiled, using the current test source and
cached native libedge_gateway.a; build argv/stdout/stderr/exits are in focused/.
The first link failed because its output directory did not exist as seen from
WSL; that failure remains as native-build.*, and native-build-2.* records the
successful link after creating the directory. No ARM or product binary rebuilt.

Fixture executable SHA256:
`e03355f350816e87eb1738545f46c11b5783831dfbc8c566b1f7a34c5b2c3100`.
Library SHA256:
`d9c02f29e4fa2c830db033ccefc038ca12315c260318e9b9a213f77115226ad2`.
SCADA loader/runtime-map/router/system-monitor-points sources have no diff
between the cached candidate source ff9cf1a and base 488c3f7. This is not a
fresh full-library build or target-ELF qualification.

The existing fixture's new --readonly-monitor entry loads the same candidate
project with the actual ScadaProjectLoader, registers system_monitor_points,
and uses MemoryPointStore/PointStoreRouter/ScadaRuntimeMap::readScreen in the
private namespace. Actual output:
`readonly monitor index=920000005 value=42 quality=1; write and old-store route refused`.
It submits a write only to the local read-only API negative test; no physical
driver, bridge, network or UI interaction is involved. This verifies native
loader/routing, not a rendered Qt screenshot or live SystemMonitor sample.

## Scope And Recovery

The optional, explicit scadaReadOnlyProject approval binds both complete release
inventories and exact old/new current targets. The old release remains byte-for-
byte untouched; only a preexisting approved candidate can be selected. Six
candidate files, single matching machine/node, 1-9 explicit read-only monitor
indexes, and metricCard-only overview are supported. Unknown stores, fallback,
writable tags/maps, other widgets, foreign nodes, extra files, aliases and pin
drift refuse. Existing app/env SHM migration and all physical/bridge/default
fences remain; no general project generator or C++ product code changed.

Apply also sets autoReload=false under this explicit profile and the existing
config backup; recover restores original app bytes/mode and symlink text.
Observe/activation/direct startup share the project validator; list does not
scan project bodies. Candidate bytes may drift after use: startup refuses, and
stopped recovery preserves those changed bytes while restoring the verified
old binding. Unknown current or changed old release refuses after stop. Both
before/after atomic-switch failures complete official repeated recovery with
no start. A recovered old schema2/absent-guard transaction can use the existing
successor handoff, apply this project, activate through modeled ExecStartPre,
then recover to old empty project and original guard absence.

Existing empty-map A observe, old A successor and B restricted activation remain
covered. Python 3.6 AST/compile/NUL checks passed (static/ raw receipt); this is
not execution by Python 3.6. Git whitespace checks passed. Tests use isolated
mount/net/IPC and fake systemctl with selected real pre-start commands, not
actual device services or systemd. No existing active approval was modified.

Remaining: externally approved complete real project pins and prestaged release,
device-specific admission, actual Qt rendering/mappings, and hardware/PCS
qualification. This branch is local-only; it is not merged into realese1.0,
included in Windows, or authorized for the ongoing A/B acceptance.
