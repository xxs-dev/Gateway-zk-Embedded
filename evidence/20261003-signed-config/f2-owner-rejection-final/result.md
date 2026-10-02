# F2 owner rejection lifecycle (LOCAL_ONLY)

The private fixture calls the real `RuntimeHandoff.own` install callback,
`finish`, `recover`, `cancel`, and `unfinished` paths for a mismatched release
and an injected low-space admission. Only systemd identity/stop commands are
substituted. Both reject before the production pending journal and keep the
live baseline/config unchanged. The original job/token remains in the active
marker and is discoverable by the official unfinished query. A simulated
uncertain stop leaves `CANCEL_REQUESTED`, the unfinished pointer, and no health
decision. The same job is then cancelled after a positive stopped-owner mock;
health becomes `failed/unchanged`, the unfinished marker is retired with its
receipt, repeated finish is idempotent, and new ownership is refused.

Final command:

`wsl.exe --user root -- env PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_signed_config_owner_rejection_finishes_original_job_unchanged`

Exit 0; 1 test with two reason subcases; 0.504s. Inputs were identical before
and after; exact SHA list is in `inputs-before.json`/`inputs-after.json`.
`stdout.raw` SHA-256 `0c37af133b2478cabd0efbaad9f46fe812e89d583e348ac4852407726681583e`;
`stderr.raw` SHA-256 `cb688d26d4e1426f43509f5b692743d622f7ba170bda37c0ddc8a2b9ede06db3`.
Full command/exit and stream sizes are in `run.json`/`capture.json`; raw files
remain local, not part of the review commit.

An earlier exploratory run failed inside the fixture with `KeyError:
'ControlGroup'` (tool exit 1, before product admission). It was corrected by
supplying the mocked manager property; that run was not captured as separated
raw streams and is not counted as product red evidence. This test does not
execute real systemd, RSA verification, or C++ `loadFinalStatuses` for the same
private job. Those boundaries remain OPEN.
