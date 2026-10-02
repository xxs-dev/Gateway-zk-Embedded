# Configured Business Production Lifecycle: Review Plan

Status: SOURCE_ONLY_PLAN, LOCAL_ONLY. No build, device action, OTA job, or
production approval. Source inspected at edge `5d73c1f`; this plan does not
change the installed 23/24 profiles or authorize a participant.

## Current constraint and decision

`deploy/runtime-upgrade-guard.py` inventories many production units but admits
only fixed RTU/MQTT/Monitor readers plus launcher/watchdog. In
`deploy/offline-runtime-upgrade.py`, adoption binds the prior legacy allowed
units, while both adoption and install require identical old/new config hashes,
unit lists, gates, SHM and trust. `start`, `health`, boot continuation and OTA
recovery follow the same three-reader policy. Installed files are not evidence
that the corresponding services may run. Removing just the schema restriction
would leave start, health and recovery inconsistent.

Implement one reviewed, versioned configured-role policy in the existing
baseline/approval and journal path. Preserve validation of historical v1
records under their original rules; do not reinterpret or rewrite old active
records. A candidate's admitted roles are the exact intersection of (a) a
reviewed role catalog, (b) strictly parsed and pinned candidate configuration,
(c) approved simulator/hardware capability for that board, and (d) pinned
effective systemd units and dependencies. Unknown instances, protocol types,
executables, dependencies, configuration keys that influence admission, and
implicit SHM names fail closed. The broad inventory of present units remains
separate from the approved start set. No arbitrary `systemctl` unit names.

## Role and configuration contract

1. Define one exact unit-family catalog for Modbus RTU/TCP, DLT645, DIO, CAN,
   IEC, compute, event, MQTT driver/forwarder/tunnel, Monitor, EMS cluster,
   AGC/AVC, display/Qt/bridge/kiosk and camera where actually configured. For
   each admitted family bind instance config path, executable/script and hash,
   unit template/effective fragment/drop-ins, required SHM names and ABI,
   dependencies, activation policy and health proof. SHM mapping policy is
   explicit per role: required exact names, optional exact names, or none;
   never accept one arbitrary approved mapping as proof for every process.
   Include non-ELF helpers and Qt nested assets in full file inventory.
   `ky-ems.service` requires `qt-display-bridge.service`; its wrapper
   `gateway-qt-run.sh` ends by execing `ky-ems/KY-EMS`, whose actual `/proc/exe`,
   dependencies and resource cost must be checked. Verify process
   identity/cgroup and role-specific liveness. Reject an
   unapproved runtime process or a required role that is absent.
2. Use a strict parser for `config/runtime` to resolve intended instances;
   compare its result with `gateway-services.sh` (`desired_units`, lines
   136-525) and then make the launcher consume the journal-approved set during
   a production transaction. Its current tolerant invalid-config skip and
   display/hardware probes must not silently change an approved role set.
   Preserve existing ordinary launcher behavior outside the production gate.
   Admission requires exact app/device/TLS/logic dependencies and unique safe
   instance names. Pin effective systemd dependency closure, not just a unit
   filename. Keep `gateway-cellular`/network failover, identity, host network,
   OTA trust, and physical controls outside this write set.
3. Obtain separate reviewed 23 (4 GiB) and 24 (2 GiB) simulator manifests:
   exact app/device files, point model, endpoints, logical SHM names, expected
   services, and explicitly absent hardware. Do not infer these from factory
   defaults or today's three running units. Any unavailable serial/CAN/DIO,
   display, camera or physical actuator role is explicitly disabled unless
   its simulator and safety controls are approved. EMS/SCADA capabilities stay
   installed; enabled strategy/control must be limited to simulator targets,
   with real output disabled. This is an operating profile, not deletion of
   product capability. Choose and pin the actual R07 event/outbox storage
   backend for each profile before build: factory config currently includes
   SQLite paths, while CMake marks `EventStore` `EXCLUDE_FROM_ALL` and the
   current builder omits it. Do not claim EventStore is a running service or
   silently switch backend. Qualify selected persistence/retention/recovery;
   any unsupported backend is explicitly disabled or blocks that profile.
   `EmsParityCheck` and `pointctl` are tool artifacts, not auto-start roles.

## One transactional write set

Extend the existing `production-adopt/install/recover` approval, not a parallel
installer. Approval binds previous/candidate baseline digests, original raw
approval digest, complete program inventory, exact old/new config paths and
SHA/mode/size, exact allowed-unit transition and effective unit/gate pins,
SHM identities, board identity, trust and expiry. The only mutable config paths
are reviewed runtime app/device/logic/TLS files needed by the candidate;
`device_identity.json`, machine code, network/cellular settings, signing key,
historical lineage and old fences remain immutable. Added/removed config files
must be explicit, not inferred from a directory copy. No symlinks, `..`, hard
links, unknown files, or unpinned systemd override.

