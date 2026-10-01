# Native Boot 23 A4 Actual Execution

2026-10-02. One command under nativeboot-23-A4-execution-scope.md on 192.168.22.23 / COMM202600999, Python 3.8.10 / AArch64.

Source 6e09390c70adeed724e88edc23a2b9fb87178b48; runner cd23e8e5e4757451e2e247b5e52594bf7a07de26.

`python -B evidence/20261002-nativeboot-A4/execute-board-fixture.py --execute-approved`

**2 PASS / 0 failure / 0 error / 0 skip. Local, remote and fixture exit 0.** Fixture 3.445s, unittest 2.849s. No retry or additional host. No A3 passing cases repeated.

## Checks Reached

Memory selector supplied private MemAvailable=0 and matched the exact activation refusal. Tmpfs selector supplied sufficient private MemAvailable and statvfs available bytes=0, reaching the exact native-boot capacity refusal. Each executed assert_unadmitted (native_calls empty; no boots intent/proof; no live_v11/retired_v10 objects; source bytes unchanged) and no reader start. The A3 failed combined selector remains preserved as failed; its former unexecuted branches now have separate target-Python evidence.

No product capacity code changed. These are private simulated insufficiency cases, not a measurement of host memory pressure or a production memory/load guarantee.

## Preservation And Cleanup

11 fixed input and post-input hashes match. All 16 business baseline groups match across preflight/before/after/afterCleanup (four samples), including PID/start ticks/ELF, SHM identities/maps, state/active/config/guard/marker and offline lock. Raw commands/baselines remain inside the original receipt.

Fixture process group 525812 is empty; SSH is closed and the local command exited. Successful inode-verified removal touched only this run's owned `work` directory. Final baseline sampled **after** work removal; source/log/result root retained at `/tmp/gw-nativeboot-6e09390c-23-A4`. A1/A2/A3 evidence and failed work retained, including A3's 30 passing selectors. WSL47230 untouched.

## Raw Evidence And Boundary

- board-23-A4-result.json: 139432 bytes, SHA256 c82246efcb974199b88dd52cba5b089df0a3c7e399f5ff720f6faee315ab5350. Original CRLF receipt bytes, not reserialized.
- board-23-A4-fixture.log: 2738 bytes, SHA256 7289f3c8e0ab561a9d6da0a938eeff29032ccebf67cfc43a8b6935fe8564b296. Exact remote log recovered from original receipt and checked.
- board-23-A4-summary.json: selectors, assertions reached, 11 posthashes, baseline/cleanup boundaries.

The raw plan's remoteAuthorized=false is preparation provenance; the later fixed scope authorized this one execution. No raw fields rewritten.

Only Linux Python/private-file/flock fixture qualification, with constructor/systemd/proc/network substituted. Not actual native creator/mutex, real systemd/OS reboot, business deployment, signed HTTPS OTA or B/C/D acceptance. No follow-on source repair or device operation under this consumed scope; wait for independent audit/new precise scope.
