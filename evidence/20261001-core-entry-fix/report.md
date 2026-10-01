# Core Read-Only Entry Review

Base cb45f3fc78cd6288d9bb49f06f93ca07b441ecfa, branch
fix/edge-review-r1-20260930. Frozen for independent review; no commit, push,
worktree, subagent, device migration/start/deployment or native program rebuild.
Remote exec/SSH contexts from implementation are all closed. Local test jobs
and private namespaces have exited. Unknown existing untracked evidence/cache
was retained. No coordinator state/shared review files changed in EDGE role.

## Diff

- deploy/offline-runtime-upgrade.py:158added/37removed.
- deploy/runtime-upgrade-guard.py:192added/8removed.
- tools/core_readonly_activation_test.py:new narrow local test suite.
- gateway-services.sh unchanged; existing activated-list/guard launch path is
  already suitable, but exact paired SHA installation is still mandatory.
- Evidence only in this directory; full frozen patch and byte hashes are in
  product-and-test.diff and review-manifest.json.

CORE_READONLY_TEST is an opt-in on the existing approval/manifest/ready hash
chain, not another updater or a change to historical pins/profiles. Only
COMM202600999 RTU+MQTT+Monitor and COMM202600998 Monitor readers are accepted.
Complete19program manifest includes the existing18candidate ELFs plus the
formal migration CLI. Installed Qt/other programs not replaced are separately
pinned in disjoint preserved inventory. Guard+launcher+4necessary OTA/smoke
scripts are paired individually. This future25-path scope is not deployed or
authorized by this report; it needs its own exact manifest/approval review.

Core source magic/version/native size, hashes and dev/inode are checked before
file replacement. All participants must be stopped/PID0 and maps/open FDs
absent. Existing migration CLI makes verified persistent backups and new v11
targets; script checks backup SHA, source identities/bytes and target ABI
before switching. Sources are never unlinked, renamed or exchanged. JSON
changes must equal only the approved internal SHM mapping; other configuration,
point properties, identity, network, TLS and Qt environment remain unchanged.
Ready cannot add readers; excluded services stay stopped and exact effective
fence coverage is verified on activation/start. Failure refences and official
recovery restores old programs plus old references consistently. New absent
monitor binding/backup reject dangling symlinks; recovery removes only the
validated campaign-created binding, never an arbitrary path.

## Tests

New18 + old21 + existing focused5 =44tests, all exit0. Focused5cover runtime ELF
OTA refusal, plain OTA guard behavior, compatibility shape refusal, legacy B
observe roundtrip and legacy recovered A absent-guard handoff/activation.
Python3.6 AST compatibility and git diff --check exit0. Exact commands and
durations in manifest; private namespace receipts under related-regressions.
Early failures are retained in early-failures.md. Tests use synthetic pins,
mock systemctl and migration only at the process boundary. They do not certify
the native CLI, current device lineage, a real Broker or real OTA.

## WIN02 Preservation

Read only local historical files under
C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-windows-live-execution-20260928-win02.
Actual approval SHA b3ac290348f357ed27b11afa0bbaa91aaea7b1b5b5643519b8cfb2dfde4f8f99;
readback-recovered-state SHA8ebd221f59a95d271616d5f1e71128029e904cdb04696e7b37fbab0a957ccef7,
phase RECOVERED_STOPPED,7segments/26file records. Seven source hashes match the
latest22.23 read-only source evidence. Qt display pin
c387afad62e32945f698517060cdddfbde608d153576455d7d68e92ee7979cfc and monitor
inbound-source pin6a64c844c339a826986bfeea7de2c7af85526a439ae9d36c8fe1f1d2bcf1ba0b
are real historical metadata, not permission to repeat WIN02 or its scripts.
Core permits inherited Qt/inbound/SCADA metadata only as predecessor preservation
proof. The full old predecessor checks stay intact; core bypasses A rewrites,
Qt environment append and SCADA current switching, not the lineage checks.
Tests cover successful synthetic legacy-to-core successor and metadata/receipt
drift refusal. Real current retained targets/backup/fences/receipt remain a
separate implementation gate. The available operator-terminal-recovery-summary-1
is NOT an offline-shm11-recovered-1 receipt; it cannot be passed as one.

## Remaining Gates

The native migration build runner exited1 without a JSON receipt; no remote
build return code/log or executable SHA is known. One object, no executable,
no lingering build process, source/package pins unchanged. Next implementation
phase must diagnose/recover actual logs/exit status and incrementally complete
only this CLI; do not rebuild18ELFs or bury this missing artifact at deployment.

Ordinary runtime ELF OTA remains forbidden. The persistent stop fence remains
through apply/activate/recover and blocks even ordinary non-ELF OTA. There is no
approved official fence-exit lifecycle in this change. Real official OTA success
is therefore NOT completed; plain guard acceptance without a fence is not a
device result. Do not delete marker/old state or enlarge OTA permissions. Any
isolated ARM lifecycle fixture is a separate result, not actual OTA installation.
Temporary OTA/public-key/process-CA permission remains for a later legally
reachable test; its byte restoration baseline is the valid migrated config,
whereas full rollback pairs old program with old SHM references.

After independent review and explicit commit approval, this same agent can
return to implementation for the CLI and current WIN02 read-only qualification,
then separately approved exact deployment/MQTT acceptance. No follow-on action
is triggered by this report.
