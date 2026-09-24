# A implicit monitor/Qt binding: local stage

Local-only WSL private mount/network/IPC namespace tests. No SSH, device,
ARM build, Windows package or existing sealed artifact was changed. The actual
A Qt projection was read from
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-qt-binding-20260925-01/stdout`
(SHA256 `46d2304b312893c74734ee8b6a195784714c3b643839215f778364828563ed53`).
Its `runtime-map.json` is exactly `[]\n` (SHA256
`37517e5f3dc66819f61f5a7bb8ace1921282415f10551d2defa5c3eb0985b570`).
Both old processes had the monitor SHM environment key absent. This is not an
A apply/observe qualification; historical PID disappearance remains UNKNOWN.

## Test receipts

- `red-fixed/result.txt` SHA256 `c86c9112dba51fbd4aaa34c7336e66fe04a364964311ee121cd010be81a66412`:
  2 expected failures: original installer did not switch Qt env and accepted an
  already-bound key. `red/result.txt` is an earlier fixture-setup error, retained.
- `red-observe/result.txt` SHA256 `19f6a195343473ca24903be8240963d034fd75bd8a85e08acbae78ec2235198b`:
  monitor-only observe was rejected by the old binding requirement. The Qt
  selection remains intentionally refused.
- `red-inbound/result.txt` SHA256 `0fd8eae8596440c47dcb0522dba7b8ee4a7360fdc374dd6c8822dcab0be8afeb`:
  `mqtt.enabled:true` and `systemMonitor.directMaintenance.enabled:true` both
  incorrectly entered observation before the narrow guard.
- `green-focused/result.txt` SHA256 `47762314d4c344cacfbb70c3b1c5ba0afa4eb5b2a40cfbf6b9834967acba1156`:
  9/9 PASS covering joint apply/recover, monitor-only observe, forbidden Qt
  selection/nonempty map, both inbound gates, old A stop-only and B roundtrips.
- `green-partial-recover/result.txt` SHA256 `387d06dc8ebd6420bf51fefdf496a3c1d5ffe48194bba1964a5ad52f9421f471`:
  1/1 PASS after a failed product-file switch; recovery restored original Qt
  env bytes and monitor drop-in absence.
- `green-legacy-inbound/result.txt` SHA256 `b8c105217bcae49d82c950791677fb08792b637edcac20a0bd2140025e714b39`:
  1/1 PASS: absent nested direct-maintenance section with legacy listen key
  remains refused. Other intermediate red/green receipts are retained as-is.
- `green-sibling-loader/result.txt` SHA256 `ff0e20f30152e312e1fff5a172721454289935795f222acde592f3ebea22ab21`:
  2/2 PASS: the monitor observation retains the A three-app/one-device loader
  shape and rejects an enabled sibling camera.

All raw test method logs, compile logs and fixture provenance JSON remain in
their respective directories. Thirteen generated native `offline-upgrade-fixture`
ELFs, each SHA256 `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`,
were moved to
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-joint-monitor-qt-20260925/native-fixtures/<receipt-dir>.elf`.
They are not release binaries and are not tracked here.

## Dependency probe and boundary

The two unique user-service unit fixtures under `systemd-loader/` were linked
to the local WSL systemd 259 user manager for one short start, then stopped and
unlinked. The condition-skipped dependency had `ConditionResult=no`,
`ExecMainPID=0`; the `Requires=`/`After=` main `/bin/true` service had
`Result=success`, `ExecMainStatus=0`. No persistent user unit link remained.
This is a console-observed local loader result, not a saved raw systemctl log or
device startup qualification.

The new approval pin only binds the two readers to the same approved V11
monitor target and supports exact stopped recovery. This stage permits only a
monitor-only observation when effective `mqtt.enabled` and nested
`systemMonitor.directMaintenance.enabled` are explicitly false. Those actual A
values are still UNKNOWN and require the planned fresh intake. Qt and its
write-capable Bridge remain inhibited; no Qt V11 mapping or display acceptance
has been established. The old default and every source remain protected by
the existing stopped digest/unmapped checks. B's existing transaction format
and independent monitor binding remain compatible.
