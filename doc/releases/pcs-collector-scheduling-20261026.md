# PCS Collector Scheduling Review

Date: 2026-10-26 (Asia/Shanghai). Branch: `realese1.0`.
Base: `dca6578` (the separately reviewed UI changes).

## Problem and Scope

An exclusive PCS Ethernet port does not bypass application scheduling.
The collector previously selected a whole realtime/background batch once,
then executed it without reconsidering newly due realtime points. Increasing
the background budget could therefore delay critical revisits even when the
PCS replied quickly. Sorting by age was also skipped when every task fitted
the count budget; scheduling timestamps used the batch entry time.

This change affects the shared Modbus collector and the daemon's scheduling
feedback, not the PCS protocol, point map, safety thresholds or live settings.
The initial code-review phase made no device deployment, driver restart,
PCS connection or live test. The subsequently authorized full deployment and
read-only field verification are recorded below.

## Changes

- Check a monotonic time slice between eligible physical requests, including
  adaptive split probes. The slice is the shortest enabled readable point
  interval, capped at 1000 ms. In-flight I/O is never interrupted.
- During background work, yield when a realtime point becomes due inside the
  current invocation. Replan against updated point history on continuation.
- Always order task candidates by existing priority/backoff/oldest-value
  policy, even when the count budget would allow the whole list.
- Give one selected background request an early slot if the previous slice
  deferred all selected background work. This prevents starvation under an
  overloaded realtime queue. Update the background cadence on actual I/O,
  not merely on selection.
- Record read/update/backoff times at completion, using monotonic elapsed time
  relative to the caller's time. Successful telemetry retains its wall-clock
  acquisition timestamp.
- Return deferred-work and next-due hints. The daemon resumes deferred work
  after a 1 ms interruptible yield and rechecks control leases. Future due
  times can wake it between fixed ticks without accumulating extra deadlines.
- Unattempted work is not published as failed. A failed partial slice does
  not throw the whole-batch failure exception; actual failures still update
  the existing device/task failure counters and thresholds.

Configured task counts still bound selection per collector invocation;
adaptive probe limits and failure backoff policies remain in use. Continuation
is a scheduling wake-up, not a 1 ms Modbus polling configuration. The existing
single-connection, serialized client and frame pacing are unchanged.

## Verification

Native Linux/Qt Debug build in WSL completed. New regressions cover:

- 20 realtime tasks plus 233 background tasks, 100 ms simulated duration per
  request, 1000 ms point intervals, background budget 32, for 500 slices.
  Both ordinary and realtime-focused modes refreshed every task; maximum
  critical revisit gap was 2203 ms. Each slice stayed within 1000 ms in this
  fixture, and deferred points were not marked bad/offline.
- A realtime point becoming due during background requests preempts the
  remaining background batch, before a full extra slice elapses.
- Completion-based due times, partial-failure/offline thresholds, slow
  adaptive split progress, existing budgets, grace periods and backoff tests.
- Daemon continuation, stopping continuation when work is finished, honoring
  a newly acquired control lease, and returning to normal cadence after
  intermediate due-time wake-ups.

The stress fixture intentionally uses 233 separate background tasks. It is
not a claim that the field's 233 background points require 233 wire requests.
The earlier offline field planner reported 49 background wire tasks.

Initial full CTest run: 105 passed, two skipped, three failed out of 110.
The migration test subsequently passed with its required isolated backup
directory and root host-namespace context. The OTA final-state failure was
also reproduced on the unchanged scheduler baseline `dca6578`:
`mutation faults must not prevent reading other trusted finals`.
The protocol E2E failure was also reproduced on `dca6578` with all required
drivers built: the simulated CAN write is rejected with
`failed to lock ownership state`.

Final rerun on the completed scheduler changes, excluding those three tests:
105 passed, two skipped, zero failed (107 registered tests, 268.62 seconds).
Together with the separately passing migration test, the verified result is
106 passed, two pre-existing failures reproduced on the baseline, and two
skipped tests. No unrelated test or production code was changed to hide them.
`git diff --check` passed. Local logs are retained in
`generated/scheduler-review-20261026/` in the release checkout (not committed).

