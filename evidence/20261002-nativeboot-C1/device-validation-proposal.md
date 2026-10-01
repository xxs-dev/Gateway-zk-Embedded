# C1 Local Manager Validation Proposal

2026-10-02. LOCAL_ONLY preparation; no C/device/VM/WSL/Broker connection.
B2 was accepted independently; its scope is consumed. All A/B1/B2 evidence is
preserved. Product source remains unchanged at082071e.

## Fixed Proposed Scope

23/COMM202600999 only; new /tmp/gw-nativeboot-082071e-23-C1.
Five read-only inputs,369831bytes; exact SHA/size/mode in input-manifest.json.
Three product modules are original Git blobs, plus two explicit observation
adapters. No ELF rebuild/download/transfer, business lock, SHM creation or mutation.
Nine transient units only, derived by unit_names/planned_commands. Names must
all be absent before controller launch. No installed unit file edits, aliases,
daemon-reload, reboot, trust/network/sysctl or business service action.

Default command, local pins/grammar only:
python -B evidence/20261002-nativeboot-C1/execute-manager-fixture.py

Future sole execution command, NOT authorized now:
python -B evidence/20261002-nativeboot-C1/execute-manager-fixture.py --execute-approved

New independent fixed C1 scope is required before that flag. No automatic24/C2/D
or retry/new root if a call fails, times out or has unknown delivery outcome.

## Ten Ordered Checks

1. Actual RuntimeHandoff.cgroup_mount/process binds caller and PID1 to real
   manager hierarchy. Hybrid name=systemd v1 is not confused with secondary v2.
2. Actual manager holds private readers/continuation queued behind a gate.
3. Actual production_boot_dispatch persists DISPATCH_INTENT/DISPATCHED and queues
   worker After gate; repeat on the same dispatched work does not resubmit.
4. Reader/ExecStartPre observation occurs after successful gate completion.
5. Failed private gate's Requires+After reader remains closed.
6. Actual product guard rejects an unadopted private home under ExecStartPre;
   substituted reader body never runs. Not positive production guard acceptance.
7. Original RuntimeHandoff.launch/own reaches OWNER_READY while donor lease held,
   with no callback execution/mutation.
8. After release, original OWNER_OWNED lease/PID/startTicks/unit/cgroup/InvocationID
   and exact argv are verified live. Cloned identity negatives are rejected without
   writing damaged journal. Original recover cancels the private unchanged job.
9. Only an owned donor process exits after READY; the same original lease passes
   to the actual independently managed owner, then original cancellation proves
   no tasks and failed/unchanged health.
10. Real submission followed by an injected controller communication exception
    is unresolved; second launch must reject consumed journal without another
    dispatch. Original intent/job cancels; no install or new transaction.

Ten checks share some manager scenarios; not ten independent full lifecycles.
Donor-before-READY/every crash window and runtimeMayChange=true recovery remain
outside C1, not silently treated as covered.

## Explicit Product/Fixture Boundary

RuntimeHandoff class logic, durable private journals/index/locks, actual manager
commands, flock, /proc/PID/cgroup/mount bindings run unchanged. Owner executable
is a pinned observation shim staged under the expected offline filename; it calls
the actual own method but replaces install with bounded file observation only.
Controller/donor/owner command recording wraps actual subprocess execution, not
manager responses. Global160 O_EXCL slots include all three process roles.

production_gate is called unchanged; ConfigParser reads its actual dependency
fields. Only the two official gate-name references map to the private unit.
Positive ExecStartPre is an observer. The negative uses the actual guard.
production_boot_dispatch itself runs; its publisher/authority is a private
fixture, After points at the private gate, and finish executable maps to an
observer. Actual phase save remains, but this is NOT real full boot authority,
native boot preparation, production_boot_finish, health promotion or install.
All mappings and original/generated command bodies are emitted for review.

## Deadline and Cleanup

Outer hard420s plus existing20s kill grace; SSH channel450s. Accounted363s:
input validation15 + initial unit shows18 + controller90 + group/subreaper12 +
cgroup selection5 + nine per-unit cleanup budgets108 + three baselines90 +
filesystem/setup/result allowance25. Margin57s. Pre-upload baseline separately
bounded by60s command; no manager actions in that stage.
Baseline stages use30s alarms. Cleanup each unit12s includes show2/stop3/
reset-failed2/show2 and task proof/logging; cgroup selection independently5s.
Entry CPU15/AS256MiB/file32MiB/fd64/core0 and25s observation wait.
Controller uses retained B2 CPU/AS/file/fd limits with CPU/elapsed reduced90s.
No indefinite retry. Uninterruptible kernel I/O is not guaranteed to finish by
these user-space deadlines: absent proof must fail and preserve the namespace.

Controller forks are contained by B2's owned session/subreaper cleanup. Detached
manager units are separately checked: initial absence, expected transient
fragment, exact recorded Python argv, then scoped stop/reset-failed and
MainPID0/no job plus actual RuntimeHandoff.no_unit_tasks. An unknown unit
fragment/argv is NOT stopped. Per-command raw exit/stdout/stderr or explicit
UNKNOWN timeout fragments survive. Entry self-expiry is NOT absence proof.
Positive work cleanup requires all10 checks, empty group, all9 unit proofs,
unchanged16 business groups and parent namespace. Final baseline follows cleanup.
Root/audit/command reservations/responses/source/result/logs are retained; only
inode-checked initially-empty work may be removed. Historical terminal receipts,
business fences/transactions/backup/intent are never removed.

## Local Evidence and Limits

Final command-record-test.py:6PASS/0failure/error/skip,1.921s, exit0.
Real private Windows files and separate child process, actual recorder AST;
manager is stubbed and Windows O_NOFOLLOW flag substituted. This is not Linux
fork/flock/cgroup/systemd proof. Previous coordinator six-test pass covered the
pre-atomic snapshot; it is not reused as proof of these final bytes.
Source/embedded remote Python3.8 grammar passes (not interpreter execution).
One preparation anchor assertion expected3 occurrence replacements but actual
pinned B2 had2; corrected before any connection. One manifest patch hunk-order
attempt was rejected without modifying files, then reordered. Neither is a
product/device failure. No A/B/old suite repeated.

A supplies private product lifecycle tests; B2 supplies exact old native creator
cold/reopen/ftruncate/resume/refusal in private namespace. C1 reuses that evidence,
does not multiply it. D must still test current full configured B0/B1 runtime
and normal OS reboot/native creation/real gate+worker admission, signed HTTPS/RSA
original-job OTA, failure/rollback/terminal recovery, simulator qualification,
receiver outage/reconnect and sustained main acquisition. D1 factory pairing/
raw-source resolution, R10 diagnostics and cross-end R01-R14/N01-N05 remain OPEN.
WSL47230 remains unknown/untouched. No product change, push or device operation.
