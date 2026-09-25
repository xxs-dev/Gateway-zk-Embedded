# Offline SHM10 -> SHM11 installation

This is a root-operated maintenance entrypoint, not online OTA or a new cluster
protocol. Explicit fixed-voter mode requires all fixed voters to have control
disabled, Gateway participants stopped and restart inhibited. Explicit standalone
mode requires equivalent local evidence and no enabled EMS cluster in any app. The
external SHA256 argument is an out-of-band approval pin, not publisher signing.
Never derive that approval from an untrusted package's own claim.

The current reviewed R5 `program-manifest.json` SHA256 is
`75c7018d4d78d93e67f8239c041b5f1003d460f0266f549e0c25053cc9795ca6`.
It describes source `9ccfce8822a2504524c29d9505179ab9e94aa775`, baseline archive
`0232f029a66b9df5a502ee8d10e3e24eac807edea82ee7346ae1042c8901a2d9` and the R5
delta `2dc8582535ea8358c839eec95ba509df2001915b91989132793164866ca3a652`.
Compose an exclusive payload directory using each product's `archivePath`,
`bytes` and `sha256`. Only `kind=product` entries are installed (20 products,
including the inherited migration CLI); 12 probes and the helper are excluded.
This approval does not authorize an actual device migration.
The full factory builder/installer now require `memory_point_store_migrate`.
Base/project payloads keep their narrower profiles; an offline operator must
provide the complete approved external payload, not assume a base package has
the migration tool. No new ARM compilation is needed for that existing CLI.

## Approval document

`schemaVersion` is `offline-shm11-2`. Schema1 approvals/ready receipts are refused;
there is no implicit mode downgrade. Keep the pinned original script pair with
any existing schema1 transaction for its recovery; do not rewrite its saved
approval/state to schema2. Required fields:

- `mode`: exactly `fixed-voter` or `standalone`; it is never inferred or defaulted.
- `transactionId`: unique 1..63 letters/digits/underscore/hyphen.
- `gatewayHome`: absolute non-symlink existing Gateway directory.
- `nodeId`, `identitySha256`: exact current local identity in both modes.
- Fixed-voter only: `membershipSha256` pins the
  fixed roster (`data/cluster-membership.json`, `assignments[].nodeId/cabinetNo`).
  The runtime schema is `schemaVersion:"1.0"`, nonempty `clusterId`, positive
  uint64 `membershipEpoch`, and 2..5 unique node/cabinet assignments. The complete
  set must match every enabled local emsCluster's clusterId, expectedMembers,
  maxMembers and lockedCabinetNo. The erroneous `members` shape is refused.
  A non-default membershipFile is explicitly unsupported by this narrow entrypoint;
  do not validate one roster while starting against a different file.
- `runtimeCompatibility`: exactly `pointStoreAbi:11`, `clusterProtocol:2`,
  `upgradeMode:"offline-all-participants"`; `controlEnabled:false`.
- `expiresAtUnix`: bounded operator authorization expiry, integer UTC seconds.
- `programManifestSha256`: externally reviewed full component manifest pin.
- `installPaths`: exact target-to-relative-path map for EVERY product in the
  approved manifest. Normal targets permit only `bin/<target>`. The explicit
  `LocalDisplayQtEms` or `KY-EMS` target also permits `bin/KY-EMS` or
  `ky-ems/KY-EMS`. Use `"LocalDisplayQtEms":"ky-ems/KY-EMS"` for the existing
  factory/real Qt layout, retaining the SAME approved component hash and payload
  archive path. No directory relocation or Qt resource replacement occurs.
  Absolute/traversal/noncanonical paths, aliases outside this list, duplicate
  destinations, missing mappings and the obsolete `installNames` field refuse.
- `installedRuntimeSha256`: exact relative-path-to-current-SHA256 map of installed
  runtime files under `bin` and `ky-ems`, including nested ELF files. For example
  `"ky-ems/KY-EMS":"<old SHA256>"`, NOT a basename key. All existing runtime
  paths must be covered by installPaths; missing/extra/hash-mismatched files refuse.
  Symlinks anywhere in these managed trees refuse. Additional ELF resources or
  executable layouts outside the supported mappings require separate review,
  not automatic relocation/deletion. Non-ELF Qt resources remain untouched.
  Inventory is checked before and after stopping, and again before observation.
- `configSha256`: exact map of ALL files relative to `config/runtime`, including
  identity, templates/configuration and credentials (hashes only).
