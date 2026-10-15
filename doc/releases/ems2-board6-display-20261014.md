# EMS 2.0 Board 22.6 Display Installation

Follow-up: `ems2-board6-kiosk-20261014.md` supersedes the desktop-top-panel and
post-activation-reboot limitations below. Dedicated fullscreen and automatic
boot entry now pass, including user-confirmed screen filling and navigation.
Other field/production gates remain open.

Date: 2026-10-14, America/Los_Angeles. Device logs use UTC+08:00 and
therefore show 2026-10-15. This record supersedes earlier statements that
22.6 was not modified. The user explicitly authorized replacement and reboot.
22.7 was not changed in this operation.

## Installed Scope

- Device: `192.168.22.6`, identity `COMM202600999`, Allwinner AArch64.
- Executable/helper source: `ff75a5271e5be4d9a532084ed5ea038e69cf342a`.
  No new C++ build or product-source change was made for this installation.
- Factory SHA256:
  `dc2c16f106e25dda3ec59c4fdbedc3b74160d1b5b5d64d19240a9ec89f3d8b6f`.
  Paired tools: `d43000686edef494a9f502069dd666428e704f946a8c00a17d1e2a6578b3e3b1`.
- UI project: `2.0.14-lanhu`. Device-specific SCADA archive was generated on
  22.11 by changing only device identity strings in 58 JSON files and rebuilding
  checksums. Other project files/assets were unchanged. SHA256:
  `a1ddf96b3fc761a93cceaa002e291137b0556d3a8584c6b87a1f8a7b9b977189`.
- Native Qt uses XCB on the existing 1920x1080 X11 desktop, not offscreen.
  The service copies the current GDM authorization file into its root-owned
  runtime directory before starting. Only the X11 socket is exposed to Qt;
  no framebuffer, GPU, serial, CAN or GPIO device is granted to it.
- Driver, Monitor and Qt run in a shared private network namespace, private
  devices, zero capability sets, no-new-privileges and seccomp restrictions.
  Acquisition is simulated, device point writes and OTA are disabled. Real
  field values are not mapped into the UI; missing values remain `--`.
- Old application processes are stopped. The retained old installation is a
  rollback artifact, not a parallel running system.

## Preservation And Deployment

The stopped original installation and systemd files were archived and verified
on-device and locally: 553 entries, SHA256
`2f546723bb7e55cbbd0770e1bb70b06e39849aa956d9986eb716e147009081d5`.

The first stopped trial correctly refused two legacy backup entries with mode
0777. Recovery completed before retry. Only
`backup/runtime-20260918083902/tls` and its `mqtt-service-stunnel.conf` were
tightened to 0700 and 0600; original modes/content remain in the archive.
A sparse 1 GiB historical MQTT ring exceeded the reprovision per-file bound.
Its containing `data/mqtt-spool` directory was atomically preserved at
`/srv/gateway-board6-display-ff75a52-20261014-v3/retained-original-mqtt-spool`.
It was neither deleted nor replayed into the new runtime.

The second stopped trial and recovery passed. Final transaction:
`ems2-board6-display-ff75a52-20261014-final`, approval SHA256
`88ecdcc82f311353f59b57b463092472fa7ca62578ac8ec011360badd1043cd0`.
The previous home remains in that transaction's `previous-home` directory.
Restoring the exact pre-maintenance installation also requires the preserved
spool and original backup metadata; do not treat the old directory alone as
the complete original archive.

## Startup Results

Initial activation `LOCAL_ACTIVATE_D6` exceeded the lifecycle helper's 45-second
systemctl timeout while the launcher redundantly started already-running units
and retriggered recovery checks. The stop request canceled a concurrent recovery
job; a second paired recovery completed after jobs settled. Preparation D6 was
then explicitly aborted. No healthy baseline was published for that attempt.

The board had 73 retired shared-memory objects. A read-only timing probe measured
2.492 seconds for the retired-mapping scan versus 0.454 seconds with an empty
name set; runtime file hashing took 0.202 seconds. These measurements support a
startup-cost contribution, not a complete performance root-cause proof.

One authorized reboot changed boot ID from
`87ba3880-1293-4b45-8826-8386e8ffb40f` to
`3b44b0af-20b4-4313-acd2-faeb9fd6c768`. Old volatile stores disappeared through
normal reboot, without manual unlink, replay or gate removal. Fresh preparation
`LOCAL_PREPARE_D6B` and activation `LOCAL_ACTIVATE_D6B` completed through systemd.
Activation approval:
`5e890a651fe655e93b2cd19c02ad597d5057aedcf9ad6079a23d09bf6e57da0c`.
Healthy baseline:
`1172a9b5018e5d3fc1730cbbe4373dafddfe9c5e5545b26c1c56e64fe3174be6`.

The launcher completed as active/exited, its expected oneshot state. Driver,
Monitor and Qt were active/running with zero restarts. Actual desktop captures
showed the instrument and network-monitor pages at 1920x1080. The desktop top
panel remains visible; these are not dedicated fullscreen-kiosk acceptance.

Final read-only verification passed all 18 component hashes/dependencies and
374 paired tool files, previous-image/spool integrity, no pending transaction,
old service shutdown and the runtime isolation checks. Seven samples over
60.293 seconds showed no restarts, maximum point age 261 ms and maximum local
HTTP latency 22.618 ms. This bounded observation is not endurance qualification.

## Evidence And Open Gates

Evidence is under `generated/production-20261014/board6-display-ff75a52-v3/`.
Earlier v1/v2 staging evidence is retained: identity mismatch and sandbox X11
authorization preflight. Final proof and seven-sample bounded run are recorded
in `final-verification.json`; screenshots are `screen-during-startup.png` and
`screen-post-reboot-started.png`.

`productionReady=false`. This is controlled display/software testing, not an
unrestricted public or field release. Remaining gates include real acquisition,
control/HIL interlocks, long-duration and power-loss testing, production
credentials/trust, touch acceptance and formal 95% UI fidelity. A shared X11
desktop is not a hardened production kiosk. The expensive repeated startup
checks and timeout interaction are still a release risk; a clean-boot success
does not fix or qualify the un-rebooted legacy-state case. This reboot occurred
before final activation, so automatic recovery of the new X11 profile after a
subsequent reboot is not yet accepted.

Preserve paired lifecycle tools and both failed and completed bootstrap records.
Never replay the old stopped-reprovision recovery over the active D6B journal.
