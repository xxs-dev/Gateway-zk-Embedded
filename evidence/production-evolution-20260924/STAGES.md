# Production evolution local stages

## Integration and source-to-artifact boundary

Official `realese1.0` was clean at `7e392ce` and was an ancestor of the reviewed
candidate `ff9cf1a`. It was fast-forwarded locally to that candidate without
conflicts, resets, remote push, builds or modification of `D:/workspace/realese1`.
The runtime source remains the qualified `9ccfce8` composition. Candidate evidence
commits are included, not substitutes for a new ARM build.

## Rollback stage

- `0fbb1d6`: real `ota-rollback.sh` red fixture. A failing service restart returned
  success and prematurely restored the applied-version marker.
- `b89f3f0`: propagate service recovery failures; preserve the error code and
  best-effort cleanup; stop only attempted recovery units, including the implicit
  TLS gateway-services recovery; defer the version marker until success.
- Old backup copies do not replace `/opt/modbus-gateway/data/` or device identity.
  A second retained red fixture demonstrated stale durable-state overwrite.
- SCADA resources retain their separate recovery path and do not acquire the
  new whole-runtime manual-stop behavior.
- `rollback-boundaries/result.txt`: seven selected tests PASS, including four
  service-action subcases and three existing SCADA/factory shell regressions.
  Marker-write failure preserves exit 42 and still cleans up/attempts stop.
- Fixtures execute actual scripts with a fake systemctl in private mount, network
  and IPC namespaces. `/opt`, `/dev/shm`, `/run`, `/tmp`, and systemd/default
  directories are private tmpfs. No host installation or device access occurs.
- This stage changes only shell/test files; all qualified R5 runtime ELF files
  remain reusable. Cross-ABI entrypoint guards and coordinated offline migration
  are separate unfinished stages, not qualified by these rollback tests.

## Test-only helper duration stage

Only the explicit `--duration-seconds` upper limit changes, from 600/three digits
to 2100/four digits. There is still no default duration, renewal or extra mode.
SIGALRM handling, whitelist, TTL1000, refresh200, OFD singleton lock, SHM identity
checks, and all runtime ABI/production sources remain unchanged.

The expanded CLI test accepts 600, 1800 and 2100; rejects empty/zero/negative,
fractional, suffixed, duplicate and >2100 values; checks EOF and explicit quit;
and keeps stdin open to verify actual one-second SIGALRM exit and OFD lock release.
It does not wait 30 minutes or claim endurance acceptance.

`run_helper_duration.py` compiles only helper and its test against the existing
qualified native candidate library, recording source/library/binary hashes.
`helper-duration-final/result.json`: both compile, isolated test and C++14 syntax
PASS. `helper-duration-old-comparison` compiles the original helper Git object
against the expanded current test and fails at the expected four-digit duration
startup. The first red/green attempts instead hit a test-fixture collision with
the previous helper's still-fresh input; those logs remain preserved. Subsequent
duration-only cases no longer write inputs, retaining the production refusal.

For ARM, only `ems_shadow_live_helper` needs a new binary for endurance. The
helper test is an optional verification target. Do not rebuild R5 product ELF
files or mutate sealed packages. Implementation-test alone may build/run later
under Coordinator's pinned authorization; this stage authorizes no remote action.

## Interim entrypoint restrictions

Online OTA/rollback cannot replace runtime ELF based only on self-declared ABI.
Legacy V9 rolling upgrade is retired. Factory initialization rejects existing
runtime/config/data and explicit SHM reset. Service start/apply checks configured
SHM headers, preserves segments and propagates stop failure with manual-stop kept.
The unreachable factory SHM wildcard deletion has been removed.

`entrypoint-guards-final/result.txt`: 16 selected actual-entrypoint regressions
PASS, including cold factory positives, plain config OTA, SCADA separation and
rollback error handling. Guard Python 3.6 syntax PASS. Earlier trial failure and
12-test result remain retained. No runtime ELF was rebuilt or deployed.

This is a temporary safety restriction, not a completed offline upgrade path.
Header checks alone do not establish executable ABI compatibility. The next
stage must use externally pinned component approval and actual migration CLI,
with stopped-control recovery instead of unsafe automatic ABI downgrade.

## Offline apply/recover entrypoint

