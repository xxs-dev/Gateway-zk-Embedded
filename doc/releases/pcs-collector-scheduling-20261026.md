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
No device deployment, driver restart, PCS connection or live test was made.

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
- The designated Allwinner build server was not available through the current
  SSH authentication. Native validation does not replace AArch64 compilation,
  deployment verification or a fixed-configuration live comparison. Future
  field validation must preserve the restored configuration and allow complete
  background warm-up before comparing critical and full-scan timing.
