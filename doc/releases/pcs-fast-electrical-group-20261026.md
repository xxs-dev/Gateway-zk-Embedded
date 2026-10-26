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
20 ms maximum or the PCS internal ADC/register update rate. Field results
will be recorded after the completed immutable evidence has been verified.