The new small Python entrypoint uses the existing migration CLI and externally
approved manifest/component hashes, not a rebuilt ELF marker or package ABI claim.
The reviewed R5 manifest pin and operator approval schema are documented in
`deploy/OFFLINE-RUNTIME-UPGRADE.md`. All installed runtime products, exact config
bytes, identity/roster, full offline voter attestations and SHM names are checked.
Exact-unit persistent systemd conditions preserve enabled/masked state. There
is no network/kernel-watchdog management, runtime start or implicit authorization.

`offline-entrypoint-trial`: 6/7 passed. The positive recovery caught EXDEV while
retaining a newly introduced executable across filesystems. Fixed to exclusive
copy/fsync/hash verification before unlinking the transaction-created live file.
`offline-apply-recover`: 9/9 passed, including cold factory positives. Real native
CLI validates source byte preservation, V11 latest/history/receipt retention and
cleared owners/claims/pending/authority. Negatives cover pending commands, owners,
live mmap, partial multi-segment migration, pinned-byte/voter omission, stop failure
and a genuine bind-mount EBUSY after the first executable has been replaced.
Recovery restores old program/config bytes but preserves newer durable dedup and
both SHM generations. General startup stays fenced. No ARM execution claimed.

Only a disposable fixture generator was compiled; the native migration CLI and
all R5 product binaries were reused. The existing runtime source is unchanged.

## Observer start, real package mapping and pairing

`observe` is separately externally approved after every fixed voter confirms the
same component manifest and its own completed state hash. Only approved compute,
cluster and monitor instances may start; physical drivers and launcher remain
persistently fenced. All product/config/source/target hashes and absent live maps
are rechecked. Original enabled/masked policy is unchanged; masked observers and
replayed receipts are refused. Start/is-active/reinhibition failures stop attempted
observers best-effort and cannot report success. This is observation, not physical
control or an automatically running old-ABI rollback.

Coordinator findings corrected: templates receive condition drop-ins but no
instance stop/show operation; explicit LocalDisplayQtEms -> KY-EMS mapping uses
the approved identical payload hash; outputSharedMemoryName is included in both
reference extraction and rewriting, with an output-only configuration fixture.
The full factory builder and installer require the existing migration CLI; paired
deploy scripts include both new Python entrypoints. Base/project scopes remain
unchanged and require the external complete approved payload for offline upgrade.

`offline-observe-final/result.txt`: 15 targeted tests PASS, including real R5
20-product manifest preflight against the hash-pinned recovered baseline archive
and R5 delta, with the approved Qt alias. Twelve probes are not installed.
No R5 ELF is rebuilt/executed here. Real native migration CLI was exercised in
the separate apply/recover fixtures. Runtime `src`, `include`, `apps`, migration
CLI source have zero diff from qualified9ccfce8; deploy changes need script pairing,
not a runtime rebuild. Python3.6 AST checks passed for both new scripts.

The first observer trial had two incorrect expected drop-in filenames in the test;
the actual entrypoint succeeded/reinhibited correctly. Fixed assertions, retained
failed result. A later 12-test output reported OK but its incremental receipt
contained113 NUL bytes; it is retained as evidence of an unqualified receipt, not
counted as a clean named-test result. The existing runner now buffers its report,
fsyncs it and verifies exact readback. `offline-pairing-readback/result.txt`: five
focused observer/pairing and affected service/config tests PASS with clean readback.

Final script bytes/hashes: `offline-pairing-readback/paired-deploy-delta.json`.
Native test artifact/source hashes: `native-fixture-provenance.json` in the same
directory. Disposable fixture executables are preserved outside Git under
`C:/Users/12193/AppData/Local/GatewaySuiteEdge/offline-fixture-evidence-20260924`.

Remaining: no real ARM device migration/systemd start, SIGKILL/power-loss recovery,
or physical PCS qualification. Real-systemd dependency effects require device
implementation review. Approval receipts are operator attestations, not an online
signature/vote protocol. Existing stopped V10 authority/pending bytes are never
automatically reauthorized; original-control rollback is intentionally unsupported.

## Real runtime schema correction

While reading fixed9ccfce for S2 diagnostics, Edge found two actual delivery
blockers missed by reduced installer fixtures: runtime membership uses
`assignments`, not `members`; Compute also uses `outputDefaultSharedMemoryName`.
Coordinator withdrew the8926a5f offline pairing candidate and authorized this
separate deploy-only repair. No performance/product/ABI/protocol change is mixed in.

