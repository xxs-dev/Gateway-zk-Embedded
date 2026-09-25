# A joint monitor/Qt observe: local bounded regression

Base product commit: `61cb45b839f633c41cb8b9f4bdf221d0c7643137`.
Tests ran locally on 2026-09-25 in WSL root private mount/net/IPC namespaces
through `tools/install_isolation_test.py --evidence <receipt> --test <method>`.
No device, ARM build, Windows package, or Gateway host service was touched.

## Receipts

- `red/result.txt` SHA256 `90cf0053764d2b88986ff984901869b4392705136e870f15bd6a0da913c035a9`:
  3/3 expected failures at the old observer whitelist. This precedes product edits.
- `green-initial/result.txt` SHA256 `2c7c4b7ba75a25bfbea52ce0ff102d36fec78c418544cd694452173f53180926`:
  4/4 PASS after the bounded path was added, before final stop-PID and bridge-condition checks.
- `green-focused/result.txt` SHA256 `e89ca9a610073b5930b011f2c0989826fa3aef8921aadf598f23d8e744b35665`:
  9/9 PASS: ordered monitor+Qt start/recover; Qt start failure refences and stops
  both readers and Bridge; unexpected Bridge activation and non-skipped condition
  refuse; inactive graphical target refuses before start; reversed reader order and
  nonempty map refuse; existing monitor-only and B monitor binding roundtrips pass.

Raw per-method stdout/exit logs, fixture compile logs and provenance JSON are in
each receipt directory. All three native `offline-upgrade-fixture` ELFs were
moved to
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-joint-monitor-qt-observe-20260925/native-fixtures/{red,green-initial,green-focused}.elf`;
each SHA256 is `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.
They are local test fixtures, not release binaries. Existing earlier evidence
and the prior systemd 259 dependency probe were not changed or rerun.

The A profile remains exact standalone monitor-only or ordered monitor+Qt,
with fixed unit/binary pins, empty SCADA runtime map, switched common monitor
target, explicit disabled MQTT/direct maintenance, Bridge fence and all other
approved units stopped. All source/default SHM hashes and unmapped checks
remain in observe. Failed start restores inhibition, reloads, stops both readers
and Bridge, and checks stopped state plus PID 0. A Qt observation requires an
already active graphical target; no graphical session is started by this script.

This is local script behavior only. Actual A effective inbound values are still
UNKNOWN. Implementation must prove both reader executables, effective env,
new V11 target inode mappings, unchanged old source/default inode mappings and
hashes, Bridge not running, other physical/control participants fenced, and
bounded recover on device. Qt has write entrypoints and is not inherently
read-only. This stage does not qualify A dynamic observe or physical control.
