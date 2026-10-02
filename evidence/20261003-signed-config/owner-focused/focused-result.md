# Signed config edge checkpoint (LOCAL_ONLY)

This checkpoint adds `config` to the signed/sized runtime handoff, pairs the
v2 role parser in a strictly inventoried stage, and uses the existing
`ProductionRuntime` journal for approved config changes. Old v1 three-helper
records and v1 baselines without a parser remain valid under their original
rules. It is not a production or device acceptance.

## Private results

- `wsl.exe --user root -- env PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v` with the two selectors `test_v1_to_v2_introduces_parser_missing_from_legacy_baseline` and `test_v1_to_v2_failed_start_removes_new_parser_on_recovery`: exit 0, 2 PASS, 0.383s. At that run, `deploy/offline-runtime-upgrade.py` SHA-256 was `a9535752a81bd83e73ae8c31be71e3f405f7dacc35517689a3c87674bdbdb0b4`, and `tools/production_runtime_baseline_test.py` was `a9e270146dfe0ef32e8e1cefee3154cffc0979d41563c3e580c47382f2e9175d`. Inputs were unchanged before/after. Separate raw streams and the exact selectors are in `../v1-parser-compat/`; `stdout.raw` SHA-256 `a5f4a42ac8895906e9501ff76ab4b09123dff74bfd9c94de8fd971e683c2db6d`, `stderr.raw` SHA-256 `e8604a526e59a3cddcdc5feedd17c632921340816c5b7090fc8b4b458eafb507`.
- `wsl.exe --user root -- env PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v` with `test_v2_signed_config_real_owner_add_delete_replace_and_finish`, `test_v2_signed_config_rejects_candidate_release_before_payload_copy`, and `test_v2_signed_config_rejects_low_space_before_payload_copy`: exit 0, 3 PASS, 0.788s. Five recorded inputs were unchanged before/after; full SHA list and exact selectors are in `inputs-before.json`, `inputs-after.json`, `run.json`, `capture.json`. `stdout.raw` SHA-256 `0bc9b5e145f8ad9981bcc45038488b6e5eded7811833eb7297ac0a37fff3e302`; `stderr.raw` SHA-256 `dd8c97126205efcbf1fa33c274352a64c4b4b2f4de1c12e6276f3d7e2f81d89a`.
- Local host configure: `wsl.exe --user root -- cmake -S . -B /tmp/gw-signed-config-local-build -DBUILD_TESTING=ON`, exit 0. Local host build: `wsl.exe --user root -- cmake --build /tmp/gw-signed-config-local-build --target ota_runtime_handoff_service_test -j 4`, exit 0 and compiled `src/ota_service.cpp`. The build was not captured as separate raw streams and is not an executable test result. Current `src/ota_service.cpp` working-file SHA-256 is `3864112b372c792c5a4439830a0a02bd907dc0c1cf7326fbc804c1b35c616ffc`.

The private owner tests replace `guard`, absolute `Path` mapping, systemctl,
and owner manager controls. They exercise journal/lease/health logic but do not
prove real launch, systemd, RSA verification, device behavior, or power-loss
durability. The successful config TAR in those tests was hand-built from the
same private fixture, **not** the TAR produced by the platform checkpoint.
The platform-to-edge identical-TAR bridge, F2 admission refusal lifecycle,
full release tamper/old-base negatives, normal MQTT/direct journal entry,
ordinary logic delete/restore, ARM build and deployment remain OPEN.
