# Native Boot A1 Actual Result

2026-10-02. One authorized 192.168.22.23 / COMM202600999 execution only.
Scope coordinator commit 8e16cc8; source e3739ba; fixed runner ec94429.

Command: `python -B evidence/20261002-nativeboot/execute-board-fixture.py --execute-approved`.
Local/remote/fixture exit1. 44 tests: 23 PASS, 7 FAIL, 14 ERROR, 0 SKIP;
fixture 49.724s, unittest 48.851s. No timeout or replay. This is NOT acceptance.

11 uploaded Git inputs verified; all 11 post-run hashes match. All 16 business
baseline groups match before, after and final sample. RTU PID475164, MQTT475219,
Monitor475276 remain active with NRestarts0 and identical exe/config/state/guard/
SHM identity/start-tick/mapping pins. No real lifecycle CLI/native creator/service
or host SHM change was called. Fixture process group519874 has no remaining members;
SSH closed and the local command session exited. Original WSL47230 untouched.

Failure correctly retained `/tmp/gw-nativeboot-e3739ba-23-A1/work`, source, log and
result. No work removal occurred: `afterCleanup` is the runner's final sample after
the conditional cleanup branch, NOT proof that this failed run's work was removed.
No additional connection/cleanup, alternate root, test rerun or product repair.

Initial log triage: 16 cases are blocked by the private systemctl show substitute
returning batch rows where the caller expects a scalar property; one mapped
/proc/locks is missing; one pending-job setup calls production_context while its
unresolved install correctly inhibits startup. Two file-fsync cases have archive-count expectation
mismatches; one zero-call assertion counts read-only systemctl inventory queries.
These are initial categories, not proof that the underlying product paths work;
blocked native/boot branches remain unvalidated. Original tracebacks are preserved
verbatim inside board-23-A1-result.json, together with raw baseline commands.

Receipt SHA256 d690df3e426dfa4a80acae110de2db8a069b64ba78cd8f7e637d30b8a831dcb5.
Fixture log SHA256 71ab06006ce060ea2b6236c5f18a3b1f494cec24685d88dda777b0b89a29d014.
Detailed categories and unchanged-field assertions: board-23-A1-summary.json.

Linux/AArch64 interpreter and private fixture only; not business deployment,
actual pointctl/native mutex construction, systemd/reboot or signed HTTPS OTA.
24 and B/C/D were not executed. Stop here pending independent receipt review.
