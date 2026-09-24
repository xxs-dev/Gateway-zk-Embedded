# Monitor Drop-in Precedence

Base product commit: `4fde07950743a5b3d56ad11c6911e76fbcb3014c`.

Systemd 259's local unit loader, invoked by `systemd-analyze verify` with a disposable `SYSTEMD_UNIT_PATH`, reports the template `90-offline-probe.conf` as `Skipping overridden file` and exposes only the instance `90-offline-probe.conf` plus `998-shm.conf` as effective `DropIn Path` entries. No Gateway or host service was started or modified. The full loader output is `systemd-dropin-precedence.log`, SHA256 `fe51a69eabee683d8bc0aacf79fb3aa98918d99eb14bd7840b04dced25699e34`.

- Corrected fake-systemctl before the product fix: 2 focused tests, loader PASS and B observe FAIL on `monitor default effective drop-ins changed`. `red-result.txt` SHA256 `9082945c3202bb2125bdce7f13bfed793a86910a02f941c0fa56674516755f0d`.
- After the one-path fix: 6 focused tests PASS, including actual loader, B observe, extra/changed drop-ins, changed binding, and old-default stop-only refusal. `green-result.txt` SHA256 `aef717d4e1170c832182492b0334b9f6cef3d49de4c9bc49436234f453e49b5b`.
- An initial probe run also failed because its disposable unit lacked `DefaultDependencies=no` while using an isolated unit path. That original result remains at `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/edge-systemd-dropin-precedence-20260925/red/initial-result.txt`; it is not the product red receipt above.
- The unchanged native migration fixture SHA256 is `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0` and remains outside Git. Final script SHA256: `5df78c6c3654c5286823eac5732103e303dbb3a1031121b722dfaf9dff13de71`.

Both physical `90-offline` files still pass the existing byte checks. Only the effective `DropInPaths` expectation changes. This is a local systemd-loader and fake-systemctl regression, not ARM or device acceptance.
