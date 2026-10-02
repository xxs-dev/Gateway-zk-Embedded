# Configured-role parser local checkpoint

- Source base: `7bf4e971e3b378017af888e03d550f7425375a3c`.
- Scope: read-only v2 role resolution, not wired to the v1 guard, launcher, install, start, boot or recovery entrypoints. No production admission is enabled.
- `deploy/production_role_policy.py` SHA256: `6b2dbfbe6d24bd02e3de882a6acce84e444f9a41712ced41240626f9ecffbb55`.
- `tools/production_role_policy_test.py` SHA256: `4c6c6705551bdf0583685a243690db9535404f79576670efb9ac1866ed03bf34`.
- Final command: `python -m unittest tools/production_role_policy_test.py -v`; exit 0, 27 tests, 1 Windows symlink skip. Raw output `targeted-unittest.raw.log` SHA256 `8aae59e931781f193a64124b77117d51a2fcc02d4fbe8885b673fcc33206df34`.
- Earlier draft output is retained as `draft-23tests.raw.log` SHA256 `862fe0992cda45a893ec5f2f1856223139863ae9771326a673144e67d64dda1b`; it does not validate the final source.
- Python 3.8 AST parse and `git diff --check` passed on the final files.

R1 now computes effective per-main SHM sets, including loader defaults for camera, MQTT singular, compute output and Monitor sibling discovery. R2 rejects camera custom commands and requires a pinned loopback RTSP receiver; the result records `/usr/bin/ffmpeg` and `/bin/sh` as external runtime dependencies. R3 rejects `udp_test` while claiming a vcan SocketCAN binding. Tests cover missing and explicit fields and negative configurations without starting processes.

This is source-level Windows Python evidence only. POSIX symlink behavior, live effective systemd units, external binary presence, actual SHM creation, capacity, full business activation and recovery, signed OTA, ARM build and board behavior remain unverified. The conservative memory floors are admission inputs, not measured peaks or production guarantees.
