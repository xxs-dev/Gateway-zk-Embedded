# R10 L3 Build-Only Local Review

This is a LOCAL_ONLY checkpoint, not an execution authorization. No SSH,
credential loading, build-host/board command, B1/L2/P2 replay, or push occurred.
The L2 runner, manifests and prior receipts remain unchanged. Product source is
the exact commit `eb6cc533ce730d6ff929d05a362070c18f455f6e`; L3 does not
change business code.

## Frozen Inputs

- Manifest SHA256: `db53bd32fd1a782b7855aeeef8c36f89ca95d9c8d7e4bbda27b5077da3d1f94e`.
- `git archive` SHA256: `15fc9c2457f953f4fd854fca2acea41937cf98d1ea1d8b6d9635c296fd4662d9`,
  8,151,040 bytes. Its commit ID and 442 regular members were checked against
  exact Git blobs. No `evidence/` member or key/credential-looking path is in
  the source archive. The supplemental `ownership-probe.cpp` is separately
  taken from the same fixed commit and pinned in the manifest.
- Exclusive proposed root: `/srv/build/gw-r10-eb6cc53-build-L3` on
  `192.168.22.11`. It has not been checked remotely for occupancy yet.
- Existing L2 `build-child.py`, `build-group.py`, and environment inspector
  are SHA-pinned. A small L3 wrapper binds only the new root; the build child
  still enforces its L2 namespace, timeout, receipt and return checks.
- Four CMake targets: `SystemMonitor`, `diagnostic_process_test`,
  `system_monitor_diagnostic_test`, `system_monitor_service_test`; separate
  test-only `ownership-probe`. Exactly five AArch64 ELFs are expected.

## Local Verification

`python -B evidence/20261002-r10-linux-L3/prepare-l3.py --freeze` exited 0.
`python -B evidence/20261002-r10-linux-L3/execute-l3.py` exited 0 and reported
`connects=false`, `remoteResultAvailable=false`, `executionAuthorized=false`.
`python -B evidence/20261002-r10-linux-L3/L3_runner_test.py` exited 0, six
tests. They cover fixed archive/target identity, no-auth default admission,
pre-auth drift/pin rejection, missing execution pin, remote command binding,
and SFTP parent type/owner/mode rejection. This is a Windows local API/model
regression, not Linux namespace or AArch64 behavior.

Runner SHA256: `db20fa1d83ae3e619f1fcbc98c07812445e50899e93ea7f0ae6e66e458f29f74`.
Preparation SHA256: `4522939b93a4598f80569888c5c23035e40c2ac1f9ad18eae32a31963a591a5b`.
Environment wrapper SHA256: `fc4a12db6ece5b1bd3cec2b0e0904e86a7194e3828f05dfc9a6180c98b43ad18`.
Test SHA256: `b71793a7b4adf3efab0a909e3d69057f95b074665c55081f629654fa7118765e`.

The next step requires an independently approved exact L3 manifest SHA and a
fresh read-only 22.11 preflight before any upload. Do not treat the local
dry-run, old L2 build or WSL diagnostic PASS as a new AArch64 build, board
behavior, business deployment, OTA, or three-end acceptance.