- `units`: exact Gateway-only templates/instances and software launcher/watchdog
  units to inhibit and stop, covering discovered units. No SSH/network service,
  kernel watchdog or arbitrary unit is accepted. Existing enabled/masked policy
  is recorded, never changed by apply/recover.
  `gateway-services.service` remains mandatory. The software
  `gateway-health-watchdog.service` may be omitted only when systemd reports
  exactly `LoadState=not-found`; a discovered, loaded or unknown watchdog must
  be in scope and its stop/active-state failures still refuse the transaction.
  Template files such as `compute-engine@.service` receive inhibition drop-ins
  but are never passed to `stop`/instance `show`; only concrete instances stop.
- Fixed-voter only: `offlineVoters`: exactly every fixed voter, each with `controlDisabled:true`,
  `participantsStopped:true`, `restartInhibited:true`, and `evidenceSha256` of the
  implementation operator's reviewed stop/process/SHM inventory receipt. These
  are externally approved attestations, not remotely verified votes or signatures.
- Standalone only: `offlineLocal` contains the same three true stop/control/fence
  flags and evidence hash, plus `nodeId` matching the bound local identity.
  `membershipSha256` and `offlineVoters` must be absent. Every configuration file
  is inspected: any enabled EMS cluster or ambiguous enabled type refuses this
  mode. Do not substitute a shadow app or synthesize a one-member roster. Existing
  disabled historical roster/consensus files are neither written nor removed.
- `segments`: unique `{source,target,sha256}` records for every required SHM
  reference. The hash describes the stopped source, the target must not exist
  and must differ from every source. The sole reference exception is the direct
  `emsCluster.virtualSharedMemoryName` field when that same object has explicit
  boolean `enabled:false`: the disabled cluster coordinator and the known
  compute/MQTT/authorization consumers do not open it. It need not be listed
  when absent. Its name is still validated. Enabled, missing or mistyped
  `enabled` never gains this exception; another reference to the same name
  still requires a segment. No missing-segment migration or synthetic roster
  is supported. The only extra approved source may be an existing fixed
  `gateway_point_store` with no JSON reference; it receives the same stopped
  digest/quiescence/backup/copy checks, but its implicit readers are unproven,
  so `observe` refuses this transaction. All other extra stores refuse.
- The fixed `gateway_point_store_system_monitor` is a second, separate narrow
  extra source when it actually exists. It must be explicitly listed with a
  stopped SHA256 and distinct V11 target; omitting an existing instance refuses.
  Admission requires standalone mode, both `system-monitor@monitor-service.service`
  and `ky-ems.service` in the stopped unit scope, both SystemMonitor and
  LocalDisplayQtEms in the approved product set, the real `ky-ems/KY-EMS`
  destination, and no existing `998-shm.conf`/monitor binding pin. The old
  segment stays intact and is backed up/copied by the same migration loop.
  Without `qtDisplayEnvSha256`, `observe` still refuses this stop-only
  transaction. Recovery retains the original absence of a monitor drop-in.
  Other extra segment names remain forbidden.
- Optional A-only `qtDisplayEnvSha256` pins the original
  `config/runtime/qt-display.env` bytes, with the monitor SHM key absent.
  Admission also requires the pinned Qt unit, wrapper, monitor template and
  ABI11 Qt/SystemMonitor candidates, plus an empty `scada/current` runtime map.
  While all units are stopped, the transaction appends only
  `GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=<approved monitor target>` to that
  env file and creates the fixed monitor `998-shm.conf` with the same target.
  Both changes are staged and backed up in the existing state; stopped recovery
  restores the original env bytes/mode and removes the newly created drop-in.
  This does not alter the SCADA project, old SHM, dedup, or unit policy.
- For the pinned A joint profile only, when the original
  `apps/monitor-service.json` explicitly has both `mqtt.enabled:true` and
  `systemMonitor.directMaintenance.enabled:true`, approval must also contain
  `aInboundDisableSourceSha256` equal to that original app file's
  `configSha256` entry. Apply switches exactly those booleans to `false` in
  the existing staged config update; observe still requires both effectively
  false. Recovery restores the original file bytes and mode. Missing/wrong
  approval, mixed/null/legacy values, or this pin outside A joint binding
  refuse before fencing. This is an inbound maintenance shutdown, not a
  physical control approval.
- Optional `systemMonitorShmDropinSha256`: original-byte SHA256 for only
  `/etc/systemd/system/system-monitor@monitor-service.service.d/998-shm.conf`
  when that exact instance is in `units`. The file must consist solely of
  `[Service]` and one `Environment=GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=<source>`
  line, with `<source>` in approved `segments`. No other environment or drop-in
  format is accepted. The script rechecks original bytes/mode after stop,
  durably backs them up, switches that one value to the approved target after
  SHM copy, and restores original bytes/mode on stopped recovery. Omit this pin
  only when no SystemMonitor observer will start.

