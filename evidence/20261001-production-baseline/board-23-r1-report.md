# Board 23 Private Checkpoint Fixture

2026-10-01. Single implementation agent. One explicitly authorized invocation of
runner 46e5fd9, SHA 6c758a5e...9848cc, against fixed product 7e93c74.
The full scope/input/output identities are in board-23-r1-report.json; the original
88,936-byte board-23-r1-result.json retains raw before/after systemctl output,
baseline JSON, eight input verifications, selected case names and fixture log.
The original receipt bytes are preserved in Git without CRLF normalization.
Default diff-check flags that receipt's preserved CRLF as whitespace (exit 1);
the explicit cr-at-eol check accepts its original line endings (exit 0).

## Actual Result

- COMM202600999 / 192.168.22.23, AArch64, Python 3.8.10.
- Local runner, remote command and fixture each exit 0; stderr empty.
- Eight selected cases PASS, zero failure/error/skip. Fixture wall 12.989s;
  unittest reports 12.348s. Eight uploaded Git input hashes match (374,942 bytes).
- Actual ModbusRtu/MqttDriver/SystemMonitor PIDs 475164/475219/475276 remained
  stable, running, NRestarts=0; actual executable hashes unchanged.
- All 13 compared baseline fields unchanged: identity, boot, state path/hash,
  approval, active, installed guard, configuration (12 files), stop marker,
  readers, runtime processes, seven source/target names and offline-lock identity.
  Mutable SHM contents were not hashed or restored; name equality is not inode proof.
- Fixture process group 510220 has no remaining members; subreaper setup was
  required by the runner. Only verified self-owned work was removed.
- Unique private /tmp/gw-production-checkpoint-7e93c74-23-r1 retains source,
  fixture.log and result.json. No old transaction/evidence was removed. SSH closed.
- No retry, device24 access, real lifecycle CLI, business service action,
  installed helper/config write, real SHM creation, key/trust or Broker action.

The cases cover failed rollback terminal resumption on the original durable job,
boot-hook recovery of a pending-free owner tail, same-boot positive kernel lock
proof, dangling pointer rejection, effective unit mismatch rejection, payload
ELF architecture rejection before intent, malformed boot references before
mutation, and real fixture-fork interruption with owned-file recovery.

This is actual target-interpreter evidence with real PRIVATE files, journals,
leases, fork and kernel flock. Service/proc/native SHM/network boundaries are
substituted; fixture ELF bytes are synthetic and never executed. Capacity log
values belong to synthetic meminfo, not a new actual-host capacity test.
Neither promotion nor rollback here is a real business OTA install/recovery.
Timeout/kill escalation was not triggered, so normal cleanup is proved but its
escalation path is not newly certified by this successful invocation.

## Remaining Boundary

Original WSL session 47230 still has no stdout/exit status; retained, not rerun,
and no result is attributed to its unknown source-load point. This board run does
not convert that session into a pass or add eight independent product features.
The earlier two independently accepted promotion tests were not repeated.

Native v11 boot creation/ordering and durable per-boot proof, full configured
business admission, actual new C++ production artifacts, native final-state and
affected core40/historical21 regressions, real RSA/HTTPS runtime OTA and dual-board
normal reboot/cross-product acceptance remain OPEN. The observed all-history
handoff <=128 boot limit is recorded for the NEXT boot slice: use a bounded
unfinished-owner index plus immutable retirement/history evidence, not deletion
of completed journals or an unlimited scan. No product change made for it here.
