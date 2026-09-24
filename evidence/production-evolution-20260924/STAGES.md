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