`real-schema-red` fails with missing `members` against the real R4 roster and full
public app. After the roster correction, `real-schema-shm-red` reaches installation
but detects the real Compute default output still naming old SHM. Both retained.
`real-schema-final`:10/10 selected tests PASS; includes12 malformed-roster cases,
hash/config mismatch, expected-voter count/locked cabinet checks, actual R4 and
S2 app+roster apply/observe/recover, full parser key coverage, approved Qt alias,
full-voter enforcement and stop/service guards. Seven earlier green cases retained.

R4 inputs are checked against their existing immutable manifest. S2 public app
and roster are reproduced by the reviewed structured relocation and checked
against the exact S2 payload pins, never live credentials. For disposable execution
only controlEnabled is set false and identity/membership paths are moved to the
canonical private namespace locations; native V10 fixture bytes supply the SHM.
The whole app is used, not a hand-written reduced replacement. Test systemctl is
still simulated; this is not device/runtime-network qualification.

Roster validation binds schema/epoch, unique nodeId/cabinetNo, local node and all
enabled configurations' clusterId, expectedMembers/maxMembers/lockedCabinetNo.
The wrong members shape has no compatibility fallback. Non-default roster paths
are refused explicitly. Both script operations share all five runtime SHM keys;
unknown/wrongly typed references or exact old names left in unsupported fields
refuse migration before switching. Original hashes and all-participant fences stay.

New eight-script pins: `real-schema-final/paired-deploy-delta.json`. Product ELF,
SHM ABI and KECP protocol unchanged; no remote connection or product rebuild.
Windows must re-pair changed scripts; prior8926a5f artifacts remain preserved.

## CPU P1: capability snapshot

Coordinator authorized only capability sampling after the diagnostic benchmark
was committed as `ddbd131`. The bridge now makes one existing batch16 query per
sample. It does not cache across samples, alter the store/SHM ABI, or change target
short-circuit evaluation, point validation predicates, control or authority code.

`capability-snapshot-red` runs the new actual-bridge regression against the pinned
original library and fails specifically at16 single calls/zero batch calls.
`capability-snapshot-green` compiles only the new bridge object, links it before
that unchanged library, and observes zero single calls/one batch with exactly the
same16 indexes. All309 emitted semantic cases match old output: missing/late
points, bad quality, NaN/Inf, timestamp/TTL/expiry boundaries, flag thresholds,
feedback mapping, deleted/reused slots, reinsertion and configuration disable.
The store's existing stale-marking semantics are preserved rather than redefined.
Target short-circuit assertions also pass. Three existing affected native suites
pass: ems_cluster, ems_cluster_output_authority, ems_cluster_strategy_startup.
They include authority epoch, leader/term/membership and restart safety coverage.
C++14 syntax passes on local GCC15; GCC6.3/ARM is not executed.

`s2-cpu-p1` measures only the new bridge using the same fixture/compiler/library
conditions as the retained old benchmark, without rerunning old primitive tests.
Median actual capability+target process CPU falls from1274.837 to354.926us before
helper inputs and from477.125 to292.043us with helper inputs but absent feedback.
These are local sparse/uncontended Debug results, not an ARM utilization claim.
No qualified R5 executable/archive is modified. P2 health-regex work is separate.

Source/link boundary: only `ems_cluster_main.cpp` instantiates the production
EmsClusterPointBridge and calls sampleCapability; CMake assigns that translation
unit to EmsClusterCoordinator. ComputeEngine/MqttDriver/MqttForwarder also call
the unchanged addEmsClusterPointRoutes function in the same library object.
With the existing function-sections/gc-sections size option they discard unused
bridge methods. Without that option, these executables may retain changed dead
bridge code. A future minimal ARM build must verify the sealed flags and symbols;
functional need is EmsClusterCoordinator, not all20 runtime executables. No ARM
build is authorized by this source-dependency analysis.

## CPU P2: fixed health expressions

LoadSampler now owns four const regexes, constructed once per sampler instead of
three/four times per sample. The original patterns, conversion/fallback logic,
health age bounds, conditional healthy evaluation and exceptions are unchanged.
No JSON parser replacement or timing/CPU threshold adjustment is included.

