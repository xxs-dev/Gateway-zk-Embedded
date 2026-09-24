# A Implicit Monitor SHM: Stop-Only Stage

This is local script qualification, NOT A deployment or joint-reader rebinding.
No device, ARM build, Windows package, or historical sealed artifact was changed.

The A-only read probe at `C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-monitor-binding-20260925-01/stdout` has SHA256 `64282fb1a693a156c4b13e2838d70107e01b79ea5013fb0904b60d8a0cb40965`.
It reports `gateway_point_store_system_monitor` V10 mapped by both SystemMonitor
and KY-EMS, with SystemMonitor's environment key and drop-ins absent. The probe
is `INCOMPLETE` because an unrelated initial-stat PID disappearance remains
unclassified; it is not stop readiness and must not be upgraded to PASS.

- `red-coverage.txt` SHA256 `a50898e4129bee2281d801bb166e6fd064eaac2ca03203ef71509f6f6c022d90`: including the real implicit segment was rejected as an extra; omitting it incorrectly reached `UPGRADED_STOPPED`.
- `red-alias-after-stop.txt` SHA256 `99244b3d0979ff5fda2df1f24dec4d6d13322c0e0562fc7f7c026b8370b2e6d7`: a dangling alias or an unlisted alias appearing during stop was missed.
- `green-result.txt` SHA256 `da028851641c5020f37e84c92526fa17ec79781bb66742d443c160b6242f7691`: 11 focused namespace/native CLI tests PASS, including B binding/partial recovery, old default, disabled EMS, dangling drop-in and arbitrary-extra refusals. The earlier 10-PASS result remains at `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/edge-a-monitor-extra-20260925/green/pre-dropin-check-result.txt`. Native fixture SHA256 `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0` remains outside Git. Final script SHA256 `064479af684b98bf4d3a4fb7857e62551ab8e10a5f255f9ca6ba68ef278d1c17`.

The new exception is only the fixed implicit monitor name, explicitly approved
as a segment with both known readers and products in the stopped transaction.
Its old bytes remain in place and recovery preserves the original absence of
`998-shm.conf`. `observe` is refused for this state; this stage does not create
an environment binding or edit a SCADA project. There is no A apply authority.

## Minimum Next Projection

From A only, after independent read approval: `ky-ems.service` effective
`FragmentPath`, `DropInPaths`, `ExecStart`, `Environment`, `EnvironmentFiles`,
`UnsetEnvironment` and hashes of the exact effective unit/drop-in files;
`config/runtime/qt-display.env` existence/hash plus only the
`GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME` key's presence and SHM-name value.
Do not export the other environment or credentials. Pin the candidate
`gateway-qt-run.sh` executable path/hash and confirm no unreviewed launcher
overrides the environment.

For the already known `apps/monitor-service.json` path, project only
`localDisplay.scada.enabled`, `projectDirectory`, and `autoReload` with its
unchanged file hash. If SCADA is enabled, first review that exact directory
and any `current` symlink target; then project only the resolved
`runtime-map.json` path/hash/size, total mapping count, count of mappings
still naming `gateway_point_store_system_monitor`, and those mappings'
`nodeId`, `tagId`, `index`, `sharedMemoryName`, `writable` fields. Return no
point values, unrelated mappings, full environment or large project body.
The loader reads `runtime-map.json` from `projectDirectory`, while the Qt
process separately adds the environment-derived monitor store; both paths
must converge on the same new target before dynamic observation can qualify.
