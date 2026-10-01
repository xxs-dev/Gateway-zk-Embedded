# A4 Capacity Test Split

LOCAL_ONLY. Parent 987cddd845169adfa3f52b85065d7d81cab99bd2. Product code unchanged.

A3 actually ran 31 selectors: 30 PASS / 1 FAIL / 0 ERROR / 0 SKIP. Keep its byte-exact receipt/log and retained remote work. The failed case expected `capacity|memory`, while the actual guard correctly reported `insufficient MemAvailable for activation`. The later no-admission assertion and tmpfs branch did not run.

Only tools/production_native_boot_test.py changes: split into independent memory/tmpfs selectors. Each requires its precise anchored error and calls existing assert_unadmitted (no native call, no boot intent/proof, no SHM target/retired source, original source bytes), plus no reader start. Memory uses zero MemAvailable; tmpfs starts with sufficient MemAvailable and zero statvfs available bytes. No broad exception match or product capacity relaxation.

Local Python3.8 grammar, standard-library selector loading/count=2, and diff check exit 0. **No tests executed here**: Linux root/flock fixture remains pending. Windows loading is not a runtime PASS/skip. No Windows substitute framework, WSL or SSH.

Manifest: 11 Git blob inputs / 501604 bytes, only native test hash changed relative to A3. New runner reuses fixed A3 baseline, connection, upload, limits, group/subreaper, conditional cleanup and final-baseline mechanics; only source/manifest/root/receipt, two selectors and count change. Default no-connection dry-run. Do not rerun the 30 passing A3 cases. Fresh 23-only scope must be independently authorized before execution. Failure work/evidence retained; no automatic retry or 24.

This is preparation for private Linux capacity rejection tests, not native creator/systemd/reboot/business/RSA-HTTPS OTA acceptance. B/C/D remain necessary separate targets. A1/A2/A3 records and remote roots remain intact; WSL47230 remains unknown and untouched.
