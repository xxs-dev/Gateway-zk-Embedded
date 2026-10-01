# R10 Local Checkpoint

2026-10-02. Sole EDGE agent; LOCAL_ONLY. Baseline `18b600b0aa0d4a15f602fb830b3078dd5102befc`.
This is a source checkpoint for independent review, not Linux/AArch64 or production acceptance.

## Change

Nine code/build/test paths are pinned in `manifest.json`. Diagnostics no longer run
the synchronous popen helper in handleDiagRequest. One private Linux supervisor
owns each command group/subreaper tree; monitor ticks poll at most 64 KiB, cap
output at 256 B..1 MiB, and enforce a monotonic 10 s command deadline.
Normal exit, signal, timeout124, cancellation125 and cleanup-unknown are distinct.
Unknown cleanup permanently closes diagnostic admission; notification does not
release a still-live supervisor. Stop requests cancellation even for runOnce-only
objects. Destructor waits at most3 s and reports uncertainty rather than claiming
unconditional cleanup of unkillable kernel tasks.

Requests retain cmdId/target correlation and the existing fixed vocabulary.
In-flight identical retries merge silently; conflicts refuse. Completed results
are cached before delivery (16 records, total8 MiB,5-minute process-local TTL);
unexpired records are not evicted for new work. This is not durable cross-restart
idempotency. Existing non-diagnostic shell/telemetry paths remain unchanged and
can still block; this does not prove whole-monitor or whole-stop latency bounds.

The final fixture adjustment waits at most1 s for the private stalled command
to launch before exercising duplicate/stop assertions, avoiding a scheduler race.
The other final narrow correction uses a no-FD10 ms poll during stopping: the
cancel socket remains readable/HUP and otherwise spins a retained reaper. The
test-only300 ms retention now traverses that actual cleanup loop (all real
children already reaped), with a100 ms wait/less-than50 ms owner CPU assertion.
This POSIX backoff assertion is written, NOT executed on Windows.

## Actual Local Evidence

- Historical exact helper with a corrected Windows command harness:447 ms,
  command exit0,2 output bytes; timing assertion exit1 (RED).
- Source structural checks:3 PASS/exit0. These are not behavioral substitutes.
- Windows helper syntax exit0; service/new-fixture syntax exit0 with3 existing
  warnings (two dirent comparisons and unused durable parameter).
- Portable standalone compilation exit0, executable exit77 (POSIX SKIP);
  the PowerShell wrapper reported1 for this nonzero skip, not a successful suite.
- Diff check exit0. No new binary artifact has been built for Allwinner.

`captured-results.json` preserves available captured tool outputs, including
the invalid first quoting reproduction and the comment-matching source-harness
failure. Neither is product RED. The corrected reproduction stdout contains
a literal backslash-n; it is not silently converted into a raw newline log.
Earlier uncaptured harness/syntax errors remain described in session history,
not reconstructed as invented raw logs.

## Open Gates

POSIX code has not been compiled or run here. Linux fork/signals/subreaper,
escaped descendants, FD closure, wire/EOF races, and actual service/safety fixture
all remain unverified. The tests presently model supervisor death BEFORE command
creation, not death after launch. Actual parent death is also missing.
Abrupt supervisor death after launch can leave descendants orphaned: the parent
poisons admission, but cleanup of those descendants is NOT proved. This is a
production ownership gate, not a resolved failure mode. Kernel D-state and
oversized/failed child inventory also cannot be presented as guaranteed cleanup.

Windows same-second logical cmdId collisions and platform propagation of new
reason/signal/timeout/cleanup fields need separately scoped contract work.
D1 paired-helper inventory closure is separate and untouched.

## Boundary

No SSH, devices, build host, VM, Broker, platform, WSL query/restart/kill, push or
stable-ref changes were performed. Unknown untracked files were preserved.
All exec calls used for this resumed local checkpoint completed; no new remote
session exists. Original WSL47230 remains unqueried/unknown, not declared finished.
No coordinator status or shared review was edited.
`linux-validation-proposal.md` is NOT execution authorization.
