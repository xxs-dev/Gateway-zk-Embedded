---
name: gateway-superpower
description: Use for Gateway-zk / edge-gateway work: project paths, cross-compile rules, deployment targets, verification commands, and cleanup boundaries for this gateway production/EMS workflow.
---

# Gateway Superpower

Use this skill whenever working on the Gateway-zk edge repository or the edge-gateway Java platform repository.

## Project Paths

- Edge repository: `D:\workspace\Embedded\Gateway-zk`
- Platform repository: `D:\workspace\CloudPlatform\idea\edge-gateway`
- Cross-compile host: `192.168.22.11`, SSH user `root`
- Persistent remote repository: `/srv/build/Gateway-zk`, owner/build user `tronlong`
- Edge verification device: `192.168.22.16`, user `root`

The cross-compile host is a remote ESXi VM. It has no live HGFS, Windows-drive, WSL, or other
mapped source directory. Never use `/mnt/hgfs/...` or `/mnt/d/...` as a source path on `192.168.22.11`.
Old CMake caches and local VS Code/WSL tasks are not valid cross-build or release evidence.

Before every edge build, connect to `192.168.22.11` and inspect the branch, commit, and worktree at
`/srv/build/Gateway-zk`. Run Git and builds as `tronlong`; logging in as `root` is only the SSH entry
point. Never compile on the edge device. Build releaseable edge binaries only on the cross-compile
host; use `192.168.22.16` only for runtime verification.

## Source Synchronization

When the intended commit is pushed and the remote worktree is clean, synchronize with `git fetch`
and `git pull --ff-only` as `tronlong`.

When validating unpushed or staged changes, export the exact local Git index and upload it to a
unique `/tmp/gateway-build-<id>` directory. Build there as `tronlong`, copy back only the selected
artifacts, verify SHA256 on both hosts, and remove only that exact temporary directory.

Never use `scp` or `rsync` to overwrite `/srv/build/Gateway-zk`. The remote persistent repository may
contain work owned by another session. Do not run `git reset`, `git clean`, forced branch switches, or
`rsync --delete` there.

## Verification

For platform changes:

```powershell
node --check src/main/resources/static/app.js
mvn -q test
```

For edge EMS / compute changes, run on the cross-compile host:

```bash
sudo -iu tronlong
cd /srv/build/Gateway-zk
git status --short --branch
cmake --build build-aarch64 --target legacy_ems_test -j 4
qemu-aarch64-static -L /home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/aarch64-linux-gnu/libc ./build-aarch64/legacy_ems_test
cmake --build build-aarch64 --target ComputeEngine -j 4
```

Directly running AArch64 binaries on the host without `qemu-aarch64-static -L .../libc` is expected to fail.

## Production Package Notes

- Factory package name: `gateway-factory-defaults.tar.gz`
- Install command on the edge device:

```bash
tar -xzf /home/gateway-factory-defaults.tar.gz -C /home
START_SERVICES=1 RESET_SHM=1 sh /home/gateway-factory-defaults/deploy/install-factory-config.sh
```

After deploy, check:

```bash
/opt/modbus-gateway/deploy/gateway-services.sh list
systemctl status gateway-services.service
```

Also inspect shared memory with the existing point tools when relevant.

## EMS Rules

- Production `ComputeEngine` only accepts `graphEms` backed by one executable `schemaVersion=2.x` file.
- `legacyEms` is restricted to Windows one-time migration, `EmsParityCheck`, and isolated regression tests; never add it to a production app config.
- Run V1/V2 shadow comparison through `EmsParityCheck` with isolated stores, not by enabling both rule types in one production service.
- Keep `pcsWriteback.submitWrites=false` during candidate verification. Enable writes only after V2 output is verified against the isolated baseline.
- Runtime state belongs under `/opt/modbus-gateway/data`, not in config OTA packages.
- Graph templates should keep profile-controlled branches explicit:
  - `Meter_TQ`
  - `Meter_CN`
  - `Meter_BW`
  - `Meter_FH`
  - `BMS_MODEL`

## Cleanup Boundaries

Do not commit generated or local-only artifacts:

- `build-aarch64/`
- `target/`
- `tmp/`
- `graph_ems_*.json`
- `gateway-factory-defaults.tar.gz`
- `config/gateway-otapackage/*.tar.gz`
- uploaded engineering point-table/project archives
- `.claude/`, `.codex/` caches except intentional skill files

Before commit, verify:

```powershell
git status --short
git diff --cached --name-status
```

Commit source, config templates, scripts, docs, and tests only.
