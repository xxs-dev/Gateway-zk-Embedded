# Shadow Acceptance Helper (Not a Product)

Linux-only, explicit target `ems_shadow_live_helper`, excluded from the default
build and all install/package lists. Links the same-source `edge_gateway` static
library, pthread and rt; no new third-party dependency. C++14 compatible.

```sh
cmake --build <existing-owned-build> --target ems_shadow_live_helper --parallel 1
<pinned-helper> --shm ems_shadow_20260924_pair --exclusive-inputs --duration-seconds 600
```

The explicit duration accepts integer seconds from 1 through 2100 (at most four
digits). There is no implicit duration or renewal. A 30-minute observation must
fit within the pinned total duration, including startup allowance. The existing
SIGALRM stop remains active; no helper restart is equivalent to continuous operation.

The CMake target must first exist in the approved source/build configuration.
This does not authorize remote compilation or device execution.

## Preconditions

- Only the literal SHM name above is accepted, without a leading slash. Its POSIX
  name is `/ems_shadow_20260924_pair`. It must already exist as ABI11. The helper
  never creates, unlinks, resizes or migrates a segment and has no default name.
- Use only the authorized isolated app. Physical routes must be absent; all graph
  outputs latestOnly, maxWritesPerScan=0. The helper is the sole writer of the
  selected synthetic inputs. Remove matching expression/Graph target writers.
- Linux OFD record locks must be supported (Linux 3.15+); a second helper or an
  unsupported lock fails closed. The lock does not interfere with runtime flock.
- The parent supervisor must continuously drain stdout/stderr, enforce timeouts
  and own the helper lifecycle. This is a pipe child, not a network service.

## NDJSON Protocol

One command per line, at most 4096 bytes, 20 commands per second. Strict fields,
unique indexes, finite values. Bad input exits 2, reports on stderr, stops refresh;
no success response is emitted for a rejected command. EOF, quit or termination
stops refresh without clearing or zeroing values. Old values expire naturally.

Startup emits `{"ok":true,"op":"ready","targetsPaused":true}`. No input is
written until set; targets remain paused until explicitly resumed.

```json
{"op":"set","values":[{"index":724000,"value":1},{"index":724020,"value":50}]}
{"op":"set","values":[{"index":724060,"value":18},{"index":724061,"value":18},{"index":724062,"value":18},{"index":724063,"value":6},{"index":724064,"value":6},{"index":724065,"value":6}]}
{"op":"resume-targets"}
{"op":"sample"}
{"op":"pause-targets"}
{"op":"quit"}
```

Set accepts 1..16 rows, validating the entire batch before staging it. The whitelist
is 724000, 724020..724028, 724060..724065. Enable/ready/interlock/override are 0 or 1;
SOC is 0..100; capacities are nonnegative. Set changes are written on the next
200ms refresh, not at acknowledgement. All configured active inputs get quality1,
wall-clock ts and expireAt=ts+1000, even if the value is unchanged. Resuming requires
all six targets to have been set. Pausing only targets leaves enable/capacity/SOC
refresh and the separate real ComputeEngine health/sink process running.

Set/pause/resume/quit acknowledgements contain `ok:true`, the same `op` and
`targetsPaused`. Sample contains:

- `ok`, `op:"sample"`, `shm`, `sampledAtMs`, `targetsPaused`, `pendingWrites`.
- `inputs`: the 16 whitelisted inputs; `points`: the nine sink indexes 725000..8.
- Present point: `index,present:true,value,finite,quality,ts,expireAt,stale`.
  Missing point: `index,present:false`. Nonfinite observed values are represented
  by `value:null,finite:false`, never silently replaced with zero.
- Numbers use classic locale and `max_digits10`. Times are integer milliseconds.
  Quality and expiry are preserved from the point-store API. A sample is not a
  transactional snapshot across all inputs, sinks and health/status files.

The helper neither manufactures health/authority nor writes diagnostics/sinks.
It never submits or drains commands. A nonempty pending queue, changed segment
identity/size, fresh preexisting input when first claimed, or observed external
change to a helper-owned input stops it. The writer check is diagnostic, not an
atomic fence against an uncooperative process; the reviewed single-writer app
manifest remains mandatory. Restarting a helper requires previous input TTLs to
expire before claiming them. Segment lifecycle must remain frozen while running.

For the separate first-dispatch Graph startup phase, use a fresh helper session,
set only enable/capacity/SOC, leave targets paused and never set/resume them. The
six Graph switches own 724060..65 in that phase. Do not run the numeric/TTL feeder
concurrently with this graph or transfer targets back while the graph is running.

## Narrow Tests

`ems_shadow_live_helper_test` is opt-in with
`-DGATEWAY_BUILD_EMS_SHADOW_HELPER_TESTS=ON` and BUILD_TESTING enabled. Run it in a
private mount/IPC namespace with a fresh /dev/shm. It skips 77 if the fixed segment
already exists. It never replaces an existing test/production segment.

Coverage: name/index/field/domain rejection; missing segment not created; exclusive
helper lock; set atomic validation; pause preserves target timestamp while other
inputs refresh; resume and live changes; double round-trip; metadata and empty queue;
no authority creation; foreign writer detection; lost SHM not recreated; actual
pipe protocol, timer refresh and EOF exit. Runtime and ABI are unmodified.
