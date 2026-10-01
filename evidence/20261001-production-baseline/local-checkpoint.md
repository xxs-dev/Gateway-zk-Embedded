# Production Baseline Local Checkpoint

2026-10-01, EDGE role, single agent. Parent 3057a94. LOCAL_ONLY.
Exact file sizes and SHA256 are in local-checkpoint.json. The raw worktree pins are
explicitly distinguished from the one Git LF blob override (src/ota_service.cpp).
The first staged-blob assertion exposed this difference; it was not a product test.
All eight proposed Python-fixture input blobs equal their raw worktree pins.
This is an intermediate,
not fully validated source checkpoint, not deployment or production readiness.
No device/build-host/VM/Broker access, service operation, push, or stable-ref move.
Unknown existing changes/untracked evidence are preserved and excluded.

## Implemented Slice

The existing offline/guard chain now supplies production-adopt/install/recover/boot,
immutable complete runtime inventories, a fsynced current pointer and pending journal,
paired file/gate/boot-unit restoration, and health promotion. It does not replace old
profile pins or rewrite historical R2 records. Raw externally hashed approval bytes
are preserved. Program copy/restore streams at 64 KiB and archives only temporary
files proved owned by the specific transaction. Unknown files remain fail-closed.

Structured health policy generates one loopback GET; curl --disable is first and
no caller argv, upload, redirect, proxy, config or client-file options are admitted.
Qt nested inventory is preserved; unsafe paths/symlinks are rejected. Effective
systemd inventory is checked, with only the standard /lib -> /usr/lib alias handled.
Payload ELF architecture is now checked before intent, not merely after replacement.
Boot pending type/ID/SHA validation precedes path construction and owner handling.

The independent OTA owner keeps the original job lease. Official original-job
recovery resolves durable promotion/health/version/final-journal tails without
reinstalling. C++ bounded reconciliation accepts a distinct bound completion proof;
the existing cancellation/unchanged proof is not repurposed for runtime mutation.

Complete installed participants are retained and protected; startup admission remains
an INITIAL known-reader safe policy, not final full production functionality.
Ordinary manifest ELF OTA/fence restrictions are unchanged. SHM mutable data is not
snapshotted/restored. New boot currently refuses missing/recreated targets; refusal
is NOT normal reboot success. Native creation and broader business eligibility OPEN.

## Validation And Blocker

Prior checkpoints reported a 25-case private-file lifecycle pass and native handoff
build/pass; these are not reclassified as a full pass at the present hashes. The
coordinator independently passed two post-signature/terminal tests at its recorded
snapshot, covering FOUR promotion windows. Its exact evidence SHA is in the JSON;
these same two tests were not repeated here. They are not RSA/HTTPS or OS reboot.

The original missing-five command remains session 47230, Windows PIDs 80652/65848,
started 22:49:12. No stdout/exit status has returned; later read-only WSL ps/list also
blocked and ONLY those observation calls were cancelled. The original test handle
is retained. No shutdown, kill, replacement environment or duplicate test launch.
Its source-load point is unknown, so eventual output must not blindly certify this
later checkpoint. No test timeout is labelled a product failure or PASS.

Executed at this checkpoint:
- Windows Python AST parse with feature_version=(3,8), four Python files: exit 0.
- git diff --check: exit 0 (existing LF/CRLF Git warnings only).

Missing-eight focused cases and other missing validation are enumerated in JSON.
The fork wait-status assertion was made Python 3.8-compatible using WIFEXITED and
WEXITSTATUS; grammar success is not actual Python 3.8 runtime evidence. Existing
core40/legacy21 and native final-state are affected regressions not yet run here.

## Proposed Board Fixture Scope (NOT Authorized)

If WSL cannot be unlocked, independently approve a fresh private directory per board
for these eight files ONLY: deploy/offline-runtime-upgrade.py, runtime-upgrade-guard.py,
ota-health-result.py, gateway-services.sh, ota-rollback.sh, gateway-ota-recover.service,
tools/production_runtime_baseline_test.py, tools/fixtures/production-historical-r2-allowlists.json.
All Python dependencies are standard library; no pip, ELF, CA/key or download needed.
The files total 374942 bytes. Verify each deployed and copied byte against the JSON
before invocation. The two unchanged dependencies are also pinned in the JSON.

Root is required for private fixture file-ownership checks; do not bypass those checks.
Proposed launch: an outer timeout of 120 seconds (TERM, then KILL after 5 seconds),
RLIMIT_AS 384 MiB, CPU 60 seconds, file size 64 MiB, fd 128 and core size zero.
Use a verified TMPDIR under the unique run directory. Invoke ONLY the eight pending
test names in JSON, selected explicitly with unittest; never invoke the host CLI
apply/adopt/install/recover/boot or an entire historical regression suite.

Fixture /opt,/etc,/run,/dev/shm and synthetic /proc are remapped under its own temp
root. systemctl, curl, verifier execution and service properties are substituted
with commands that reject unknown calls. Boot recovery's dynamically imported
handoff module is similarly isolated. Actual kernel flock/fork and read-only own
/proc/stat, /proc/self/fd, /proc/locks are used; no business PID/fd/lock is touched.
The fake ELF headers are never executed. The process-death case exits only its forked
fixture child. Do not upload real identity/config/approval/state/keys to this fixture.

Before/after: bounded read-only business PID/restarts and already approved config,
active state and installed guard pins; report drift and stop further execution.
Inspect residual child processes and private file inventory after timeout/success.
Cleanup only after resolving the exact root beneath the approved unique namespace,
rejecting symlinks/path drift; never wildcard-delete /tmp or alter actual SHM/fences.
Preserve small stdout/exit/hash records locally. This tests a real board interpreter
and private file lifecycle, NOT business deployment, Broker, real OTA or reboot.
No SSH or execution of this proposed scope occurred in this stage.

## Native Boot Follow-Up

Source confirms tools/pointctl.cpp:261 constructs MemoryPointStore(shmName), while
stats at :389 only calls getStats. Default CreateOrOpen delegates to ABI11;
native creation holds PosixFileLockGuard, handles size-zero/unpublished initialization
and initializes the robust mutex. This is a usable lead, not an approved boot gate.
Still needed: fixed real AArch64 ELF/pin/ABI proof, approved logical v11-only list,
native tmpfs/memory capacity and directory bounds, exclusive no-holder creation,
startup order, per-boot durable identity proof, crash handling, and actual dual-board
normal reboot. Never create a retired name, use --app-config, synthesize SHM with
Python or treat old inode/old queue bytes as new-boot authority.
