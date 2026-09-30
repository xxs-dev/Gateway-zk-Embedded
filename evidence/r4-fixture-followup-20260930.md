# R4 fixture and active-job follow-up (2026-09-30)

Branch: `fix/edge-review-r1-20260930`. Isolated WSL x86_64 native only: disposable tmpfs mounts on `/tmp`, `/opt`, `/run`, `/dev/shm`. No target device or package build.

- `tools/ota_fixture_cleanup_test.sh`: native exit 0. Both OTA fixtures rejected a pre-existing `/opt/modbus-gateway`; its sentinel remained unchanged. Cleanup deletes an installation only after this fixture created it, the path resolves exactly to `/opt/modbus-gateway`, and the path is not a symlink.
- `tools/ota_apply_self_upgrade_test.sh`: native exit 0. Installs the explicit current OTA script/helper baseline, applies a real self-upgrade package, verifies job-bound healthy result, upgraded script hash and both version markers. A running runtime ELF OTA is then refused by the current guard; original binary hash, live test process and version markers remain unchanged.
- `tools/ota_async_health_test.sh`: native exit 0. Existing failure/success/interrupted-recovery cases remain green. A late old health helper with a different active job/token is rejected before writing any result or version marker.

The fixture uses local fake systemctl and process state. This is not real device restart, MQTT connection health, or a rebuilt Windows initialization payload.
