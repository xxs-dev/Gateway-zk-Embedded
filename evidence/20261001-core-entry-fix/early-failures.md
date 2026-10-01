# Early Test Results

These retain the observed non-passing stages, not claims about the frozen diff.
No early fixture error is counted as a product regression or a successful test.

## Product Red

Before changing cb45f3f deployment code, the new core manifest/three-reader
roundtrip failed at read_inputs -> activation_script_pins, exit1:

```
ValueError: activation startup scripts require exact two-file approval
```

The first invocation also discovered the imported AcquisitionTest class and ran
its21 historical tests, all passed. The new test module then used a module alias
so its suite contains only the narrow core tests. The original separate Linux
candidate gate mismatch is recorded in coordinator implementation evidence and
is not duplicated as an ARM/device result here.

## Fixture Error

The first12-test expansion exited1:11passed and1fixture error. A synthetic
systemctl metadata helper tried reading a deliberately dangling998-shm.conf,
before the real recovery file gate could inspect it:

```
FileNotFoundError: .../system/etc/systemd/system/system-monitor@monitor-service.service.d/998-shm.conf
```

The narrow test metadata stub was corrected to report the inactive unit without
opening its broken drop-in. The actual production recovery path then rejected
the symlink with ValueError and did not report RECOVERED_STOPPED. The separate
old-backup symlink case also passed. No production gate was weakened to make
this fixture pass.

## Green

Single tracer roundtrip:1test exit0. Next focused suite:14tests exit0/15.367s.
Before the user freeze message,4further requested safety-boundary checks were
added:open source FD, same-byte source replacement before activation, excluded
participant late start/override, and ordinary OTA fence retention. Final narrow
suite:18tests exit0/20.230s. Legacy21tests exit0/2.793s. Five existing focused
private-namespace regressions exit0/6.907s. These are local simulated file
transactions, never device migration, actual MQTT, actual OTA or sensor truth.
