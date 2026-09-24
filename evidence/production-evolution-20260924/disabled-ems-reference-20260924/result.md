# Disabled EMS SHM reference, local script-only fix

Source worktree base: `bd000dedc83d3ec31f8016ddbfeb8d77a6541dc3`.
No device, build host, ARM compiler, package assembler or physical control was used.

M2 read-only inputs (not full app bodies): `host-23.json` SHA256
`30dda4d8d6ed75c3b58dea956a5d93d9e806c7f346787fbdfc7300e61b3299e3`
and `host-24.json` SHA256
`1d667dbe80d26c89a402e26bffcdc32f4452c9366b6605abb2c648544c67c2d7`.
Both project `mqtt-service.json` `emsCluster.enabled=false` and
`virtualSharedMemoryName=ems_cluster_store`; both report that SHM absent.
These projections alone do not qualify complete live migration inputs.

Runtime source review: `ems_cluster_main.cpp` exits on disabled EMS before its
store; `compute_engine_main.cpp`, `mqtt_driver_main.cpp` and
`mqtt_forwarder_main.cpp` add the cluster store only when enabled;
`cluster_write_authorization.cpp` does not open it when disabled. The exception
is limited to this direct disabled EMS field. Other references to the same name,
including enabled MQTT/compute readers, remain mandatory. Missing or mistyped
`enabled` does not qualify. The name is still validated.

Camera boundary: A projects an explicit empty `cameraService.sharedMemoryName`,
not an `enabled` value. `requireString` preserves the empty string. The camera
service returns early if disabled or its camera list is empty, then substitutes
`gateway_point_store` for an empty name. Nonempty camera names remain collected
by the generic guard, including when camera is disabled. No camera migration
exception, absent-active-unit inference or configuration edit was made.

`red/result.txt` SHA256
`629ca2e97950aede85e94334d087026029e12f401e279a30cd310b7f085b8902`:
the disabled/absent EMS apply refused at exact SHM-reference coverage; boundary
rejections passed. `green/result.txt` SHA256
`e586a39ba7d8a1d960a703c4aefd11fb0caa0f01d7ef0122b59a76cd9a39edc4`:
14 focused native namespace/fake-systemctl tests passed, including disabled
apply/observe/recover without a segment or roster, enabled/ambiguous and shared
reader rejection, startup guard, real R4/S2 fixtures, R5 preflight, and affected
OTA/factory/observation checks. The native CLI fixture does not qualify ARM or
the actual hosts. One existing observation test was tightened to mutate the
full fixed-voter config so it reaches its intended configuration-drift gate.

`paired-deploy-delta.json` pins the same eight deploy files, with only
`runtime-upgrade-guard.py` changed. The sealed R5 program manifest is unchanged.
P1/P2 Coordinator-only ARM work is separate; no new installer pairing,
deployment, device migration or release approval is claimed here.

## Evidence close-out after 6943b97

The two self-built 57,928-byte native fixture ELF files originally tracked in
`red/` and `green/` were moved without changing `6943b97` to the separate
ordinary evidence directory named in `fixture-archive.json`. Both copied/moved
files were checked before and after the move against the provenance SHA256
`f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`.
Their original paths are removed from the current tracked tree; historical Git
objects in `6943b97` are not rewritten. Existing evidence elsewhere is untouched.

The earlier preliminary 9-test run had 8 passes and 1 failure in
`test_offline_observe_rejects_physical_and_changed_inputs`: its assertion expected
`configuration changed`, but the test had replaced the full fixed-voter EMS
object with only `controlEnabled:true`, so `local_identity` refused first with
`enabled fixed-voter cluster configuration required`. During consolidation, the
preliminary `disabled-ems-reference-20260924-green/result.txt` directory was
removed; that exact result file is **not retained or hash-verifiable**. This is
a supplemental account of the observed run, not a recreated raw log. The test
was adjusted to preserve the full cluster object and change only
`controlEnabled`, then the final retained `green/result.txt` recorded 14/14.
The original expected-failure `red-final/result.txt` *is* retained at
`red/result.txt` with its original SHA256 above. The still-earlier preliminary
`red/` directory with a test-placement error was also not retained. No tests
were rerun for this evidence close-out.
