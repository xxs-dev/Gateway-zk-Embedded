# First Runtime Preparation

`runtime-bootstrap.py` prepares a first runtime after a stopped whole-image
reprovision. It is **not an activation command**. It does not start or enable
services, replace live configuration/units, remove STOP/fences, promote a
production baseline, or delete/reinitialize existing shared memory.

## Prerequisites

- Run as root on Allwinner AArch64 with the existing offline lifecycle lock.
- Retain the exact `INSTALLED_STOPPED` reprovision receipt, installed image and
  units, previous image, STOP and manager-loaded fences. All participants must
  be stopped with no remaining business process.
- Use one reviewed, root-owned, nonwritable-to-others helper bundle containing
  `runtime-bootstrap.py`, `gateway-reprovision.py`, `offline-runtime-upgrade.py`,
  `runtime-upgrade-guard.py` and `production_role_policy.py`. Recovery requires
  the same helper pins. Never substitute newer helpers into a historical
  recovery bundle.
- The installed `/opt/modbus-gateway/bin/pointctl` must be a pinned AArch64 ELF
  supporting `create-empty --shm <new-name>`. Old binaries are rejected without
  executing an unknown command. Do not patch a reprovisioned image in place:
  deploy a new reviewed package through its own stopped transaction first.
- Obtain the approved OTA public key at `/etc/modbus-gateway/ota.pub` and the
  approved HTTPS artifact origins. Fixture keys/origins are not production trust.
- Keep the candidate config outside both `/opt/modbus-gateway` and
  `/opt/gateway-runtime-bootstrap`. Identity, package manifest, network and TLS
  bytes/pins must match the installed configuration.
- Use a reviewed `board-simulator-profile-v1`. Physical outputs are not
  authorized. Native Qt must explicitly bind its protected monitor SHM name.

## Inputs

All JSON is root-owned, single-link, canonical and nonwritable-to-others.

| Input | Required Fields |
| --- | --- |
| Authority | `machineCode`, `identitySha256`, `networkSha256`, `tlsSha256`, `otaPublicKeySha256`, `monitorShmName`; same protected-authority schema as `production_role_policy.py` |
| Profile | `format: board-simulator-profile-v1`, `machineCode`, `ramMiB` (2048 or 4096), `capabilities`, `simulatorBindings`, `allowedUnits`, `segmentNames` |
| Trust | `requireSignature: true`, `checksumRequired: true`, `allowInsecureHttp: false`, `publicKeySha256`, nonempty distinct `httpsOrigins` |

`allowedUnits` must match the resolved role order exactly. `segmentNames` is the
sorted unique set of all resolved role SHM names, with 1-128 entries. Names must
be fresh, never reused from existing host objects. Config may contain the pinned
`edge-package-manifest.json`; it is metadata, not an additional service config.

## CLI Sequence

The following names are placeholders for reviewed inputs, not device-ready
configuration. Use the absolute path to the paired helper bundle. Review the
generated approval and pin its SHA256 before preparation. Expiry is a Unix
timestamp no more than one day ahead; preparation is bound to the current boot.

```sh
python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py plan \
  --config /reviewed/config \
  --creator /opt/modbus-gateway/bin/pointctl \
  --authority /reviewed/authority.json \
  --profile /reviewed/profile.json \
  --trust /reviewed/trust.json \
  --transaction REVIEWED_TX --expires-at REVIEWED_UNIX_EXPIRY \
  --output /reviewed/prepare-approval.json

python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py prepare \
  --approval /reviewed/prepare-approval.json \
  --approval-sha256 REVIEWED_APPROVAL_SHA256

python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py recover \
  --transaction REVIEWED_TX --approval-sha256 REVIEWED_APPROVAL_SHA256
```

## State And Recovery

The private journal is `/opt/gateway-runtime-bootstrap`, mode 0700. Preparation
records `INTENT`, `CREATING`, then `PREPARED_STOPPED`. Its external transaction
contains the approval, copied config, progress and `ready.json`; the final
`current.json` pins that ready receipt. New approval/pointer/receipt publication
uses fsynced staging plus atomic no-replace rename. Progress replacement is
atomic. Partial staging files are retained, never treated as valid records.

Preparation validates the visible `/dev/shm` mount ID as tmpfs, checks tmpfs and
memory headroom, then calls native exclusive creation. Each completed empty v11
store is bound by name, device, inode and whole-file SHA256. Native zero stats,
ownership, permissions, ABI and absence of mappings/open descriptors are checked.
Origin, helper/trust pins, stopped status, boot, expiry and earlier objects are
rechecked before publishing the receipt. Retained host SHM metadata is unchanged.

The prepared receipt explicitly says `activation: not-authorized`. There is no
command in this module that converts it into a production startup permission.

Recovery validates its paired authority and pointer ownership, records
`ABORTED_STOPPED`, and removes only matching bootstrap pointers. It retains all
config/evidence and all new/partial SHM. Recovery is repeatable and does not
require an unexpired approval or unchanged test trust, but the stopped origin
and paired helper pins must still match.

An interruption after durable approval but before the first state checkpoint
can be aborted only when no bootstrap pointer or target SHM exists. If the
approval itself was never published, there is no recoverable authority: leave
the unused transaction/staging evidence intact and use a new transaction ID.
Never reuse a consumed ID or orphaned SHM name. Do not delete objects manually
to make preparation pass. A preparation abandoned across reboot cannot be
activated using its old boot-bound empty-store evidence.

## Validation Boundary

`tools/runtime_bootstrap_test.py` uses private root-owned fixture paths and a
mock service manager. Ordinary cases substitute the native constructor; its
opt-in case uses the real AArch64 creator in a separately mounted private tmpfs
and exercises the actual kernel mount/capacity checks. The service manager,
lock proof and mapping discovery remain fixture substitutions in that case.
`tools/pointctl_create_empty_test.py` additionally verifies byte-identical refusal
of existing objects and two concurrent creators with exactly one winner.

Exception-injected interruption tests are not SIGKILL or power-loss acceptance.
Full first-runtime activation, healthy baseline promotion, activation-failure
recovery, reboot, running upgrade/rollback and sustained operation remain open.
This helper is not yet part of the installed factory candidate on 22.7.
