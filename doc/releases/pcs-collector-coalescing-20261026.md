# PCS Collection Planning and Selected-Task Coalescing

## Scope

Follow-up to the 2026-10-26 scheduler deployment on COMM202600108.
The preceding full-point 20 ms test achieved 29.26 requests/s, a 5.061 s
median complete 440-point sweep, and 85.73% median CPU usage of one core.
Those are gateway polling measurements, not the PCS internal sampling rate.

The reconstructed plan matched the captured block set: 69 blocks (20
realtime, 49 background), including 36 blocks of one or two registers/bits.
Increasing the batch cap from 32 to 64 would remove only one block. Address
gaps and priority separation matter more than the configured size cap.

## Implementation

- Cache readable point indices in function/address order for each collector.
  Filter due points using borrowed definitions, copying only into output tasks.
- Remove redundant priority splitting and unconditional offline-candidate
  copies. Move selected tasks instead of copying them again.
- Compute backoff, priority and oldest-value ranking once per task, then
  partially sort only the approved budget.
- Coalesce adjacent/overlapping selected tasks of the same function, within
  the existing batch cap. Only due, budget-approved points participate; no
  unapproved background work, address-hole bridging or interval changes.
  The earliest selected execution slot is retained, including the reserved
  background slot after a deferred slice.
- A mixed read consumes the background interval. Failed combined reads
  record each original task's failure/backoff and disable selected-task
  coalescing for that collector's lifetime. Individual reads can recover
  without repeatedly probing a rejected combined range. Existing within-
  priority batching and adaptive handling of ordinary tasks remain intact.

No configuration schema, transport pacing, electrical write policy, shared
memory ABI, control lease or network-service changes are included.

## Regression Coverage

The added adjacency regression failed on the prior implementation before
the source change, then passed. Coverage includes normal/focused scheduling,
background intervals and zero budgets, disabled/not-due points, address gaps,
function boundaries, batch caps, aliases sharing a register, decode offsets,
reserved background execution and failure recovery using original identities.
Existing slow-request, adaptive split, offline probing and completion-deadline
regressions pass unchanged.

Native tests use the release checkout in WSL. The migration test requires
`GATEWAY_MIGRATION_TEST_BACKUP_DIR` on persistent local storage; an unset
variable or tmpfs path is rejected as intended. Its persistent-directory
retest passed. The complete 110-test run had 108 passes, one environment
failure resolved by that retest, and one skipped legacy migration test:
109 passes and one skip after the focused retest. Four collector/daemon/
Modbus regression targets were also rerun successfully after the final edits.

## Build and Deployment

Implementation commit: `35be4275ccbd4a3fa01815308e7e16d99e5081b3`.
Production source: `db5c618dce3b458f1a58f09f0dadce3af0e7f683`, adding an
explicit typed lambda and `this` capture for GCC 6.3 compatibility. The
collector/daemon regressions passed again after that one-line change.
Both commits were pushed to the GitHub and GitLab `realese1.0` branches.

The first isolated cross-build exposed the old compiler's nested generic
lambda lookup limitation; no device deployment occurred from that build.
The corrected Git archive, SHA256
`4b3f84694640b1f0c1ad420c30826e71dc084b5f00530951acf6d64929e6c803`,
was uploaded and verified on 192.168.22.11, then built with the repository's
`tools/build_edge_aarch64.sh` in
`/tmp/gateway-coalesce-20261026-1645/build-v2`. No shared build tree was reset.

The Allwinner AArch64 ELF was explicitly downloaded and hash-verified:
`5e9aad585e5a12ef243d1565f6cf29df72c3691f0b6e6a117e583d040b7f6ac2`.
Only `/opt/modbus-gateway/bin/ModbusRtu-pcs108` was replaced on COMM202600108;
the old executable is retained in the diagnostic directory. No shared memory
migration, other driver replacement or network restart was needed. The
deployment service verified the new process at 16:54:46 Beijing time.

## Identical-Configuration Field Comparison

The two new stage configuration files are byte-identical to the preceding
20 ms tests. Each stage first refreshed all 440 points after driver restart.
The key-point window is 90 seconds, the all-point window 120 seconds.