Before the first live config/program/gate replacement, journal ownership of
all temporary names and durably back up every old path in the write set, plus
required unit files; verify pins and filesystem headroom. Stop/fence the old
approved participant closure, then install pinned program/config/unit bytes
with bounded copy, fsync and atomic rename. Recheck role derivation against the
new live config while still stopped. Start only the approved candidate closure
in dependency order; a launcher must not independently enable more roles.
Health binds the candidate baseline and stable actual processes, SHM mappings
where applicable, configured point/loopback evidence and disabled-role absence
before pointer promotion. A missing or contradictory health proof leaves the
transaction stopped. Recover under the original transaction/approval restores
the exact old config/program/unit/gates and old authority (or completes a
durably promoted candidate), archives only owned interrupted copies, and
preserves prior fence/backup/receipt records. Cover power/interruption windows
before backup completion, between each config write, after start, after health
publication and after promotion; never clear an uncertain pending marker.

Make `production_verify`, `production_unit`, `ProductionRuntime.start/health`,
`production_boot_start_current/finish`, `production_ota_resume`, and normal
recover use the same approved role/dependency resolution and baseline digest.
New boot must recreate only approved ABI11 logical segments through native
creation, then start/health the approved set after the recovery gate; it must
not infer a candidate from today's mutable config. Signed OTA uses the same
paired approval/write set and original job/transaction for failure and retry.
No v10 source recreation or fence removal shortcut.

Daily configuration delivery is part of this boundary, not a separate
permanent prohibition. `src/system_monitor_service.cpp` presently applies
MQTT config files one by one (around `buildConfigApplyReply`), and delete/
restore write live files before an optional launcher restart; direct HTTP
`configApplyJson` and its delete/restore endpoints do likewise. In production
mode, route apply/delete/restore and SCADA/project asset publication through a
single approved provision/recovery transaction with the same identity, old/new
pins, complete write set, health and terminal result. Until that path is
implemented, reject a mutating request before the first live write and return
an explicit unsupported/blocked result; dry-run/read remains distinct. A
successful per-file write is not an installed/healthy reply. Retain request ID,
machine target and per-file error semantics for Windows/platform; expose
accepted/pending, terminal healthy or recovered/unknown rather than treating
`restartGatewayServicesLater` scheduling as completion. Test multi-file
partial failure, restore/delete, rapid reply/retry and SCADA project files
against the real MQTT/direct handlers and matched client task records. This
is a source-derived risk, not an observed device failure.

## Capacity and validation

Replace the three-reader-only `activation_capacity_preflight` table, including
its fixed instance names, with documented per-role policy reserves and
dependency closure (`ky-ems` plus bridge counts both). Count additional cold-start/private working
memory once, not existing v11 SHM pages already reflected in MemAvailable;
bound point-store/tmpfs growth separately. A floor is an admission heuristic,
not an RSS peak guarantee. Measure real active services, point counts, queue
growth, startup peaks, available RAM/tmpfs, latency and storage during a
bounded workload on both 4 GiB and 2 GiB boards. If a reviewed 2 GiB profile
cannot fit, disable optional hardware/display roles explicitly or stop; do not
weaken a shared floor or promise full load from synthetic evidence.

Focused local regressions: catalog/strict config and unknown-role negatives;
each configured family/dependency and missing-hardware policy; v1 historical
read/recover; approved role expansion/reduction; old/new config pin and
identity/network/trust negatives before mutation; partial backup/copy/rename/
fsync failures with exact recovery; launcher disagreement; disabled/duplicate
process, wrong ELF, missing required role, SHM/Qt and non-SHM health; capacity
boundary/unknown; boot pending/promoted/rollback and original-job OTA retry;
MQTT/direct config apply/delete/restore and SCADA publication before-write
denial or full journal transition with truthful asynchronous task results.
Keep the already accepted diagnostic/D1/D2/D4/B2 suites unchanged; run only
affected local tests plus required integration tests. Later Linux/systemd/native
and actual signed HTTPS OTA tests remain separate gates.

## Fixed build and deployment sequence, not execution authority

First freeze edge source/config manifests and independent source review. Build
one complete AArch64 business/factory/helper/Qt payload from an exact Git
archive using the existing 22.11 Allwinner toolchain in a new isolated root;
record every source/artifact SHA, ABI, interpreter, dependencies, factory
allowlist and paired helper. Include the selected storage backend's actual
build/runtime artifacts; do not add `EventStore` merely because it appears in
`RUNTIME_BINARIES`. Do not build final binaries before policy/config
stabilize. Refresh Windows DeviceInit/paired assets only from those sealed edge
artifacts, then build the Windows package from its fixed source; build the
platform JAR/migration/UI from its fixed source and verify the durable binding
and API contract. Separate commits and artifact manifests per product.

Only after a new precise device/build scope: inspect 23 then 24 baseline,
identity, capacity, simulator availability, backups and recovery input; apply
one approved journal transaction per board, validate all admitted services,
point mapping, continuous full/forwarding/diagnostics, disabled physical roles
and stable resource envelope. Test official recovery and a controlled normal
restart with native ABI11 recreation, then signed OTA and matched Windows/
platform workflows under their own exact scopes. Preserve old B0/B1 and
23/24 evidence; no second stack, Broker change or physical actuation. Final
acceptance remains open until those deployed proofs and a bounded soak exist.

Open inputs for review: board-specific simulator/config manifests and physical
capability exclusions; exact per-role memory floors and measured limits; exact
service dependency/effective-unit policy (including display and tunnels);
approved config additions/deletions and recovery ownership; final three-product
source/artifact pins. No device action is authorized by this document.
