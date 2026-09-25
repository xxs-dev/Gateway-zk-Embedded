# A joint inbound maintenance shutdown: local script regression

Base product commit: `d30e19ee5ed68f67672569d9c59c633c5e431934`.
Tests ran locally on 2026-09-25 through `tools/install_isolation_test.py`
with `--evidence <receipt> --test <method>` under WSL root private mount/net/IPC
namespaces. No device, ARM build, Windows package or host Gateway service ran.

- `red/result.txt` SHA256 `8433240e40f08af27f9404d0f6807b1cf3a6a72dcfe9a7d9127c3f4553fc6e4d`:
  2 expected failures: approved source remained inbound-enabled; unapproved
  source was accepted by apply.
- `green-initial/result.txt` SHA256 `3a508a69041580759968b6105d2831fda48afb10b7480ed9aa243adacaec0607`:
  intermediate 7-test run with 3 old-test expectation failures. Retained intact.
- `green-focused/result.txt` SHA256 `9d760cd0475b9bdde15e0541680e673f14b85d649c73bf25e8c3f47b12ea3380`:
  12/12 PASS. Covers approved true/true to false/false, observe, recover of
  original bytes/mode, no pin/wrong pin/non-A pin, mixed/null/legacy refusal,
  unchanged adjacent fields, existing A monitor-only and B default roundtrips.

The raw per-method logs, compile logs and native provenance JSON remain under
each receipt directory. Three generated native fixture ELFs were moved to
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-inbound-maintenance-20260925/native-fixtures/{red,green-initial,green-focused}.elf`;
each SHA256 is `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.
They are test fixtures, not release binaries. The completed runner wrote a
nonempty `result.txt` including `Ran 12 tests` and `OK`; AST parsing and
runner-tail readback were also checked after an intermediate truncated edit.

`aInboundDisableSourceSha256` is only an approval of the original exact A
monitor app bytes. It does not relax the explicit-false observe gate or alter
physical control. A real A transaction still needs separately approved current
config SHA and full stopped/mapping evidence. This stage does not qualify
activation or real-device maintenance observation.
