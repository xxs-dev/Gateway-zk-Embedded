# PCS Fast Electrical Group

## Profile

The existing coalescing driver (`db5c618`, binary SHA256
`5e9aad585e5a12ef243d1565f6cf29df72c3691f0b6e6a117e583d040b7f6ac2`)
already supports this separation. No production C++ change or binary
replacement is needed for the first bounded validation.

`tools/comm108_fast26_profile.py` creates a separate stopped/read-only test
configuration without connecting to the PCS or modifying its input file:

- FC04 registers 201-226, point indices 10021-10046: priority 0, period 20 ms.
  The 26 voltage/current/P/Q/apparent-power/frequency/power-factor/input points
  fit one existing 32-register read limit, with no address gaps.
- Status indices 10010-10014 and Q setpoint feedback 10266-10268: priority 1,
  period no slower than 1000 ms. Electrical write permissions stay disabled.
- Other original priority-0 telemetry: priority 2, minimum period 5000 ms.
- Other enabled telemetry: priority 3, minimum period 5000 ms; longer existing
  periods remain unchanged. No readable point is disabled.
- Request pacing 20 ms; realtime/background budgets each one task per slice
  in both normal and focused modes; background slot interval zero.

The generator refuses enabled writes, startup writes, changed/missing fast
register mappings, wrong point coverage or inadequate existing batch caps.
This profile is specific to the verified COMM108 point table, not a general
automatic tuning policy for running converters.

## Local Verification

`python tools/test_comm108_fast26_profile.py` passes two tests covering
scheduling-only changes, unchanged input, preserved slow periods and refusal
of writes, startup actions, missing points, mapping drift and small read caps.

The collector regression models the 26-register fast block, two 1 s safety
blocks, seventeen 5 s telemetry blocks and forty-nine 60 s slow blocks.
With a fake 20 ms request gap and 1 ms response, both normal and focused modes
produce a 21 ms median / 41 ms maximum fast-block gap and a 1055 ms maximum
safety-block gap over 70 simulated seconds. Every slower block updates at
least twice. The collector and daemon realtime-priority CTest targets pass.
These synthetic results are not hardware measurements.

## Field Procedure

Local evidence directory: `D:/workspace/new/comm108-fast26-20261026-1720`.
Remote directory: `/opt/modbus-gateway/diagnostics/pcs-fast26-20261026-1720`.
Use the existing identity-guarded SSH helper and exclusive PCS driver only.
Warm up all 440 points after restart, then measure 180 seconds. Keep the
existing 3 s safety freshness limit, pending=0, electrical writes disabled,
capture health and CPU/resource bounds. Keep EasyTier/4G services and other
configuration hashes unchanged. Restore the exact original PCS configuration
on completion or failure, with bounded service runtime and no automatic retry.

The test harness preserves the after-restart freshness cutoff and restoration
marker; three mocked tests cover cutoff ordering, restoration idempotence,
foreign-config refusal and abort-without-retry after a failed warmup.

At 20 ms pacing, other requests necessarily consume slots, and responses can
overrun. This targets typical fast-group refresh near 20 ms, not a hard
20 ms maximum or the PCS internal ADC/register update rate.

## Field Results

The single 180.303 s measurement completed on 2026-10-26, approximately
17:26:04-17:29:04 Beijing time. No stage was retried. The deployed driver
binary was unchanged; this test used the scheduling profile above.

| Metric | Observed result |
| --- | --- |
| Complete fast-block requests | 6508, always FC04 / start 201 / count 26 |
| Matched fast replies | 6507; last request is at capture end |
| Fast-block average rate | 36.17 requests/s |
| Recorded request gap | median 23.535 ms; P95 40.994 ms; P99 43.311 ms |
| Recorded reply gap | median 23.818 ms; P95 41.970 ms; P99 47.190 ms |
| Recorded request/reply latency | median 1.356 ms; P95 4.695 ms |
| All-request throughput | 43.22 requests/s |
| Driver CPU, percent of one core | median 13.41%; P95 14.59%; max 18.96% |
| Driver maximum RSS | 28468 KiB |
| Maximum observed safety-point age | 1083 ms, below the unchanged 3000 ms limit |
| Enabled points updated in window | 440/440 |
| Full-table sweep observations | first 60.060 s, next 60.259 s |