## Limits and Field Follow-Up

- A single slow/timeout request may exceed the time slice; this is cooperative
  scheduling, not a hard real-time deadline or transport cancellation.
- Realtime demand above serialized transport capacity cannot be made to meet
  every configured interval. The 2203 ms result is a deterministic software
  fixture, not an on-device freshness guarantee or a PCS sampling limit.
- Background cold-start coverage still requires a complete scan. Restarting
  between short observation windows discards the process-local scheduling
  history; do not use repeated short windows as full coverage evidence.
- No frame interval, batch register count, point period, protection threshold,
  electrical permission or protocol interpretation was changed. In particular,
  the conflicting 20/100/5 ms document scopes remain unresolved.
- Allwinner build-server authentication was initially unavailable. With the
  subsequently supplied credentials, the target build and deployment below
  completed. Future timing comparisons still need equal observer workloads
  and complete background warm-up; deployment success is not proof of a
  sampling-ceiling improvement.

## Authorized Full Deployment to COMM202600108

Source: `f9178f2edd14ee22cc20d5cc96cf78ec76502ae2`, branch `realese1.0`.
All times in this section are 2026-10-26, Asia/Shanghai (UTC+08:00).

The user authorized replacing all existing runtime drivers and briefly
pausing point sampling and packet capture. Original configuration files,
EasyTier and 4G connectivity had to remain unchanged. PCS START, electrical
write enablement and competing PCS connections remained prohibited.

The isolated source snapshot on 192.168.22.11 was verified before the full
Allwinner AArch64 and native Qt cross-builds. The 17,888,687-byte runtime
package SHA256 is
`0b97028624c4a6686934cc985b7f2a169e9e481e507ebd8c60b0df5e78cb2e00`.
Both ARM regression executables (`collector_partial_failure_test` and
`gateway_daemon_realtime_priority_test`) passed on 108 in private namespaces
with simulated devices, without connecting to the real PCS. Earlier QEMU
attempts could not exercise process-shared robust mutexes and are not counted
as passing target tests. Native transaction tests covered migration, partial
exchange/installation recovery, pending-command refusal, protected-service
checks and backup/deadline behavior.

### Attempts and Recovery

1. At 15:03:30.334, the first attempt entered maintenance. It took 149.205 s
   to reach runtime restart, exceeding the PCS's configured 120 s EMS
   communication timeout. Fresh PCS fault feedback caused automatic rollback;
   the transaction was `ROLLED_BACK` at 15:08:05.449. Raw read responses showed
   fault word 617 = 2 and ARM_EMS communication fault = 1. This supports the
   maintenance outage as the cause, not a decoding error. Fault feedback was
   zero after recovery. The full attempt/recovery window was 275.115 s, not
   merely the initial 149.205 s interruption.
2. At 15:19:59.940, the second attempt used a live database seed but offline
   verification of the large history database exceeded the migration deadline.
   It aborted before executable/canonical-SHM exchange and recovered at
   15:21:38.444 (98.504 s total attempt/recovery window).
3. Reviewing the old/new sample and alarm SQLite writers confirmed no schema
   change. The final transaction left history databases in place and limited
   offline data backup to the schema-sensitive MQTT outbox/spool. It did not
   relax safety checks or change the build. Maintenance began at 15:25:01.786;
   runtime restart began at 15:26:02.669 (60.883 s), and fresh point/capture
   evidence was confirmed at 15:26:05.666 (63.880 s). Following a 240 s bounded
   observation and watchdog restoration, it reached `COMMITTED` at
   15:30:31.048.

These are separate maintenance windows, not one uninterrupted acquisition
period. Previous binaries, SHM objects, backups and failed-attempt evidence
were retained. No remote history evidence was deleted by the deployment.

### Installed State and Safety

- All 20 selected installed executables were replaced and SHA256-verified,
  including Modbus/PCS, DLT645, DIO, CAN, IEC, MQTT, runtime engines, monitoring,
  display, pointctl and the native migration utility. Installed-but-inactive
  drivers were not activated just to test them.
