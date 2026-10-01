# Native Boot Validation Proposal

NOT AUTHORIZED. No connection, upload, board command, reboot or build performed.
23 first; no automatic transition to 24. Each following stage requires its own
fixed input/command scope and coordinator approval. Historical permissions are not reused.

## A: Fixed Linux Python Fixture

Reuse the already reviewed execute-board-fixture.py process-group/subreaper and
bounded baseline helper, not a new executor framework. The necessary future patch
only changes the eleven pinned inputs and explicit test selection below. The new
execute-board-fixture.py is prepared locally with a no-connection default dry-run. Present
runner's old eight-file manifest must NOT be used to execute these changed sources.
Exact inputs/bytes/SHA are in local-checkpoint.json; no pip or native rebuild.

Fresh namespace example /tmp/gateway-nativeboot-<checkpoint>-23-A1; verify absent,
root creates 0700, source read-only, TMPDIR exclusively <run>/work. Resolve and
reject symlinks before upload/copy/cleanup. Never place source under fixture-mapped
/opt. Hash each source and copied input against Git blob, not text newline conversion.

Explicit commands (run inside that source directory, not host tools):

```sh
python3 -B -m unittest -v tools.production_boot_publication_test tools.production_native_boot_test tools.production_boot_health_publication_test tools.production_runtime_baseline_test.ProductionRuntimeTest.test_boot_hook_resolves_durable_owner_tail_even_without_runtime_pending
```

24 publication/lock/fork + 18 native-boundary + 1 health publication + 1 affected
baseline boot-tail case = 44 (the earlier 42 plus the two requested no-handoff runtime
branches). No old eight-case run, core40, legacy21, D2/D4, MQTT or
full historical suite. Root is required for real private-file ownership and flock.
The constructor/systemd/proc/network boundaries remain substituted, even on ARM.
Nothing invokes host apply/adopt/install/recover/activate/production-boot.

Suggested bounds: wall240s, TERM process group then KILL after5s; parent subreaper
reaps fork descendants and reports unresolved tasks before exit. AS384MiB, CPU180s,
file size64MiB, fd128, core0. stdout/stderr combined2MiB; guard output <=1MiB.
Timeout/error/drift stops the stage, no automatic replay or cleanup of evidence.
Retain source, original output/status/pins. Only the resolved owned work tree may
be deleted after all child processes are proved absent, then repeat the bounded
read-only baseline AFTER cleanup (not the older before-cleanup comparison).

Before/after: unchanged identity/bootId, business PID/exe pins/startTicks/NRestarts,
approved config, active/state/installed guard pins, actual SHM names/dev/inode/header,
holders and finite resource sample. Never recursively hash historical OTA databases.
The existing approved helper must remain bounded and unchanged unless a specific
small after-cleanup invocation adjustment is separately reviewed.

## B: Actual Native Creator In Private Mount Namespace

Separate approval, only after A is accepted. Reuse sealed cb45 pointctl: 674504B,
SHA f6f4a0a94d55827e874e45b9484daf8e9af74bae60ac048e7416ba433881c813.
Rehash archive/extracted binary; inspect exact dynamic-loader/library dependencies
on 23 read-only before execution. Missing dependency is a blocker, not permission
to install packages or use LD_PRELOAD/insecure substitutions.

One child-owned mount namespace, propagation made private BEFORE mounting a small
private tmpfs over its own /dev/shm view. If unshare/mount support or namespace proof
is absent, stop and report. Parent and business namespace mount IDs/SHM identities
must remain unchanged. No real /opt config/state/guard is mounted writable.
Pin at most two explicit private logical names; no retired/business name, wildcard,
--app-config, write/pending/control command or another business daemon.

Run only fixed pointctl stats --shm <approved-private-name>, sequentially under the
private offline lock and actual capacity check. Bound each subprocess15s, AS256MiB,
CPU15s, stdout/stderr4KiB, core0. Record real ABI11 size/header/mode/identity, empty
queues, native getStats success and same-boot reopen identity. An approved constructor
interruption case must be confined to this namespace and use the identical CLI;
do not fabricate a robust mutex/layout in Python. Actual native failure/partial
initialization refusal/reentry belongs here, not the Python fixture proof.
Exit/reap namespace children; its tmpfs disappears with namespace ownership. No
unlink/rename of any parent business SHM. Keep small binary/hash/output evidence.

## C: Actual Systemd Ordering, Private Units Only

Separate fixed scope/runner review required; NOT part of A or B. Use a unique
gateway-native-fixture-<id> unit namespace with an exact service-file/path inventory,
root-only private configuration, and bounded fake reader workload. Prove real
Requires/After gate ordering, asynchronous worker start, gate completion before
reader start, timeout/refence and dead-owner retry. Keep product gates/units/state
untouched; no production launcher or daemon. Any necessary daemon-reload and exact
private unit removal require explicit approval. No broad stop/reset-failed/mask.
This proves manager ordering, not full product/native/business reboot behavior.

## D: Actual Business And Reboot Gate

Only after implementation review and A/B/C acceptance: separately build/sign the
new actual MQTT/Monitor/full paired B0/B1 package, fix D1 raw-source/pair resolution
factory boundaries, pin all configured participants and official admission/rollback.
The old cb45 bundle is NOT a substitute for new business artifacts or full dependencies.
Initial reader-safe startup is not the final EMS/SCADA/driver production scope.

Then independently approve 23 baseline/adopt -> changed same-ABI install -> health
promotion/recovery -> serial real OS reboot -> native SHM proof -> actual PID/maps/
stable health. Persistent identity/point tables/network/TLS remain pinned; original
records/fences and v10 lineage are preserved. Reboot clears volatile sources by OS,
never by manual unlink or queue rewind. Only after 23 acceptance propose independent
24 scope. Real RSA/HTTPS signing/download and original-job OTA terminal recovery,
receiver outage/reconnect and continuous main acquisition need separate actual
evidence; private fixtures cannot satisfy them. No remote scope is granted here.
