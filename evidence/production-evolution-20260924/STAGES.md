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
