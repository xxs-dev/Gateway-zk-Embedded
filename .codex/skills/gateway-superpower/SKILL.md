---
name: gateway-superpower
description: Use for Gateway-zk / edge-gateway work: project paths, cross-compile rules, deployment targets, verification commands, and cleanup boundaries for this gateway production/EMS workflow.
---

# Gateway Superpower

Use this skill whenever working on the Gateway-zk edge repository or the edge-gateway Java platform repository.

## Project Paths

- Edge repository: `D:\workspace\Embedded\Gateway-zk`
- Platform repository: `D:\workspace\CloudPlatform\idea\edge-gateway`
- Cross-compile host: `192.168.22.11`, user `root`, path `/mnt/hgfs/Embedded/Gateway-zk`
- Edge device: `192.168.22.7`, user `root`

Never compile on the edge device. Build edge binaries only on the cross-compile host.

## Verification

For platform changes:

```powershell
node --check src/main/resources/static/app.js
mvn -q test
```

For edge EMS / compute changes, run on the cross-compile host:

```bash
cd /mnt/hgfs/Embedded/Gateway-zk
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

- `legacyEms` and `graphEms` can coexist for shadow comparison.
- Keep `pcsWriteback.submitWrites=false` for shadow mode.
- Enable writes only after graph output is verified against legacy logic.
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
