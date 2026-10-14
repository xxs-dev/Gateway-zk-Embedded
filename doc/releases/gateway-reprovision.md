# Stopped-Only Whole-Image Reprovision

`deploy/gateway-reprovision.py` supports explicit replacement of the entire
`/opt/modbus-gateway` installation on an AArch64 device. It is a separate
maintenance operation, not ordinary OTA, same-inventory upgrade or SHM migration.

## Contract

- Obtain explicit authorization for full program/config replacement and downtime.
  Verify the original backup and its external activation-authority dependencies.
- Stage the helper, paired `offline-runtime-upgrade.py` and
  `runtime-upgrade-guard.py` outside the installed home. Record their source commit
  and SHA256. Never change this helper set during an unresolved transaction.
- Review a clean, root-owned candidate image and complete gateway service payload.
  Candidate `data`, `run`, `ota` and `tmp` cannot contain saved runtime work.
  Device identity bytes and permissions must remain identical. This tool checks
  artifact integrity, not the business correctness of the new configuration.
- Live home, journal `/opt/gateway-reprovision` and systemd unit directory must
  share one filesystem **and mount**. Separate bind mounts are rejected before
  stopping services, even when their device numbers are equal.
- Only known gateway service files and their individual drop-ins are replaced.
  Shared systemd policies are preserved. Dormant instances are included.
- The operation leaves durable `STOP` and per-unit condition fences. Neither
  install nor recovery starts/enables a service. Do not manually remove these
  fences, reset legacy guards, or restore/delete shared-memory segments.
- Activation, physical output, reboot acceptance and power-loss qualification
  are not implemented or implied by this tool. User authorization for program
  replacement is not permission for physical actuation.

## Commands

Run from the committed, verified maintenance helper directory as root. The
candidate and unit paths must be outside the live image and journal.

```sh
python3 -B gateway-reprovision.py plan \
  --image /opt/reviewed-candidate/image \
  --units /opt/reviewed-candidate/units \
  --transaction reviewed-transaction-id \
  --expires-at APPROVED_UNIX_EXPIRY \
  --output /opt/reviewed-candidate/approval.json

python3 -B gateway-reprovision.py apply \
  --approval /opt/reviewed-candidate/approval.json \
  --approval-sha256 REVIEWED_APPROVAL_SHA256

python3 -B gateway-reprovision.py recover \
  --transaction reviewed-transaction-id \
  --approval-sha256 REVIEWED_APPROVAL_SHA256
```

The plan expires within one day. Apply recomputes it and rejects changes before
stopping. `INSTALLED_STOPPED` means bytes/units switched and verified, not a
running or production-accepted installation. `RECOVERED_STOPPED` means the
original files and units were restored, not that old execution/SHM authority was
replayed. Both success states retain the stop fences.

Retain `/opt/gateway-reprovision/transactions/<id>` and the helper bundle.
`previous-home` is the original directory; recovery moves the candidate to
`retired-candidate`. Mutable history is captured after stopping, not pinned while
services are writing it. A failed or killed operation leaves evidence in place.
Recovery is repeatable and rejects stale transactions or foreign edits. Do not
delete a pending pointer or reuse a transaction ID to force progress.

## Evidence Boundaries

- `tools/gateway_reprovision_test.py`: 40 root-owned filesystem tests, including
  nine actual child-process SIGKILL boundaries during apply and recovery.
- `tools/gateway_reprovision_cli_test.py`: real CLI, production lock and fixed
  paths inside private mount/network/PID/IPC namespaces and chroot. Uses a fake
  systemctl; includes wrong SHA, real separate-bind-mount refusal, install,
  repeated recovery and stop-failure recovery. No business binaries execute.
- `tools/gateway_reprovision_fence_test.py`: real systemd, uniquely named harmless
  units, template inheritance and reload persistence. Only a private marker is
  touched; no gateway service is started/stopped by this fixture.

SIGKILL and daemon-reload evidence do not substitute for reboot or power-loss
testing. Production-readiness evidence is tracked separately in
`ems2-production-20261014.md`.