`load-sampler-old` and `load-sampler-new` run the actual LoadSampler with only its
wall clock frozen by a Linux linker wrapper. All23 emitted cases match exactly:
missing/empty files, inclusive5000ms old and60000ms future boundaries and their
outside neighbors, false/string/uppercase healthy, negative/decimal/exponent-like
tokens, duplicate/nested fields, whitespace, and stod out_of_range exceptions in
each numeric field. These preserve existing permissive regex parsing, not claim
that it is a complete JSON validator. Local C++14 syntax passes; GCC6.3 remains
unexecuted. The standalone test is registered only for Linux, where its wrapper
applies. No cluster suite from P1 is rerun, since bridge code is unchanged.

Three retained repetitions of the actual sampler (including /proc and health file
reads) show median native Debug CPU115.856->21.900us with absent health and
149.082->33.869us with fresh health. Regex construction moves into startup;
startup and ARM utilization are not qualified. Only EmsClusterCoordinator owns
LoadSampler, via its sole ems_cluster_main.cpp entrypoint. R5 files remain sealed.

## Offline explicit paths and modes

The original bin-only inventory/write/recovery path missed the real factory
`ky-ems/KY-EMS` location. Schema2 now requires one complete installPaths mapping
through inventory/apply/observe/recover and relative-path-keyed old hashes.
Only approved bin names and the explicit Qt location are accepted. Qt resources
are retained, not moved. Managed bin/ky-ems trees reject symlinks and unapproved
runtime/ELF paths, including nested extras. Inventory is rebound after stop and
before observer startup; an added-ELF observer red test caught that last gap.

Required mode distinguishes fixed-voter (unchanged roster/quorum/full-voter
requirements) from standalone (no enabled/ambiguously typed EMS cluster in any
config, local identity+stop evidence, no fabricated roster/voters). Standalone
observation permits only Compute/Monitor, not cluster or physical participants.
All data/identity/consensus/SHM preservation and stopped-recovery boundaries stay.
Schema1 input is explicitly incompatible; retain the old pinned script pair for
any existing schema1 transaction instead of rewriting saved approvals.

Evidence sequence, not a single all-tests run:

- `path-mode-red`: real Qt mapping rejects against old bin inventory. The first
  standalone case was also masked by that same inventory refusal; retained.
- `path-mode-roster-red`: standalone fixture separated from Qt, now independently
  fails on the old required membershipSha256. No fake roster supplies the test.
- `path-mode-green`:4/6 pass (real Qt, R4, S2, partial-file stopped recovery).
  Two test setup expectations failed: the full repository AGC app also references
  a second SHM segment; the obsolete alias negative now rejects missing installPaths
  earlier. Fixed fixture coverage/assertion, not product behavior.
- `path-mode-supplement`:10/10 pass, including those two corrected cases, path/hash/
  symlink/extra ELF/duplicate refusals, mode/local evidence/EMS bypass refusals,
  standalone observer restrictions, fixed full-voter/roster negatives, actual
  pinned R5 twenty-product preflight and deploy pairing.
- `path-mode-observe-red`: added runtime ELF after apply was not checked before
  observation. Reused the exact inventory validator to close it.
- `path-mode-observe-final`:5/5 pass, including added-ELF refusal and both affected
  Qt/fixed and standalone positive observer paths, masked/foreign-voter negatives
  and final pairing. An initial selection typo ran no tests before correction.

Across these final outcomes16 distinct selected methods pass. Source files and
raw failed/positive receipts are retained; disposable native fixture executables
are held outside Git. These are namespace/fake-systemctl/native CLI tests, not
real systemd or ARM qualification. No business runtime was rebuilt in this stage.
Final eight deploy script pins: `path-mode-observe-final/paired-deploy-delta.json`.
Its runtime pin still describes sealed R5; the separate P1/P2 Coordinator change
needs its own future approved manifest, not silent replacement of that pin.

M1 boundary: implementation commit128c8a7's `execution-m1/receipt-23.json` SHA256
`0c1ca32a26c71ec268b4302a9cda0959ef54d5187cb68b2e496c017ecd6eeb1a`
contains only actual A camera/monitor projections before collection refusal.
It confirms Qt at ky-ems/KY-EMS (SHA256
`c920ec9ea83ada5f344810fea4cc484586367992893670bf13c5d5bc2cd3b08f`),
but not complete actual app bytes, units/ExecStart or SHM headers; B was not read.
The standalone positive uses full checked-in noncluster app examples with source
hashes in standalone-fixture-provenance.json, NOT this partial projection as a
replacement for the real app. Unknown active implicit readers, especially empty
camera SHM references, remain an actual-device qualification gap. Full acquisition
restart/physical output is still a separate unapproved operation.
