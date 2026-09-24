# B4 Windows Product Source Map

`products.json` is a local pairing input, not a deployable program manifest or
assembled payload. Its 20 unique product entries retain per-file provenance:
16 from the sealed R3 recovered base, three R5 replacements, and only
EmsClusterCoordinator from B4. In particular, the other 19 are not fresh
98e87e9 builds. `programs/bin/EmsClusterCoordinator` is a planned payload
position; the B4 artifact is currently a standalone file.

## Local pins

- R3 base: `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/acceptance-evidence-20260924/evidence/raw/GW-20260809-002/helper-r4-20260924T102700Z/r3-candidate-cd6b529.recovered.tar.gz`, SHA256 `0232f029a66b9df5a502ee8d10e3e24eac807edea82ee7346ae1042c8901a2d9`.
- R5 delta: `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/acceptance-evidence-20260924/evidence/raw/GW-20260809-002/arm-r5-20260924/r5-reelection-9ccfce8-delta.tar.gz`, SHA256 `2dc8582535ea8358c839eec95ba509df2001915b91989132793164866ca3a652`.
- R5 product manifest: `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/acceptance-evidence-20260924/tests/e2e/GW-20260809-002/ems-shadow-20260924/arm-r5-20260924/program-manifest.json`, SHA256 `75c7018d4d78d93e67f8239c041b5f1003d460f0266f549e0c25053cc9795ca6`.
- B4 Coordinator file: `C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/arm-authority-98e87e9-20260925-b4/EmsClusterCoordinator`, 2,837,656 bytes, SHA256 `e2d1594b0fb5ad68aa2f0791b29a984cf57a53ee0dd1fa99e07d8ad120bdf274`.
- B4 one-component manifest: `D:/workspace/GatewaySuite-workspaces/GW-20260809-002/acceptance-evidence-20260924/tests/e2e/GW-20260809-002/ems-shadow-20260924/arm-b4-20260925/execution-b4/program-manifest.json`, SHA256 `00c1b29f55c8ab0cb82e509e8cc093b17006cc75886d4c57b9d053af306a4d20`; acceptance seal `96e3cba9281d8901c244560467a4222a5addc3f4`.

The 19 inherited product hashes and sizes match the R5/B4 manifests; their
archive members and the standalone B4 file were locally rehashed against
`products.json`. The Qt archive member `programs/qt/KY-SCADA` maps to offline
install path `ky-ems/KY-EMS`. No archive was extracted or assembled.

The final Edge deploy-script commit is `6943b97a17ff184113cf52b30fe3411c4e24ee1b`:
52 paths under `deploy/`, Git tree SHA1
`62dbf2ddd0a8f58975b00bdb168c970b648eac62`; `deploy/` is unchanged
through `d2002e5a36df0920149e9423d93e251cad6a8702`.
`deploy/offline-runtime-upgrade.py` SHA256 is
`e5d9f8263c037c7988e835cd794126585b64873b29a44985ba5d7c916e1279e5`.
The existing eight-file SHA256 index is
`evidence/production-evolution-20260924/disabled-ems-reference-20260924/paired-deploy-delta.json`
(file SHA256 `426215d7a67b0edb65acdf8574ac72288686577afa48f03b57a68f618d5590dd`).
No separate 52-file SHA256 index was confirmed; use the pinned Git commit for
the full path tree, not the eight-file index as a complete deploy manifest.

## Main-runtime boundary

For a later approved A/B offline transaction, the maintained standalone flow
checks and installs the complete 20-product candidate set, including programs
absent from the old installation. Keep all currently installed, candidate-
available ABI participants together rather than mixing old and new owners.
Coordinator and AgcAvcController can be present as files without enabling
their services; this is not a claim that they remain uninstalled. Preserve
camera and EMS-cluster disabled flags and existing business data; do not
authorize physical output. A's five observed V10 stores to account for are
`gateway_point_store_dio`, `gateway_point_store_ems_virtual`,
`gateway_point_store_gw002_readonly_rtu2_v2`, `gateway_point_store_ttySP1`,
and `gateway_point_store_ttySP2`. Also include `gateway_point_store` if it
exists, and close any other actual config reference before approval. Do not
invent an absent `ems_cluster_store` for explicitly disabled EMS.

The bounded follow-up is
`D:/workspace/GatewaySuite-workspaces/GW-20260809-002/acceptance-evidence-20260924/tests/e2e/GW-20260809-002/ems-shadow-20260924/main-runtime-offline-followup/README.md`.
For A, only the unresolved `*File/*Path` field location/type in the four
already-read configs may need a narrow projection. For B, first freeze the
changed set, then obtain only its effective units, installed program hashes,
config references/flags, SHM and persistent dedup metadata, plus identity.
Do not borrow A's paths or state for B. Moving from old source `7e392ce` to
this mixed-provenance candidate is not yet a qualified state/rollback
compatibility claim; direct-maintenance and dedup state need bounded closure.

Blocked: no assembled 20-product payload or new program manifest/Windows
pairing approval; B's necessary projection is absent; A's unmodelled field and
any implicit/default SHM remain unresolved; stopped-state hashes, unmapped
segments, and consistent backup/rollback are not yet qualified. No real PCS
validation exists, so the evidence remains simulation-only.
