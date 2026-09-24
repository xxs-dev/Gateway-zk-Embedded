# Offline SHM10 -> SHM11 installation

This is a root-operated maintenance entrypoint, not online OTA or a new cluster
protocol. All fixed voters must have control disabled, their Gateway participants
stopped and restart inhibited before the operator approves a transaction. The
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

`schemaVersion` is `offline-shm11-1`. Required fields:

- `transactionId`: unique 1..63 letters/digits/underscore/hyphen.
- `gatewayHome`: absolute non-symlink existing Gateway directory.
- `nodeId`, `identitySha256`, `membershipSha256`: exact current local identity and
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
- `installedRuntimeSha256`: exact map of every installed runtime ELF/name to its
  current SHA256. An installed runtime absent from the target manifest is refused.
- Optional `installNames:{"LocalDisplayQtEms":"KY-EMS"}` selects the existing Qt
  installed name while keeping the SAME approved component SHA256 and archive
  path. No arbitrary alias or symlink is accepted. Without this mapping, an old
  `KY-EMS` installation is refused rather than leaving its launch path unchanged.
- `configSha256`: exact map of ALL files relative to `config/runtime`, including
  identity, templates/configuration and credentials (hashes only).
- `units`: exact Gateway-only templates/instances and software launcher/watchdog
  units to inhibit and stop, covering discovered units. No SSH/network service,
  kernel watchdog or arbitrary unit is accepted. Existing enabled/masked policy
  is recorded, never changed by apply/recover.
  Template files such as `compute-engine@.service` receive inhibition drop-ins
  but are never passed to `stop`/instance `show`; only concrete instances stop.
- `offlineVoters`: exactly every fixed voter, each with `controlDisabled:true`,
  `participantsStopped:true`, `restartInhibited:true`, and `evidenceSha256` of the
  implementation operator's reviewed stop/process/SHM inventory receipt. These
  are externally approved attestations, not remotely verified votes or signatures.
- `segments`: unique `{source,target,sha256}` records for ALL configured SHM
  names. The hash describes the stopped source, the target must not exist and
  must differ from every source. All SHM references must be explicit; custom
  readers/implicit defaults must be removed or provisioned explicitly first.

The entrypoint handles `sharedMemoryName`, `sharedMemoryNames`,
`virtualSharedMemoryName`, `outputSharedMemoryName`, and
`outputDefaultSharedMemoryName`. Guard extraction and rewriting share one key
set, regression-checked against the runtime config and SCADA loader literals.
Wrong reference types, unknown SHM-name keys and remaining exact old-name values
in unsupported fields are refused, never rewritten as arbitrary JSON strings.
Existing emsCluster
configuration must explicitly have `controlEnabled:false`; this tool does not
silently edit authority or identity settings to make validation pass.

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
program/config bytes. Newly introduced files are retained outside live `bin`.
It never restores data, identity, dedup, membership, consensus or ANY SHM image.
It returns `RECOVERED_STOPPED`; old ABI control MUST NOT restart. Pending writes
or authority in old SHM are not reauthorized. No automatic ABI downgrade is
implemented or claimed. Failed backup validation keeps the installation fenced.
If stopping fails, `FAILED_STOP_UNCONFIRMED` explicitly requires operator action;
it is not proof that the old running process has ceased physical control.

## Observe without physical outputs

After ALL fixed voters have completed their matching upgrade, the implementation
operator may separately approve an `offline-shm11-observe-1` receipt. It contains
`transactionId`, original `approvalSha256`, `programManifestSha256`, the exact
local completed `stateSha256`, `controlEnabled:false`, integer `expiresAtUnix`,
`startUnits`, and `voters` keyed by the full fixed voter set. Each voter receipt
contains `phase:"UPGRADED_STOPPED"`, `controlEnabled:false`, the same program
manifest hash and its reviewed completed state hash (which binds its own config
and source/target SHM hashes). The local voter hash must match `stateSha256`.

```sh
python3 offline-runtime-upgrade.py observe \
  --approval-sha256 ORIGINAL_APPROVED_SHA256 \
  --state /persistent/offline/unique-transaction \
  --ready /persistent/offline/all-voters-ready.json \
  --ready-sha256 SEPARATELY_APPROVED_SHA256
```

Only explicitly selected `compute-engine@`, `ems-cluster@`, `system-monitor@`
instances may start. Rechecks cover all installed product hashes, config bytes,
source/target bytes, absent live mappings/processes and unchanged unit policy.
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

Evidence tests use actual entrypoint and native migration CLI in private
mount/net/IPC namespaces, with a fake systemctl. They are not ARM/systemd device
acceptance. A real device migration/start requires separate implementation review.