The entrypoint handles `sharedMemoryName`, `sharedMemoryNames`,
`virtualSharedMemoryName`, `outputSharedMemoryName`, and
`outputDefaultSharedMemoryName`. Guard extraction and rewriting share one key
set, regression-checked against the runtime config and SCADA loader literals.
Wrong reference types, unknown SHM-name keys and remaining exact old-name values
in unsupported fields are refused, never rewritten as arbitrary JSON strings.
Existing emsCluster
configuration must explicitly have `controlEnabled:false`; this tool does not
silently edit authority or identity settings to make validation pass.
An empty `cameraService.sharedMemoryName` remains empty during config parsing;
the camera service may select its default store only after its enabled/camera-list
checks. A nonempty camera reference is still collected even when its camera
service is disabled. No camera exception is implied by absent active units.

## Apply and recovery

Run only after approval, from the reviewed paired deploy directory:

```sh
python3 offline-runtime-upgrade.py apply \
  --approval /persistent/offline/node-approval.json \
  --approval-sha256 APPROVED_SHA256 \
  --manifest /persistent/offline/program-manifest.json \
  --payload /persistent/offline/payload \
  --state /persistent/offline/unique-transaction

python3 offline-runtime-upgrade.py recover \
  --approval-sha256 ORIGINAL_APPROVED_SHA256 \
  --state /persistent/offline/unique-transaction
```

The state directory must be new and on ext-family/XFS/Btrfs, outside Gateway
home. Root must be in the host PID/mount namespaces with readable `/proc`.
Concurrent privileged writers are excluded during the operator's maintenance
window. The tool serializes its own operations; it is not a hostile-root sandbox.

Apply creates persistent `data/runtime-upgrade-stop` and exact-unit systemd
condition drop-ins, reloads systemd, stops all listed participants and verifies
their inactive state and absence of runtime executables. It snapshots old/new
files durably before running the approved CLI from its private staged copy.
The CLI rejects live mappings, pending commands and active owner/claim leases,
copies V10 to a distinct V11 segment and writes exclusive durable backups.
Only after all copies succeed are binaries and every configured SHM reference
switched. Sources remain byte-identical. Latest/history/completed results copy;
owners, claims, pending writes and authority do not.

Success is `UPGRADED_STOPPED`, not running/physical acceptance. Original startup
and OTA are blocked by the persistent fence. Ordinary config/SCADA operations
outside an offline transaction retain their existing paths.

Recover validates all old file hashes, stops participants and restores only
program/config bytes and the explicitly pinned SystemMonitor SHM drop-in, if
present. Newly introduced files are retained outside live `bin`.
It never restores data, identity, dedup, membership, consensus or ANY SHM image.
It returns `RECOVERED_STOPPED`; old ABI control MUST NOT restart. Pending writes
or authority in old SHM are not reauthorized. No automatic ABI downgrade is
implemented or claimed. Failed backup validation keeps the installation fenced.
If stopping fails, `FAILED_STOP_UNCONFIRMED` explicitly requires operator action;
it is not proof that the old running process has ceased physical control.

## Observe without physical outputs

After ALL fixed voters (or the standalone local participant) have completed their
matching upgrade, the operator may separately approve an `offline-shm11-observe-2`
receipt. Its required `mode` must equal the original approval. It contains
`transactionId`, original `approvalSha256`, `programManifestSha256`, the exact
local completed `stateSha256`, `controlEnabled:false`, integer `expiresAtUnix`,
`startUnits`, and, in fixed-voter mode, `voters` keyed by the full fixed voter set. Each voter receipt
contains `phase:"UPGRADED_STOPPED"`, `controlEnabled:false`, the same program
manifest hash and its reviewed completed state hash (which binds its own config
and source/target SHM hashes). The local voter hash must match `stateSha256`.
Standalone instead requires `local` with the same phase/control/manifest/state
fields plus matching `nodeId`, and forbids `voters`. Its local state hash must
match `stateSha256`. Fixed-voter receipts must not contain `local`.

```sh
python3 offline-runtime-upgrade.py observe \
  --approval-sha256 ORIGINAL_APPROVED_SHA256 \
  --state /persistent/offline/unique-transaction \
  --ready /persistent/offline/all-voters-ready.json \
  --ready-sha256 SEPARATELY_APPROVED_SHA256
```

