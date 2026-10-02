# R10 L3 Build Execution

The single authorized invocation on 2026-10-02 used local session `58093` and
returned exit 0. It consumed the L3 scope. The local receipt reports preflight
exit 0, remote exit 0, phase `BUILT`, five returned artifacts hash-verified and
SSH closed. No second invocation or retrieval connection occurred.

The isolated root was `/srv/build/gw-r10-eb6cc53-build-L3` on `192.168.22.11`.
The fixed source was `eb6cc533ce730d6ff929d05a362070c18f455f6e`, manifest
SHA256 `db53bd32fd1a782b7855aeeef8c36f89ca95d9c8d7e4bbda27b5077da3d1f94e`.
The existing Allwinner Linaro GCC 6.3.1 and CMake 3.25.2 were used. Preflight
recorded MemAvailable 6,485,106,688 bytes and free disk 108,647,854,080 bytes.

Both bounded compile commands exited 0: `build_edge_aarch64.sh --jobs 2` for
the four fixed targets, then the separate `ownership-probe` compile. Each
private compiler namespace had no surviving process at cleanup. All 442
isolated source files matched their pins after build. The shared tree's owner
identity, HEAD `746169f3ffcbe24c631f83eb3c6860622b664de0` and Git path-status
hash `b9f7824c79edc0e8257623d3f5042e0a75ec45b2ecc5d77b0391dfaa628deeb8`
were unchanged; that does not establish whole-tree content equality.

The five returned files independently matched receipt sizes/SHA256 and
ELF64 little-endian AArch64 headers. Each pinned readelf log records
`/lib/ld-linux-aarch64.so.1` and its dynamic dependencies; target-device
availability has not been tested. `summary.json` records every returned hash.
The unmodified raw local/remote receipts, build logs, readelf logs and five
ELFs are archived as 14 verified original-byte members in `raw-evidence.zip`
(3,640,355 bytes, SHA256
`a2417696c5edd980528eec631a6177c661a35a3d6aab8bbb239d49aba89453cb`).

No compiled executable was run. There was no board connection, installation,
service start, package/publish/clean action, OTA, or business acceptance. A
new independently reviewed board scope is required to test the missing proc
children interface and owner-death behavior on AArch64; L2/B1/P2 scopes remain
consumed.
