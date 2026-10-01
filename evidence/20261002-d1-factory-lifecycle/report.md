# D1 Factory Lifecycle Local Fix

2026-10-02; parent 599d1a0; single EDGE agent; LOCAL_ONLY. No production acceptance.

## Changes

- One shared staging helper for builder and installer: validate the complete regular paired trio, disable only the literal CORE_TEST_APPLY_ALLOWED value, preserve adjacent/UTF-8 statements, reject duplicate/nested/nonliteral and non-assignment bindings. Guard/health are byte-exact. Source input is not modified by installer; builder changes only its private staged package.
- Installer lifecycle authority is extracted PACKAGE_ROOT, otherwise explicit/resolved SOURCE_ROOT, otherwise DEPLOY_DIR. A partial/invalid explicit package cannot borrow lifecycle files from the external installer. OTA apply/rollback, gateway-services and recovery unit selection uses the same lifecycle directory.
- The installed installer and its new stage helper both come from the invoking SCRIPT_DIR; helper is mandatory and copied into bin. Actual installed-bin entry succeeds on a fresh private home; same entry rejects an initialized home with unchanged file inventory. This remains factory initialization, never an in-place runtime upgrade.
- Optional root suffixes are guarded across deploy/config/templates/examples/binaries/KY-EMS/manifest/public-key selection; empty config root refuses rather than probing /runtime. Explicit valid roots remain covered by real private installs.

## Evidence

Original four-case RED: exit1, 4 failures,21.233s. The initial pairing assertion was masked by the True policy. Exact parent shell entries then independently reproduced packaged-False/external-guard mismatch (exit1,10.938s) and absolute empty-root candidates (exit1,0.344s). Logs distinguish the earlier mixed-source selector harness from its corrected historical selector.

Final actual installer/AST suite: 13 PASS,0skip,59.893s,exit0. Actual base/project/full archive regression plus invalid-policy publication refusal: 2 PASS,1 explicit POSIX-main SKIP,6.030s,exit0. Missing fcntl first-run errors are retained, not relabeled as product RED or Linux PASS. Shell syntax/Python3.8 grammar/diff-check exit0.

Python3.14.3 on Windows + Git sh; no WSL. Synthetic binaries never execute. INSTALL_SYSTEMD=0,START_SERVICES=0,RESET_SHM=0; only guard kernel inventory paths map to private empty directories. Tests automatically clean their owned TemporaryDirectories; completed local subprocess sessions have returned. Terminal transcripts stored as UTF-8 LF captures with two trailing-space lines trimmed for diff-check, not byte-exact original pipe archives. All five exact source/index SHA256 and commands are in checkpoint.json.

## Still Required

Linux/Python3.8 actual installer/real-main checks, POSIX metadata/fsync, legacy lifecycle compatibility and full configured AArch64 package/device closure remain unverified. The production-init extracted-source calling shape is exercised, not its complete interactive/enrollment/systemd entry. No device deployment, native boot/reboot, signed HTTPS/RSA OTA or business evidence is claimed. No helper/runtime ELF rebuild, SSH, Broker, WSL47230 operation, service/SHM/state/approval mutation, shared/coordinator document change or push occurred. R10 and business build remain separate follow-up slices.