- All 11 canonical SHM segments migrated ABI 10 to 11 using the native tool;
  atomic exchange retained old objects and preserved canonical names.
- Original configuration hashes match the first preflight, including PCS
  configuration SHA256
  `6f6718d260d20b3da38c5afb0a074c7d69b8e8015694bd037b15ba47fa6cb2e3`.
  Point periods, budgets, original 50 ms frame pacing and electrical write
  settings were not changed.
- EasyTier PID 1574 and cellular-service PID 1781 were never restarted.
  NetworkManager, GPIO watchdog and the originally inactive failover service
  retained their original states; default route remained via 192.168.43.1
  on usb0. The remote session remained reachable. No failover/disconnect test
  was performed, so this does not certify recovery from every possible outage.
- Fresh checks confirmed stopped=1, running=0, fault/alarm=0, three Q setpoints
  zero, pending=0 and one exclusive PCS TCP connection. All previously active
  runtime services and general evidence resumed. Temporary maintenance
  inhibition was removed. Business watchdog reported healthy with zero
  consecutive failures. A further check at 15:38:03.698 confirmed the same
  runtime PIDs, configuration/network invariants and 440/440 evidence with
  zero capture kernel drops.

### Observed Timing and Limits

The old-runtime baseline contains 152 observations over 179.207 s. The new
runtime contains 109 observations over 239.270 s. All 440 points were observed
updating; neither set contains bad-quality or stale flags. Safety-point age
p50/p95 was 689/1199 ms before and 1782/2336 ms after. Background maximum
observed age was 60.463 s before and 158.597 s after, including restart/cold
history. The new observation loop also performs additional sequential service
and network checks before stamping each row. These unequal windows do not
establish a speed improvement; timestamp gaps can skip intervening updates.

The immutable post-restart PCAP covers 406.262 s, longer than the point
observation window. Wireshark reassembly found:

- 7,839 read requests on one TCP stream: FC01=677, FC02=676, FC03=2966,
  FC04=3520; no write function, Modbus exception or flagged retransmission.
- 7,838 matched responses. The single unmatched request is the last packet
  at the capture boundary, not evidence of a measured response timeout.
- Response latency median/p95/max: 1.202/7.225/170.319 ms.
- Request throughput: 19.293 requests/s; median request gap: 50.128 ms.
- Status block FC02 address 81/count 10 revisit median/p95/max:
  1207.390/1282.915/1401.365 ms. P/Q block FC04 address 201/count 26:
  1209.987/1281.582/1418.647 ms. Exact block gaps are not interchangeable with
  whole-device scan time, especially where adaptive grouping changes ranges.

This confirms installation and bounded stopped/read-only operation under the
unchanged configuration. It does not certify the PCS internal ADC rate, a
maximum stable polling rate, millisecond full-point scans or long-run behavior.
The successful-start journal retains a Qt runtime-directory ownership warning
and an unprovisioned direct-maintenance credential notice (sensitive endpoints
remain disabled); this deployment did not alter their configuration. Earlier
attempt logs also retain MQTT reconnection/TLS errors. Active services alone
do not certify every inactive driver or cloud telemetry delivery path.

### Evidence

Remote transaction: `/opt/modbus-gateway/diagnostics/scheduler-upgrade-20261026-1430c`.
Local evidence: `D:/workspace/new/comm108-scheduler-20261026-1430`.

The immutable `deployment-evidence.tar.gz` is 966,180 bytes, SHA256
`f384038d8c56772c7a6e57eb489fbd3301003be79880d7a16eebf464048d207c`.
All 20 archive members were extracted safely, manifest hashes verified and
local files reopened for SHA256 verification. It includes all three transaction
states, original/post-install observations, failed-start capture, successful
capture, native tests, migration scripts and final installed-state verification.
Local derived results are `deployment-summary.json` and `wire-summary.json`.
Failed-start raw files had been preserved and hash-verified locally before
recorder retention removed the rotating originals; those exact pinned copies
were included. The earlier failed export did not create an archive.
