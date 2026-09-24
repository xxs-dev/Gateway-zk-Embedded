# S2 CPU diagnostic boundary

## Immutable device observation

Read-only inputs reside under
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/sustained-s2-20260924/`:

- `result.json`: SHA256 `04742f14de4e9ad0de20eb7ca22b76407435d10d74c5d509defec9adf8880e15`.
- `A.jsonl`: SHA256 `a9ed5bddd76b26155f838ca10d98a9f1827bb783d9e4c7a7d8fccd0d7670b945`.
- `B.jsonl`: SHA256 `e8939bf7b5c24218771808f0f0589f3b25767703f2d281741d3faf93e1690fa3`.

The first A window spans 1.043256965 seconds at HZ100. Its 55 ticks comprise
Coordinator34, Compute8, helper5 and worker delta8. Total is 52.7195% of one core;
products alone are 40.2585%, helper plus worker12.4610%. Newly spawned children
are correctly counted from zero. This is not evidence that products alone exceed50%.
Coordinator exists for about0.933 seconds of this window, including initialization.
S2 remains FAIL; its original `FAIL_CLEANUP_UNCONFIRMED` receipt is not rewritten.
There is no steady-state CPU sample or per-function ARM timing in this receipt.

## Ranked hypotheses and predictions

1. Persistent missing points trigger repeated100000-slot scans. Batch reads using
   the existing API should reduce cost, without inventing feedback or caching misses.
2. Each health sample recompiles three number regexes and, when fresh, one bool
   regex. Reusing identical compiled expressions should reduce parsing CPU.
3. Startup and test-process overhead contribute to the first rate window. Product
   and helper/worker separation should reduce the attributed product total; it does.
   The available evidence cannot separately time startup configuration, attach,
   consensus, network initialization or individual steady-loop functions.

Transport poll50 is a maximum blocking wait, not a guaranteed50ms sleep. Available
network input may wake it early. Status logs at200ms cannot establish loop frequency.
Do not infer20Hz or multiply local timings into the ARM34 ticks as a causal proof.

## Local experiment

`run_s2_cpu_benchmark.py s2-cpu-primitives` builds only the diagnostic translation
unit, reusing the unchanged native Debug library SHA256
`d9c02f29e4fa2c830db033ccefc038ca12315c260318e9b9a213f77115226ad2`.
No CMake rebuild, runtime binary replacement, ARM build or device connection occurs.
The test includes the original main translation unit only to call its actual
jsonNumber/jsonBool helpers; its renamed coordinator entrypoint is never executed.
It runs in private mount/net/IPC/PID namespaces with private SHM. Disposable binary
and namespace are removed at exit. Logs and results are fsynced and read back.

Sources match qualified9ccfce8 for main, bridge and store. A GNU linker wrapper
counts actual bridge getLatestByIndex calls in an untimed probe. Counters are off
during timing, though the same wrapper remains on all single-call paths. Five
warm-up iterations precede each series; three finite repetitions are retained.
The fixture is a sparse private segment with initial diagnostic publication and,
in stage2, only helper-whitelisted inputs. It never supplies feedback724040..45.
There is no live writer contention, representative high occupancy, socket traffic,
disk I/O or scheduler pressure. CPU is CLOCK_PROCESS_CPUTIME_ID, not wall time.

Actual calls per sampling pair:

| State | Capability reads/misses | Target reads/misses |
| --- | ---: | ---: |
| Before helper inputs | 16/16 | 1/1 (short circuit) |
| Helper inputs present, feedback absent | 16/6 | 6/0 |

These are measured per-call frequencies in the fixture, not observed ARM loop Hz.
On a warm cache, every single missing lookup performs a full scan. The existing
batch API coalesces uncached missing lookups; invalidated previously cached slots
can still take fallback scans, so this is not a universal one-scan guarantee.

Median process CPU microseconds, x86_64 GCC15.2, original library Debug/no-O and
diagnostic TU-O0:

| Operation | Before helper | Helper present |
| --- | ---: | ---: |
| Actual bridge capability + target | 1274.837 | 477.125 |
| 16 capability reads, single primitive | 1180.995 | 478.317 |
| Same16, existing batch primitive | 285.876 | 286.735 |
| All22, batch primitive only | 280.413 | 299.580 |
| Six present target reads, single | n/a | 14.048 |
| Six present target reads, batch | n/a | 7.886 |

| Health parsing | Construct + search | Reuse same patterns |
| --- | ---: | ---: |
| Absent health, three regexes | 98.221 | 0.951 |
| Fresh health, four regexes | 131.642 | 11.053 |

The single-vs-batch16 comparisons reduce measured primitive CPU by75.8% and40.1%.
Batch22 is not an implemented bridge and omits its validation work; it must not be
reported as an end-to-end product speedup. Regex control preserves patterns,
fallback values and conversion and checks equal outputs on its two fixed inputs.
This is not a full malformed-JSON or parser regression suite. Primitive equality
checks cover index/value/quality/stale/ts/expiry/identity before and after TTL expiry.
It does not yet qualify a rewritten bridge or authority-epoch behavior.

## Narrow fix recommendation, not yet implemented

First change only capability sampling to a single existing batch16 snapshot per
invocation. Map results by exact index, preserve missing values, defaults, TTL,
quality, finite/future timestamp checks and capability readiness predicates. Keep
station-target short-circuit behavior unchanged initially. This captures the main
missing-feedback improvement without changing the shared store or main-loop API.
Do not retain a snapshot across invocations or add negative caching. A combined22
snapshot is an optional later scope requiring explicit ordering/semantics tests.

Separate small follow-up: precompile the four fixed health regexes once, preserving
current parsing and fallback behavior. Do not mix in a new JSON parser, changed
health TTL, sampling throttle or relaxed CPU limit. Constructor cost moves to
startup but repeated construction is removed; first-window ARM gain is unmeasured.

Before either product change: pin scope, reproduce bridge semantics at its public
call site, then verify missing/late writer, TTL boundaries, bad quality/nonfinite,
future timestamp, identity/index mapping, target short circuit and unchanged
authority/epoch behavior. Native local tests precede any separately authorized
ARM candidate. S2 acceptance still requires a separately approved actual-device
run under the unchanged resource gate; this diagnosis alone cannot unblock S2.
