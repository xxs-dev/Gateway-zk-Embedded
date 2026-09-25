# Fixed A acquisition activation profile: local implementation only

Runtime base d5884517586c17ce40a50717d1157534da69a61d; preparation 56dea83.
This is the coordinator-authorized isolated implementation, not device authorization.
No stable merge, push, C++/ARM/Windows build, device connection or deployment occurred.

## Exact scope and approval

Existing offline approval must explicitly include `aReadonlyAcquisition: true` as a JSON boolean.
False, 0/1, string, null or other types are rejected; absence retains the original A/B behavior.
It requires standalone/control-disabled mode, the existing A joint Monitor/Qt and TX02 readonly
SCADA approvals, paired startup-script pins and the four fixed full source config SHA256 values
in runtime-upgrade-guard.py:A_SOURCE_CONFIGS. No preparation-checker dependency or second loader
is installed. This does not create camera sections, upload-index lists or new device configs.

The separate ready document uses the existing schema and hashes, with this exact startUnits order:

1. modbus-rtu@device_modbusRTU_2_readonly.service
2. mqtt-driver@mqtt-service.service
3. system-monitor@monitor-service.service
4. ky-ems.service

The launcher gateway-services.service is appended by existing activation flow. Bridge remains
fenced; no camera, compute, AGC, cluster, extra driver, watchdog or B-device is admitted by this
profile. Both RTU/MQTT templates must be in the fenced approval inventory. A new-profile approval
cannot fall back to observer-only A/B or use observe to bypass the combined admission.

## Config transaction

apply verifies the four exact original full-file hashes and uses existing changed/state.files
staging, backups, remap, Monitor inbound-disable and SCADA autoReload=false handling. The only
additional config change is eleven explicit empty MQTT RX topics in apps/mqtt-service.json.
The physical 151 points, fullUpload flags, 5000ms interval, inline worker, topic/auth/TLS and
other values are preserved. An unchanged disabled camera file need not have a transaction row.

Before activation and on every guard entry, original backups must still match the fixed pins.
The guard reconstructs the expected original + existing remap/Monitor patch + MQTT RX patch and
compares it to the installed config and transaction hashes. Arbitrary extra config mutations are
refused even if state hashes were rewritten. ModbusRtu/MqttDriver paths and installed binary
hashes must match the separately pinned program manifest; no rebuild is implicit.

The configured full-topic base is edge/telemetry/full. Implementation confirmed that the default
machine-scoped publisher makes the expected wire topic edge/telemetry/full/COMM202600999.
Configuration proof is not proof of broker delivery. Existing passive single-identity observation
will be reused in any later authorized combined run; no MQTT request or control publish is needed.

## Unit proof and recovery

The new-profile-only helper checks five fixed templates/fragments, exact instance and template
drop-ins (including shadowed files), expected dependency sets, loaded state, NeedDaemonReload=no,
effective command prefixes, working directory, environment and environment precedence.
No general systemd parser or arbitrary unit/profile framework is added.

Empty complex fields omitted by systemctl are accepted only after matching fixed files and full
drop-in coverage with loaded/no-reload state. Evidence is recorded separately as
state.aReadonlyUnitProof.source=STATIC_PINNED_FILES with derivedEmptyFields; it never edits the
original WITH_GAPS report or claims a new typed-empty observation. Missing mandatory fields or
missing nonempty ExecStartPre/Qt EnvironmentFiles/launcher ExecStop are refused. Monitor's
EnvironmentFiles gap is checked against its then-current template/drop-in/environment in that
same combined pre-start path, not through a new independent device query.

All existing ready/identity/stopped/SHM/Qt/bridge/SCADA gates remain. activate, active_context,
activation_units, refence_activation and recover agree on the same set. Failure or interruption
stops and refences RTU/MQTT and all other participants. An unconfirmed stop keeps
FAILED_STOP_UNCONFIRMED. Recovery validates all backup bytes before restoring any config.

## Local verification and limitations

- tools/a_acquisition_activation_test.py: 17 PASS, 2.290s on WSL Linux. Real temporary file
  transactions, simulated systemctl/migration, synthetic full config and binary pins. Existing
  SCADA package validator is mocked here, but calls are retained; its real behavior is exercised
  by existing acceptance work, not newly qualified by this suite.
- Existing install_isolation_test.py: only test_offline_b_activate_recover_fixed_readers and
  test_offline_a_activation_joint_binding, 2 PASS, 3.908s in private mount/network/IPC namespaces.
  Reused pre-existing native CLI/fixture; no compile log was created and no compiler was invoked.
- Boundaries include exact byte recovery, original full settings/auth preservation, both driver
  start failures, interrupted second start, failed stop, corrupt backup, same-boot guarded service
  cycle, strict opt-in, fixed source pins, altered ready group, expanded config patch, unknown
  dependencies/env/drop-ins, stale loaded state, late drift, JSON bool/integer distinction
  and missing nonempty prestart. The final type-comparison correction also passed its
  focused regression before the final 17-case run.

No actual A unit state, physical RTU reads, sample freshness, MQTT delivery, service restart or
device recovery was tested in this phase. Those remain for one separately authorized combined
RTU-read + MQTT-TX/full + service-cycle run. No three-point rendering or B run is requested.
