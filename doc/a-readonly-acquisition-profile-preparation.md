# A RTU readonly + MQTT TX/full: local preparation only

Runtime base: d5884517586c17ce40a50717d1157534da69a61d.
One offline configuration predicate, synthetic tests and actual redacted-projection tests.
It changes no runtime, activation guard, launcher, systemd unit, device or production branch.
No compilation or connection is needed. Existing activation gates remain closed to RTU/MQTT.

## Narrow scope

The eventual scope is the existing modbus-rtu@device_modbusRTU_2_readonly.service,
mqtt-driver@mqtt-service.service, system-monitor@monitor-service.service,
ky-ems.service and gateway-services.service launcher. Qt bridge dependency handling must
remain explicitly gated. Compute, AGC/AVC, physical controls and other business services
are not included. TX/full acquisition is NOT realtime-session or full-business acceptance.

tools/a_readonly_profile_check.py consumes a SHA256-pinned, locally prepared JSON envelope:
`documents` maps exactly three apps and one RTU device relative paths to their JSON objects;
`targetSharedMemoryNames` is the reviewed target-name list. The envelope is evidence tooling,
not a new runtime configuration schema. Credential fields are unnecessary; do not export secrets.
Supply complete safety-relevant fields, not a claim that a filename is readonly.

With `--redacted`, documents instead contain the preflight entries with `projection.document`,
`projection.fieldMetadata` and `projectionSha256`. Each projection hash is checked over
sorted compact JSON plus LF. The outer envelope still requires its independent SHA256.
The function `check_projected_profile` only copies/normalizes evidence in memory; it never
patches a config or imports unit admission. CLI exit codes: 0 config PASS, 1 refusal/error,
2 PENDING. Both PASS and PENDING keep activation/actual-A qualification false.
Only proven missing fields receive the cited source defaults. Present-but-omitted values
in unknownKeys remain PENDING; missing evidence is not absence. Non-schema worker/legacy
keys use the complete original-key inventory. Invalid types/conflicting metadata are refused.
The checker is deliberately narrower than ConfigLoader, not a general schema implementation.

The predicate requires explicit booleans and the conservative initial shape: RTU only,
no northbound/server alias, no startupWrites, write.enable=false for every point,
read function 1..4, no non-null initialValue/startupValue or retain=true, exact app/device
inventory, disabled extra producers/control/OTA/maintenance, all eleven running MQTT app RX topics
explicitly empty, positive full interval and inline worker without event delegation.
It inspects top-level and meter points including disabled ones and all six loader group aliases.
Device memoryStore.enabled must resolve to true. Missing/null enabled defaults to true;
missing/null backend defaults to "memory" (models.hpp:603-610, config_loader.cpp:1362-1373).
Backend must parse as a string, with no invented enum restriction. main.cpp:163 constructs
MemoryPointStore unconditionally; its config constructor maps SHM without using enabled/backend.
Thus enabled=false is rejected by our narrow profile policy, NOT because source proves it
disables collection, and backend="memory" does NOT imply private heap storage. Any other backend
string is likewise not an implementation selector in this fixed path. No production bug is claimed.

All three apps' localDisplay.sharedMemoryNames are checked against the approved targets,
including when display.enabled=false. Missing/null localDisplay or missing/null list means []
(models.hpp:1455-1462, config_loader.cpp:3619-3635); empty string entries are skipped by readers.
Together with checked device and MQTT primary/list names, these cover the JSON store sources
that remain reachable here. Qt merges display/MQTT/device names unconditionally (:231-256).
Monitor merges enabled sibling display lists and primary/MQTT/device lists; enabled Compute,
camera, AGC and EMS cluster routes remain excluded by the existing narrow checks.
Monitor and Qt additionally call system_monitor_points::sharedMemoryName(), which resolves
GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME from the process environment, then its built-in default.
JSON systemMonitor fields are NOT an override. monitorEnvironmentQualified stays false;
effective unit/environment proof for each affected process remains a separate pending condition.
The disabled camera sibling's default mqttDriver store is not opened by Monitor discovery
(system_monitor_runtime_discovery.cpp:70-88). Camera MQTT may remain enabled in its config;
starting camera remains outside scope. Monitor MQTT and directMaintenance must be disabled;
its old RX strings then do not require rewriting. MQTT app RX strings must all be explicit empty.
MQTT main adds device stores (mqtt_driver_main.cpp:350-364); an RTU name absent from the
app's list is not a missing-read defect. Explicit indexes union with configured/router fullUpload
flags (mqtt_driver_service.cpp:708-767). With flags, publishAllOnFull is overridden; with no
effective selection, all router indexes are used. Validate effective coverage, not explicit-list
coverage. The router includes top-level routes even with meters; RTU collection uses the
effective meter owners when meters exist. Worker numeric bounds apply even in inline mode.

