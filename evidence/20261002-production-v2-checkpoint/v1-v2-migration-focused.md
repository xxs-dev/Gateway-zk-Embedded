# V1-to-v2 migration and newboot focused check (2026-10-02)

This is a local private-root test supplement to checkpoint `46fb995484e7b09c21cd78283d7b0bea0fbf0e7b`.
It does not change production admission or prove a board reboot.

## Commands and tool output

The commands selected only the named tests. Their original stdout/stderr and exit
codes remain in this task's tool records; this note is an index, not a raw-log copy.

- `wsl.exe --user root --exec python3 -m unittest -v tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_formal_install_promotes_configured_authority tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_failed_start_restores_formal_v1_authority`
  - First run, chunk `a9cdd8`: exit 1, two fixture errors before v2 entry. Rebuilding the candidate unit inventory lost the paired recovery-unit pin.
  - Second run, chunk `deebd7`: exit 1, two fixture assertions before v2 entry. V1 launcher-first order differed from the v2 role start order.
  - Final run, chunk `099056`: exit 0, 2 tests OK, 0.322s. Formal v1 adoption precedes v2 `inputs/install` and either promotion or an injected startup failure followed by official `recover`.
- `wsl.exe --user root --exec python3 -m unittest -v tools.production_native_boot_test.NativeBootTest.test_v1_to_v2_migration_newboot_binds_candidate_config_roles`
  - Chunk `a64621`: exit 0, 1 test OK, 0.281s. After formal migration, private newboot proof binds the candidate baseline source and role digest; post-gate health binds candidate configuration hashes.

Before the v1 adoption, the private fixture supplies the exact Monitor/launcher
unit templates; it also seeds a digest-named simulator profile in its private
production data root before v2 approval. The test proves the existing install
and recovery entry with those reviewed inputs, not a formal profile delivery
mechanism or a fixed board-specific 2/4 GiB profile.

The private fixture maps `/opt`, `/etc`, `/proc`, `/dev`, and `/run` to temporary
paths and substitutes service and native SHM creation. This does not cover actual
systemd, process ownership, memory pressure, power loss, ARM binaries, or device
startup. Only Monitor and the launcher are admitted in this fixture. Broader role
health, fixed 2/4 GiB profiles, normal MQTT/direct config journal publication,
logic delete/restore, and board validation remain open. The previously reported
default-WSL `/opt/modbus-gateway` impact remains unknown and was not touched.