The full-table sweep includes existing 60 s points. Prioritizing the fast
block deliberately reduces other telemetry to at least 5 s; it does not
make the entire table faster. Safety status and Q setpoint feedback retain
their separate <=1000 ms configured periods. No invalid/stale quality was
observed. There was one TCP stream, read-only FC01-FC04 traffic, no observed
Modbus exceptions/retransmission/lost-segment/malformed flags, and zero
reported capture kernel drops. These are bounded observations, not a
long-term reliability or maximum-rate certification.

### Timing Limitations

The passive recorder uses `time.time_ns()` after userspace `sock.recv()`;
it does not use kernel receive timestamps. Every 0.5 s it synchronously
flushes/fsyncs and writes capture health. Recorder scheduling or storage
stalls can therefore inflate recorded latency/gaps and then bunch buffered
packets together. This limits the interpretation of all PCAP timing
statistics, including the typical values above.

All outliers remain in the analysis: maximum recorded request gap 457.963 ms,
reply gap 497.390 ms and request/reply latency 457.900 ms. There are also
43 recorded fast request gaps below 1 ms (minimum 0.127 ms). Around frames
12531-12540 a long gap is followed by bunched request/reply sequences. No
Modbus packet has a TCP timestamp option, so sender timestamps cannot
resolve the ambiguity. These maxima are not established PCS response
delays or verified acquisition deadlines. Kernel-timestamp capture would
be needed for stronger worst-case timing evidence; no extra field test or
capture-service change was performed to obtain it.

The 100 ms pointctl observer skips fast frames. Of 1543 observations, 1378
have equal timestamps for all 26 points; reads may straddle a shared-memory
update, so this is not an atomic 26-point snapshot guarantee. Stopped-state
total P did not change. Neither these timestamps nor the polling rate
establish the PCS internal ADC/register refresh rate.

Conclusion: this profile supports typical recorded fast-group intervals
near 24 ms with an average near 36 Hz, but does not establish a strict
20 ms / 50 Hz guarantee. It was tested temporarily, not left active.

## Restoration And Evidence

Exact original configuration and its original 50 ms request pacing were
restored at 17:29:08.090 Beijing time. Configuration SHA256:
`6f6718d260d20b3da38c5afb0a074c7d69b8e8015694bd037b15ba47fa6cb2e3`.
The dedicated probe service is inactive/dead with Result=success.

A separate read-only final check at 17:35:24.627 confirmed the same restored
driver PID 4000002 active/running, fresh stopped feedback, zero Q setpoints,
pending=0, writes disabled, exclusive PCS connection, healthy 440-point
general evidence and zero capture drops. Other configuration hashes,
EasyTier/4G/network/watchdog process identities and default route matched
the baseline. There was no START or electrical-control restoration.

The immutable local `evidence.tar.gz` is 862126 bytes, SHA256:
`b75623d7e8d539ec6081aa5f9f52490dee5069e0c70b37496dffe3e397f12ca8`.
All 23 manifest entries were extracted and locally reopened/hash-verified;
uncompressed size is 25568852 bytes. The archive was exported only once.
The separate `final-check.json` was downloaded and reopened/hash-verified:
`1df37c81fdbfec9c03f3dea9fd0ba91028cdc572cb275952ce35b27f876f8f7e`.
It also preserves the recorder source and its SHA256 for timing provenance.

Local analysis files are `analysis/summary.json`, `fast26-summary.json` and
`analyze_fast26.py` in the evidence directory above. The common analyzer's
old 24-point CORE group is not used for the new 26-point cadence conclusion.
The previous 24-hour trial and its immutable evidence were not changed.
