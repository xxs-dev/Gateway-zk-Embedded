# v2 config directory preflight (local only)

Base: `9bc1d92`. `inputs` now applies the same root/descendant directory
location, root ownership and non-group/world-writable predicate as official
recovery and live parent writes, before creating a transaction or stopping a
service. Approved empty `apps/devices/logic/tls/network` directories remain
accepted; unknown directories and symlinks remain rejected. It does not
chmod/chown or remove any existing directory.

The first two new real-entry negatives on the old input failed as expected:
`config-directory-red.raw.log`, tool chunk `7586a2`, exit 1, SHA256
`7f6b5451f5975342939ea3c26c1be0557b2e8cdef9de4a8063aba1b1d972a8e1`.
An unrelated 0775 `devices` directory reached injected endpoint health; a
0775 changed `apps` parent failed only during config write. The final command
used the same first two tests plus a 0755 unknown empty directory and three
affected positive install/recovery paths:

```text
cmd /d /c "wsl --cd /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0 -u root --exec python3 -m unittest -v tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_unrelated_config_directory_rejected_before_intent tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_changed_config_parent_rejected_before_intent tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_unapproved_empty_config_directory_rejected_before_intent tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_config_add_delete_replace_install_and_promotion tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_config_completed_addition_then_fault_restores_absence tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_empty_logic_after_completed_deletion_allows_next_app_recovery > evidence\20261002-production-v2-checkpoint\config-directory-final.raw.log 2>&1"
```

Final tool chunk `a5a4ca`, exit 0; raw log SHA256
`2b774d1269fa3e63ba22acde90e0d2a75e03da00cf29c45742199d0df1d395aa`;
6 tests passed, 0 skipped/failed, 1.051 s. Pre/post working input SHA256
matched: `deploy/offline-runtime-upgrade.py`
`80369fc99a50c3eb37a6a4914e94eec26471267df0c6c1f9e91e83368e03e1f6`,
`tools/production_runtime_baseline_test.py`
`825bf278dac84eeadae52cc562082be0907aafaf2b87676a3b9d2552862710b8`.

The factory package builder copies `config/factory` with `cp -a`, and the
installer copies packaged `runtime` with `cp -a`; source/package directory
metadata can therefore reach a fresh install. Packaging-side normalization
and a separately approved, inventory-backed remediation for existing device
directories remain OPEN. A preflight refusal is not an authorization to
rewrite installed metadata, erase pending/fence/history, or claim deployment.
