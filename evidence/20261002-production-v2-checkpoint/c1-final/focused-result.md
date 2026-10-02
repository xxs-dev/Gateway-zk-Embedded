# V2 boot lineage local checkpoint

Scope: private Linux root fixture only. No board, build host, real systemd reboot,
OTA isolation runner, ARM build, DeviceInit refresh, or production deployment.

## Source and failure boundary

The BOOT2 v2 promotion wrote a transaction-qualified checkpoint and replaced
`current.json`, then an injected failure left `pending.json` uncleared. The
BOOT3 `production-boot` entry created a fresh native proof and called official
recovery. Before the fix, `restore` re-entered `promote` and rejected the
unchanged BOOT2 receipt as `production boot lineage receipt changed` (red
tool call `cd24a4`, exit 1). A second private fault injected failure after
pending unlink and before root-directory fsync; official recovery returned
`COMPLETED` without retrying root sync (red tool call `d4b3af`, exit 1).
These red calls were not saved as raw log files and are not counted below.

The fix validates the immutable candidate, health, current pointer, live
context and retained lineage/proof, then finishes an already promoted
transaction without regenerating its receipt. A completed explicit recovery
is idempotent and resyncs the production root even when pending is absent.
Tampered historical receipt is rejected on BOOT3. Neither old receipt nor old
native proof is deleted or rewritten by the test.

## Fixed inputs and raw results

All three final runs used identical pre/post SHA-256 inputs:

| File | SHA-256 |
| --- | --- |
| `deploy/offline-runtime-upgrade.py` | `bf4fe14375e6fc36be355427d79ff158ceb2ad4e2f1a7663b33664b1479f1b35` |
| `deploy/runtime-upgrade-guard.py` | `2c330256122c5f7745dfd3f0202a80724c352cfed8a7863c9ce2a366052071ff` |
| `tools/production_native_boot_test.py` | `d734f5358432b5b623a6213c3a4c1831c4db3430dfc17163c91ffd0d460ee071` |
| `tools/production_runtime_baseline_test.py` | `78a4fdf3d85b0db5d7cdcbdf79e6428041b08ec328860c3ae7b889d48d41bb9f` |

The exact `wsl.exe --cd ... --user root --exec python3 -B -m unittest -v`
commands, each selected test name, exit code, input hashes and raw output
hashes are in `final-focused.result.json`, `final-boundary.result.json`, and
`final-related.result.json`. Their corresponding `.stdout.raw` and
`.stderr.raw` files are separate, unedited process streams; the WSL startup
warning remains in stderr. The three exits were 0: 9 focused boot/recovery
tests, 1 sequential 127/128/129-promotion and step-130 failure/recovery
test, and 4 related baseline/code-only/legacy recovery tests.

An earlier `focused.*` run is retained with its own source hashes but predates
the root-fsync fix; it is not final-input evidence. The older merged
`existing-proof-boundary-draft.raw.log` remains a draft because interleaved
stdout/stderr damaged its opening text. No test was counted from that file.

## Remaining gates

These fixtures substitute the native constructor, service manager, kernel
paths and health endpoint. They do not prove power-loss durability, real
reboot behavior, full configured-role lifecycle, normal MQTT/direct/SCADA
config publication, ordinary logic delete/restore, ARM artifacts, or the
three-end production deployment gates. The WSL `/opt` historical-content
question remains unknown; this fixture did not invoke the real OTA runner or
mutate that path.
