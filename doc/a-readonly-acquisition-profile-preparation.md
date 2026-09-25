# A RTU readonly + MQTT TX/full: local preparation only

Base: d5884517586c17ce40a50717d1157534da69a61d. Actual A adaptation pending.
This change adds one offline configuration predicate and synthetic tests only.
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

The predicate requires explicit booleans and the conservative initial shape: RTU only,
no northbound/server alias, no startupWrites, write.enable=false for every point,
read function 1..4, no non-null initialValue/startupValue or retain=true, exact app/device
inventory, disabled extra producers/control/OTA/maintenance, all eleven MQTT RX topics
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
Explicit fullUploadIndexes must cover collected indexes: mqtt_driver_service.cpp:751-759
shows per-point flags may override publishAllOnFull. Inline worker is a proposed narrow
constraint, not a claim about the current device; isolated mode needs separate assessment.

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

## Missing actual A input: combine into the next single preflight

No new device intake is requested by this change. Implementation owns the next authorized preflight.

- Capture the three-app/one-device inventory, full-file SHA256 and relevant field projection
  in that same preflight, tied to machineCode/boot ID/current transaction and capture time.
  Existing hashes alone do not establish field values; absent/null/type must remain distinguishable.
- Device: protocol.type/backend/transport/standardPointsFile; memoryStore; northboundServer
  and overriding server; startupWrites; top points and every meter's enabled/points/groups.
  Each point: index, enabled, read, write, initialValue, retain, fullUpload, northbound/forward;
  preserve all pointGroups/iecPointGroups/fourRemote/fourRemotePoints/pointsByType/pointsByCategory.
- Each app: deviceConfigFiles, MQTT enabled and the eleven request/ack topic fields,
  mqttDriver enabled/store names/full interval/full indexes/publishAllOnFull/fullUploadWorker,
  ota, emsCluster/controlEnabled, computeEngine, agcAvc presence, cameraService,
  systemMonitor/directMaintenance, localDisplay/SCADA/store references.
- Full-upload topic, worker mode/event settings and configured point flags are still unknown.
  Never substitute test defaults for those actual values. No broker passwords or TLS private keys.
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
23 tests PASS, including subcases for each RX topic and each top/meter group alias.
Synthetic config only; no C++ build, device connection, live service cycle or deployment.
Coverage includes explicit empty vs missing/null topics, disabled writable points/meters,
initial zero/startup zero, retention, extra configs, northbound aliases, isolated worker,
upload coverage, malformed/duplicate indexes and JSON keys, external evidence hash mismatch.
Actual A compatibility, unit gating integration and runtime acceptance remain pending.
Two added RED/GREEN groups reproduce the memory enabled/type checks and reader store sources,
including the exact disabled Monitor display + unapproved store case. They exercise loader
defaults, enabled and disabled display lists in every app, existing MQTT/device bounds, and
the refusal to turn a JSON Monitor name into effective-environment qualification.
