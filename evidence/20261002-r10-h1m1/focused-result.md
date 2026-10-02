# R10 H-1/M-1 local result

Scope: local diagnostic request admission and reply semantics only. No ARM build,
device, live Broker, deployment, or production restart was exercised.

The handler now parses strict JSON (including escaped Unicode) and rejects
ambiguous identity fields before execution. A non-admitted request publishes an
`admission_rejection` without a terminal `cmdId`; it cannot assert that a later
delivery will not execute. A cached terminal is replayed before the freshness
gate. New admission requires `requestTs` within the effective wall-time window.
The wall-time floor uses a stable steady-clock anchor, retaining submillisecond
elapsed time across requests. Terminal records remain for ten steady minutes,
covering the five-minute past-age plus five-minute future-skew interval while
the process is alive. The cache remains bounded at 16 records/8 MiB and does
not evict an unexpired terminal to admit a new ID.

The `system_monitor_diagnostic_test` private Linux fixture passed after the
final source change. It covers the actual service loop, escaped Unicode and
malformed identity, nonterminal busy/cache-full replies, a service-object
recreation, a controlled wall-clock rollback, 1000 submillisecond steady-clock
steps, and the future-edge/ten-minute relationship. The service-object test is
not an OS reboot or persistent idempotency proof. `system_monitor_service_test`
also passed, but before the final journal-tail compatibility change; the final
diagnostic run covered that change. The first WSL attempt did not enter any
test because its temporary build ELF was unavailable; its failure log remains.

Raw local outputs (SHA-256 of working-file bytes):

| File | SHA-256 |
| --- | --- |
| `edge-diagnostic.raw.log` | `0bbcd4b2afd4de79a0ee3c8b9b247f90ec4f513ee87fa927a72e7bf57e7af4d2` |
| `edge-service.raw.log` | `2f6e1d3c9276e7c855174b81925347a4fb720ed3bd33d6929af8edf811a5421d` |
| `edge-attempt1-no-elf.raw.log` | `eb5b3f71cf1f7572c48b628683b898b09ee577c6dbb249cbc8397dd4ef127dfc` |

The three original log members are preserved in `raw-local-outputs.zip`,
SHA-256 `edfa09517009e190b150f80dd01dba4d277f5993e313790f1614860dcd750f9f`.

Compatibility: an older edge does not emit this admission-only contract or
enforce the new `requestTs` requirement. Upgrade edge before relying on the
new Windows consumer behavior; quiesce/review in-flight diagnostic requests
across mixed-version upgrades. The in-memory cache is not durable: after an
edge process restart, an unconfirmed ID may execute on redelivery. Clients and
platform must report UNKNOWN until a trusted terminal arrives, not infer a
definite refusal or cleanup. Durable exactly-once semantics, actual Broker
ordering, ARM build, and dual-board behavior remain open.
