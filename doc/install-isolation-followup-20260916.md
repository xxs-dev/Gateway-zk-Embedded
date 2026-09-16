# Offline Installer Isolation Follow-Up

## Baseline And Scope

Base: `5d40272e61e708e4694b4bdc6f15e2886efcd73e`, retaining the 5b6 runtime
and mandatory-payload preflight. No C++, headers, CMake, SHM layout, protocol,
device configuration or compiled artifact changes. The earlier v7 branch is
not a candidate. No device connection, service operation or cross-build was used.

## Corrections

- Factory `INSTALL_SYSTEMD=0` no longer installs the host network-failover
  defaults. `START_SERVICES=0` and `RESET_SHM=0` remain mandatory in this mode.
- Factory exit cleanup removes the watchdog applying marker only after this
  invocation created it and its PID still matches. Preflight and isolated
  failures leave another invocation's marker intact.
- Factory extraction uses a uniquely created temporary directory. A failed
  archive extraction exits instead of silently continuing against fallback
  source files; exit cleanup removes that invocation's extracted files.
- SCADA rejects `--restart` only with explicit `INSTALL_SYSTEMD=0`. With the
  variable unset, the existing service lookup, restart, PID/active checks and
  rollback restart remain available. Omitting `--restart` makes no systemctl call.
- SCADA metadata and temporary state files are cleaned on success and failure;
  rollback removes the unfinished activation link and restores the previous
  current link and app configuration.
- Production init creates, owns and cleans its work directory. An explicitly
  supplied pre-existing `INIT_WORK_DIR` is now rejected without deleting it.
  A fresh directory is required; `INIT_KEEP_WORK_DIR=1` still retains owned work.
- INT/TERM terminate with nonzero status before EXIT cleanup, so a caught signal
  cannot accidentally continue installation or bypass failure rollback.

## Verification

Run on local Linux/WSL as root, with evidence outside the protected mount paths:

```sh
python3 tools/install_isolation_test.py --evidence /var/tmp/install-isolation-results
sh -n deploy/install-factory-config.sh
sh -n deploy/install-scada-project.sh
sh -n deploy/production-init.sh
git diff --check
git diff 5d40272e61e708e4694b4bdc6f15e2886efcd73e -- src include CMakeLists.txt
```

The test re-executes in private mount and IPC namespaces, makes mounts private,
then overlays `/etc/default`, `/etc/systemd`, `/run`, `/tmp` and `/dev/shm` with
private tmpfs. Missing namespace support is an error, not an unsafe fallback.
All systemctl invocations use a mock; fake binaries are never executed.
Protected-path contents are checked against sentinels for isolated operations.

- Original baseline: 13 tests, 5 passed and 8 failed with the reported symptoms.
- First fix: all original 13 tests passed.
- Final: 19 tests passed, including normal factory install, normal copy failure,
  isolated success/preflight/copy failures, archive cleanup and corrupt archive
  refusal, flag guards, pre-existing init-directory preservation, SCADA restart,
  isolated refusal, activation, state-write failure rollback and restart rollback.
- Existing shell regressions passed: `scada_install_test.sh`,
  `scada_rollback_test.sh`, `factory_package_component_version_test.sh`, and
  `runtime_mode_package_test.sh` with locally generated full/runtime fixtures.
- POSIX `sh -n` passed for all three modified scripts; diff whitespace passed.
  The C++/CMake comparison above is empty.

Local evidence directory:
`D:/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/install-isolation-evidence/`.
`baseline-5d40272/result.txt` records RED; `green-verified/result.txt` records
19/19 GREEN (7.107 seconds). Per-test logs are adjacent. Intermediate fixture
setup failures are retained in `fixed-full` and `green-final`, not counted as PASS.

## Boundaries

### Mixed-Case Boolean Follow-Up

The early production-init guards now call the same `truthy` helper used by the
later execution paths. The helper is defined before the guards; its accepted
values and default-mode behavior are unchanged. Against `7291416`, three added
tests (start/reset/smoke, each with `tRuE`, `yEs`, `oN`) produced nine failing
subcases: start proceeded with installation, reset was rejected only by the
downstream factory guard, and smoke reached execution. The corrected fixture
uses an unchanged copied init script and a harmless smoke marker script.
Evidence: `mixed-case-red-fixture/result.txt` and `mixed-case-green/result.txt`
under the evidence directory above. After the fix all 22 tests passed in 7.487
seconds, including all nine mixed-case subcases; rejection occurs before the
gateway home is created, with no systemctl/smoke call or protected-path change.
POSIX syntax and whitespace checks passed. C++/CMake remain unchanged.

`INSTALL_SYSTEMD=0` is a side-effect switch, not a filesystem sandbox. Callers
must supply a dedicated `GATEWAY_HOME`, app config, SCADA root, state-file path
and backup paths. `FACTORY_TMP_DIR`, `SCADA_TMP_DIR`, `TMPDIR` and `INIT_WORK_DIR`
are caller-controlled; keep explicit overrides inside the staging area. Defaults
for factory/init work use the gateway home, and SCADA metadata uses the SCADA
root's parent when neither temp override is set. No global `/tmp` fallback is
introduced. Inputs and destination symlinks are trusted deployment inputs;
this patch does not add archive sandboxing or concurrent-install locking.

The factory installer is still not a multi-file transaction: an I/O failure
after binary copying begins can leave a partially installed staging tree. Tests
verify protected host paths stay unchanged, not automatic restoration of every
factory file. SCADA's existing release/config rollback is tested separately.
SIGKILL, power-loss atomicity, real systemd health, AArch64 execution and device
deployment are not established by these offline tests. Keep the fixed 5d402
build running; integrate these script changes separately without recompiling C++.
