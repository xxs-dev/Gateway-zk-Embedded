# Native Boot A2 Actual Result

2026-10-02. One authorized 192.168.22.23 / COMM202600999 execution only.
Source fe5ffea; fixed runner 8adb057 / SHA00966182; exact A2 scope consumed.

Command: `python -B evidence/20261002-nativeboot-A2/execute-board-fixture.py --execute-approved`.
Local/remote/fixture exit1. Fixture49.113s, unittest48.240s; no timeout/replay.
27 selectors: 9 PASS and 18 not passed. Unittest reports failures4/errors16/skips0;
the errors include three subwindows of ONE selector, so subtraction would miscount.

Passing: two exact file-fsync/archive cases; same-boot missing-lock/no-mutation;
two scalar/batch show-contract cases; three AST dispatcher intent windows; the
same-boot durable owner boot-tail case. This does not validate normal new boot.

17 traceback entries now reach prepare_boot -> no_mappings, which requests a
regular file for a private SHM target/source intentionally absent after clearing
the mapped new-boot /run and /dev/shm. Missing live_v11 or retired_v10 rejects
before creator invocation. Four negative tests consequently see this earlier
refusal rather than their intended capacity/ownership/stats/tmpfs failure.
The new official-finish selector's three subwindows do not create a continuation;
their subsequent resume.json reads fail. They are NOT three confirmed injected
dispatch failures or any successful official finish/recovery.

All11 uploaded Git inputs verified; all11 post-input hashes match. Four baseline
samples (pre-upload, pre-fixture, post-fixture, final) agree in all16 protected groups.
RTU PID475164, MQTT475219, Monitor475276 stay active with NRestarts0 and identical
exe/state/config/guard/SHM identity/start-tick/mapping pins. Process group522067
has no remaining members; SSH closed and command session5839 exited. No real
business lifecycle/native creator/service/SHM/Broker/OTA/reboot operation occurred.

Failure preserved `/tmp/gw-nativeboot-fe5ffea-23-A2/work`, source, log and result;
workRemoved=false. The field afterCleanup is the final sample after the conditional
cleanup branch; it is NOT a sample after actual work removal. No reconnect, cleanup,
alternate namespace, repair or rerun. A1 work/evidence/runner remain untouched;
old WSL47230 not queried/restarted. No transition to24/B/C/D.

Receipt SHA256 25d055c1adf9ea50df945767b973909112cd623a8c780793f6fbba9683e79c7f.
Log SHA256 ca8935d6b1edbb90d022733ebbadd0e5026b944893b8a8552e973c3d9cc0d39c,
64113bytes, retained verbatim inside board-23-A2-result.json with raw baseline commands.
Full per-entry categories and passing selectors: board-23-A2-summary.json.

Only board Python3.8/private files/flock and substituted lifecycle boundaries;
not business deployment, native mutex/ABI creation, actual systemd, OS reboot or
signed HTTPS OTA. Report failure and stop pending independent review; no product
or runner changes were made under this execution scope.