| Full-point 20 ms metric | Before | After |
| --- | ---: | ---: |
| Requests/s | 29.264 | 49.325 |
| Driver CPU median, one core | 85.73% | 20.97% |
| Driver CPU p95, one core | 87.60% | 22.44% |
| Total-P wire revisit median | 1020.94 ms | 467.99 ms |
| 24-key-point sweep median | 1011 ms | 501 ms |
| Complete 440-point sweep median | 5.061 s | 4.528 s |
| Complete 440-point sweep p95 | 5.259 s | 4.991 s |
| Complete sweeps observed | 23 | 26 |
| Maximum safety-point age | 1233 ms | 559 ms |
| Driver maximum RSS | 31676 KiB | 29176 KiB |

Measured full-point CPU reduction is 75.54%, throughput increase 68.55%,
and whole-table sweep reduction 10.53%. In key-point mode, total-P wire
revisit remains effectively unchanged (120.57 to 120.52 ms), the key-group
sweep is 203 to 204 ms, and CPU falls from 14.78% to 8.10%. The changes do
not establish a meaningful key-mode latency improvement.

Wire evidence confirms cross-priority reads, including FC03 306/count 9,
1109/count 3, 1263/count 6 and FC04 229/count 17, 561/count 27. The all-point
window contains 4198 critical-only requests, 1000 background-only requests
and 706 mixed requests, versus 2340/1171/0 before. The 73 observed block
shapes after optimization are dynamic due-point subsets, not a fixed
73-request sweep and not a regression from the prior static 69-block plan.

Critical work still receives repeated priority service while slower groups
complete a sweep. That policy and address gaps remain the whole-table
limit; increasing throughput does not proportionally accelerate the slowest
point. This change deliberately does not rewrite that priority policy,
raise the batch cap, span reserved addresses or add concurrent connections.

Both stages have one PCS TCP stream, read function codes only, zero Modbus
exceptions/flagged retransmissions, zero capture kernel drops and no invalid
or stale point flags. Each has one unmatched request at the final PCAP
boundary, not a measured timeout. Full-mode response median/p95/max is
1.220/4.045/90.144 ms. No coalescing fallback or collection error was logged.

These are bounded stopped-PCS measurements, not long-run certification or
20 ms full-table sampling. The 100 ms observer can skip intermediate point
updates; the wire revisit analysis combines all blocks covering total P.
Total P did not change during either test, so no internal ADC or register
refresh-rate claim is supported.

## Restored State and Evidence

The probe finished successfully and restored original point periods,
budgets and 50 ms request pacing at 16:59:56.195 Beijing time. Original PCS
configuration SHA256 remains
`6f6718d260d20b3da38c5afb0a074c7d69b8e8015694bd037b15ba47fa6cb2e3`.
The optimized executable remains installed; 20 ms pacing is not left active.
Fresh verification at 17:02:47.828 confirmed driver PID 3965683, stopped=1,
running/fault/alarm=0, Q setpoints zero, pending=0, electrical writes disabled,
one exclusive connection and healthy 440/440 evidence with zero kernel drops.
Other original configuration hashes, boot ID, EasyTier PID 1574, cellular
PID 1781, NetworkManager PID 507, GPIO watchdog PID 510 and the usb0 default
route were unchanged. No START, power write or network failover test occurred.

Remote evidence: `/opt/modbus-gateway/diagnostics/pcs-coalesce-20261026-1645`.
Local evidence: `D:/workspace/new/comm108-coalesce-20261026-1645`.
The immutable archive is 1325248 bytes with 32 manifest entries, SHA256
`c00dcbce6573eec16ff146060805e175f2e2d806ba094f3879910af1c5c30ea0`.
The full local archive and all extracted members were reopened and verified.
The later read-only `final-check.json` is separately downloaded and verified,
SHA256 `d99957e5045ec3e6abbf98c31b4a0731915293e7a64e1cb4759b1331f5a78d41`.
Derived results: `analysis/summary.json`, `comparison.json`, `wire-focus.json`
and `block-priorities.json`. The original 24-hour evidence is untouched.

The initial tool checksum preflight rejected a Windows-CRLF checksum list
before any service started; the LF checksum list passed. An early local
verification attempted before download completion was also rejected without
producing analysis; only the completed hash-verified archive was analyzed.
No failed field benchmark stage was retried, and no automation was created.