Even success returns activationAuthorized=false, actualAQualified=false and
realtimeSessionQualified=false. It does not replace ConfigLoader validity checks, verify
live inventory, inspect unit overrides, authenticate provenance, or authorize execution.
Full service safety remains pending the existing activation/unit/SHM gates and acceptance.

## Implicit route initialization

Both Monitor and Qt install device routes, as does MQTT. At fixed source,
PointStoreRouter::addRoute (src/point_store_router.cpp:1189) and addStore (:432)
call restoreEmsVirtualParameter (:1215-1291). It can putLatest for non-derived
ems_virtual routes with retention/direct-parameter/initial/startup settings when current
data is missing, invalid or stale. Read-only UI tags alone do not prevent this startup write.
This predicate excludes virtual protocol and any extra device/sibling AGC config, and
also conservatively rejects initial/retain/startup settings on the physical points.
Normal RTU acquisition still writes new measured values/TTL into SHM; that is expected.
No queues, authority records, dedup data or retained parameter files are cleared by this tool.

## Actual A evidence and remaining gates

Two fixtures retain exact redacted entries/public metadata, not complete configs or unit evidence:

- tools/fixtures/a-profile-preflight-20260925-01.json: source report SHA256
  8009d45e417946fa12fb7bcc7bba7a512b8a7ede1848ca2bbb7ef0866f13fefc.
  All four original projection hashes recompute exactly, including JSON numeric types.
- tools/fixtures/a-unit-env-public-supplement-20260925-01.json: source report SHA256
  81a6ebaa035cfd6a60ba139044474404bd37497391f4088be5b3f575cce56928;
  public projection SHA256 8b53c6864b7a443636830368a83d32583e50bbacd03b1d1bc02869abfb8402eb.
  Prior-report pin and all four full-file hashes/byte counts match the first fixture.

Tests modify only in-memory Monitor mqtt.enabled/directMaintenance.enabled to false and the
MQTT app's eleven RX topics to empty strings, plus consistency metadata/hashes for those
simulated changes. Original full-file pins remain source pins, NOT candidate full-file hashes.
Five observed source store names are diagnostic test bounds, NOT approved live remap targets.
Camera document, physical points and MQTT driver settings are unchanged. No candidate runtime
JSON is emitted or installed. Tests also supply deliberately synthetic values for negative cases.

The original configuration is refused. Without the supplement the minimal candidate is PENDING
at exactly these paths (all present-but-omitted, never populated from defaults):

- apps/mqtt-service.json#/mqtt/telemetryTopic
- apps/mqtt-service.json#/mqttDriver/fullUploadWorker/healthHeartbeatMs
- apps/mqtt-service.json#/mqttDriver/fullUploadWorker/failoverTimeoutMs
- apps/mqtt-service.json#/mqttDriver/fullUploadWorker/retryMinMs
- apps/mqtt-service.json#/mqttDriver/fullUploadWorker/retryMaxMs

The supplement supplies telemetryTopic=edge/telemetry and timing values 1000/3000/500/5000;
realtimeTelemetryTopic is proven absent. Full topic is edge/telemetry/full, derived using
config_loader.cpp:229-238,1457-1466. No realtime RX fallback is needed: candidate RX is explicitly
empty. Config-only candidate PASS covers 151 indexes 4500..4650 via their original fullUpload
flags, with the original 5000ms interval and inline worker. No public config gap remains for this
narrow predicate. It is not proof of fresh physical samples or broker delivery.

Reuse previously verified TX02 SCADA/remap scope and matching full-file pins; no new request
for those inputs. Unit/env gaps are separate, never turned into config defaults. The supplemental
report has diagnosticComplete=true but fieldsComplete=false. RTU/MQTT each still lack four
Exec fields and EnvironmentFiles, and Monitor lacks EnvironmentFiles (GetUnit failure).
Existing activation checks, complete config/remap binding and runtime acceptance remain pending:

- Effective RTU/MQTT unit fragments and hashes plus instance AND template drop-ins,
  ExecStart/Pre/Post, working directory, environment/files/unset/pass environment,
  Requires/Wants/BindsTo/Triggers/TriggeredBy and already-established Monitor/Qt/launcher pins.
  RTU implicitly loads config/runtime/apps/mqtt-service.json (main.cpp:63,76), despite
  its unit supplying only --config; effective WorkingDirectory and overrides are essential.
- Reuse stopped-state identities/participant checks and recovery lineage; read-only config
  checks do not prove absence of old queues or writers. Do not add device connections for this list.