Only explicitly selected `compute-engine@`, `ems-cluster@`, `system-monitor@`
instances may start in fixed-voter mode.
Standalone further excludes `ems-cluster@`: only Compute/Monitor instances may
start, even if a disabled cluster unit exists in the stopped unit inventory.
Rechecks cover all installed product hashes, config bytes,
source/target bytes, absent live mappings/processes and unchanged unit policy.
The only supported SystemMonitor observer is `system-monitor@monitor-service.service`:
it requires the pinned drop-in state, unchanged switched file and exact effective
systemd `Environment` target, with no `EnvironmentFiles` or `UnsetEnvironment`.
After start, the approved old monitor SHM must still be unmapped; a failure
stops the observer through the existing failed-start path.
Without that binding it refuses before start. An unreferenced existing default
`gateway_point_store` still refuses observe except for the narrow standalone
SystemMonitor profile: exactly the approved `monitor-service` instance, the
pinned R3 SystemMonitor binary and unit template, and exactly the three
camera/monitor/mqtt app files with empty `deviceConfigFiles`, one nonempty
approved target store, and disabled sibling SHM consumers/producers. The
approved monitor source-to-target drop-in must switch the primary store.
The effective instance `DropInPaths` may contain only the pinned
`998-shm.conf` and this transaction's instance `90-offline` inhibition file.
Systemd overrides the template's same-named `90-offline` in the effective
path list; both physical inhibition files are nevertheless byte-checked.
Template, drop-in bytes/mode, effective environment, and configuration
are checked again before start. Every source, including the old default, must
be unmapped before and after start, and source hashes must remain unchanged.
This is a config-shape/version qualification, not a node whitelist or an
approval flag. Other observers, unknown apps, changed unit bindings, and
other default-store cases still refuse. Local fake-systemctl tests cannot
establish real process target mappings or production service safety; those
remain separate on-device observation requirements.
The A joint-binding profile admits a standalone monitor-only observation or
the exact ordered `[system-monitor@monitor-service.service, ky-ems.service]`
observation
with the approved default and implicit monitor segments. It rechecks the actual
three-app/one-device loader shape including disabled sibling camera, the empty
SCADA map, both switched bindings,
the pinned Qt/monitor/bridge units, and the bridge's retained inhibition and
inactive PID 0 state. Qt selection additionally requires an already-active
`graphical.target`; observe never starts/stops the graphical target or Bridge.
The monitor app must explicitly have `mqtt.enabled:false`
and `systemMonitor.directMaintenance.enabled:false`; the general
`controlEnabled:false` approval does not disable either inbound maintenance
path. After start, every old source (including default) remains hashed and
unmapped. After both readers start, the A loader/unit/environment/empty-map
profile is rechecked, all other approved units must remain stopped, and the
Bridge must remain inhibited, inactive with PID 0 and `ConditionResult=no`.
Failed Qt startup restores inhibition and checks both readers and Bridge stopped
with PID 0. Qt is write-capable; this is not a general read-only qualification.
Actual reader executable/environment/V11 inode mappings and inbound effective
values still need bounded on-device verification; no Bridge is started.
Previously masked units are refused, never unmasked. `is-active` must succeed.
The entrypoint temporarily removes only selected observer/observer-template
drop-ins, starts them, then restores inhibition and reloads systemd before success.
It never enables a unit or lifts the persistent global fence. Physical drivers,
Gateway launcher and other participants remain inhibited. A completed observation
start does not permit an automatic subsequent restart. Interrupted startup requires
operator inspection/recovery; SIGKILL/power-loss mid-operation is not qualified.

Success is `OBSERVING_CONTROL_DISABLED`; it is not normal production control.
Any start/check/reinhibition failure triggers best-effort observer stop and retains
failure state. Replay of an already-consumed state receipt is refused. `recover`
can subsequently stop those observers and restore files while remaining fenced.
Starting physical participants or enabling control remains a separate release
decision and is intentionally not an option on this entrypoint.

## Current qualification boundary

Local tests cover actual fixed-voter R4/S2 full app/roster inputs, the real
factory Qt relative path, and complete repository noncluster app examples.
The latter are representative configs, NOT the live A/B apps. M1 retrieved only
partial camera/monitor projections on A before refusing; B, real unit ExecStart
and SHM headers are not qualified by that partial receipt. Its empty camera SHM
reference does not establish whether an implicit reader is active. The operator
must obtain complete actual config/runtime/unit/SHM scope before device approval.
No device migration, normal acquisition restart, physical output, ARM systemd
qualification or power-loss recovery has been authorized by these local tests.

Evidence tests use actual entrypoint and native migration CLI in private
mount/net/IPC namespaces, with a fake systemctl. They are not ARM/systemd device
acceptance. A real device migration/start requires separate implementation review.