Known historical source config pins (not this predicate's successful input):
device 75e3740da48961bf928b0cdeeb9815ff6919f1e233ef8d0126679a2750abbdf0;
MQTT a656fe30d5a7220cad5efcde04f40b4211393fb56bfa867eaf3f9b097d8f542b;
Monitor 6a64c844c339a826986bfeea7de2c7af85526a439ae9d36c8fe1f1d2bcf1ba0b.
Projected/redacted bytes have a separate hash; never present that as the full-file hash.

## Local verification

`python -B -m unittest discover -s tools -p a_readonly_profile_check_test.py -v`
37 tests PASS, including subcases for each RX topic and each top/meter group alias.
Synthetic configs and redacted actual evidence only; no build, device connection or deployment.
Coverage includes explicit empty vs missing/null topics, disabled writable points/meters,
initial zero/startup zero, retention, extra configs, northbound aliases, isolated worker,
upload coverage, malformed/duplicate indexes and JSON keys, external evidence hash mismatch.
Actual A activation, unit gating integration and runtime acceptance remain pending.
Two added RED/GREEN groups reproduce the memory enabled/type checks and reader store sources,
including the exact disabled Monitor display + unapproved store case. They exercise loader
defaults, enabled and disabled display lists in every app, existing MQTT/device bounds, and
the refusal to turn a JSON Monitor name into effective-environment qualification.
New projection cases cover exact hashes, source-proven defaults, withheld values and missing
metadata, legacy null-key presence, worker inline bounds, derived-topic types, partial flag
coverage, unchanged 151-point/5-second input, explicit-empty RX, CLI PENDING exit code and the
actual public supplement. No unrelated suites were rerun.

## Next minimal integration, NOT implemented or authorized here

1. deploy/offline-runtime-upgrade.py: extend existing a_joint_binding/apply and
   a_joint_config_safe with one explicitly approved A acquisition profile. Bind the MQTT
   eleven-topic change to its original full-file SHA, record new SHA and backup through the
   existing changed/state.files transaction. Reuse Monitor inbound disable, TX02 SCADA and
   source-to-target remap. Preserve broker/auth/TLS, 5000ms full upload and 151 point flags.
   No direct mutation of old committed state; apply remains stopped and fenced.
2. Same file, activate: admit exactly the RTU instance, MQTT instance, Monitor, KY-EMS and
   launcher for this profile, preserving a_joint_monitor_observe_safe, separate ready approval,
   local identity, stopped-state/SHM checks and graphical-target/bridge/SCADA checks. Config
   predicate PASS alone cannot produce ready. Reuse existing concrete checks, not a new
   general profile framework. Effective WorkingDirectory matters: RTU loads the MQTT app
   from a relative path even though its ExecStart only supplies the device config.
3. deploy/runtime-upgrade-guard.py: extend active_context and activated_unit for only that
   fixed set, with pinned RTU/MQTT binaries, template fragments, instance AND template drop-ins,
   effective commands/environment and dependency edges. Retain both A-specific Qt/Monitor
   branches currently gated by A_MONITOR_QT. Preserve old-source mapping checks and deny
   every unrelated unit; do not make a permissive generic unit allowlist.
4. deploy/offline-runtime-upgrade.py: activation_units, refence_activation and recover must
   use the identical profile set. A start/stop/restart failure or interruption must fence all
   participants, verify stopped processes/mappings and retain failed-stop evidence. Existing
   state.files backups restore MQTT RX along with Monitor/remap. gateway-services.sh already
   uses activated-list; no launcher rewrite is currently indicated.
5. Focused local tests only after authorization: correct exact group, absent/PENDING effective
   fields refused, foreign templates/drop-ins/dependencies, wrong config/approval hashes,
   MQTT patch backup/recovery, preserved A Qt/SCADA checks, interruption and refence scope.
   Any authorized actual run should combine RTU read + MQTT TX/full + one service cycle;
   no new three-point rendering run and no B-device run.

Still missing in a-unit-env-preflight-20260925-01/report.json (not config defaults):

- effectiveUnits[modbus-rtu@device_modbusRTU_2_readonly.service]: EnvironmentFiles,
  ExecStartPre, ExecStartPost, ExecStop, ExecStopPost.
- effectiveUnits[mqtt-driver@mqtt-service.service]: the same five fields.
- Monitor effective EnvironmentFiles binding.

GetUnit failure is not typed empty evidence. These fields need actual typed values or
independently pinned typed-empty proof tied to the same effective units/transaction. Other
already obtained unit and environment facts remain reusable; do not request SCADA pins again.
This list neither authorizes another device connection nor changes any production admission.
