# V2 directory preflight, local-only supplement

Base product commit: `0944ca682c9dde24e7d8f863f5e19068c26c5afd`.
No product source changed here. The new private-root tests exercise the existing
`ProductionRuntime.inputs` predicate even when `configWriteSet=[]`. They do not
touch device `/opt`, systemd, a real OTA worker, or a broker.

## Fixed N1 input

`git archive 0944ca682c9dde24e7d8f863f5e19068c26c5afd deploy tools`
was expanded into `snapshot-0944ca6/`, separate from the dirty working tree.
Archive SHA256: `80bb629cdeacb951fd9159b585ee3ebee9b54c33266ee9eeeb5d8f7daf9bee4f`.
Extracted input SHA256: `deploy/offline-runtime-upgrade.py`
`bf4fe14375e6fc36be355427d79ff158ceb2ad4e2f1a7663b33664b1479f1b35`;
`tools/production_runtime_baseline_test.py`
`78a4fdf3d85b0db5d7cdcbdf79e6428041b08ec328860c3ae7b889d48d41bb9f`.
Both matched after execution. Command: `wsl --cd <snapshot-0944ca6> -u root
--exec python3 -m unittest -v` with the six exact test selectors listed in
`evidence/20261002-production-v2-checkpoint/config-directory-preflight-result.md`.
The archive has 236 members; independent checking found only 100 raw-byte
equal Git blobs and 136 work-byte differences after Windows extraction.
Python and relevant deploy runtime inputs were byte-equal. This is therefore
a fixed relevant-input claim, not a claim that all 236 extracted members are
raw Git-byte-exact.
Exit 0; six passed in 1.042s. Raw separate streams:
`n1-snapshot.stdout.raw` SHA256
`74c5c6c79866440bdea7ec8416b2501c87e627c7f8af6f52664de543e85c31e4`;
`n1-snapshot.stderr.raw` SHA256
`95732eaba7d13171105b8b353d206c0bc72e3e774554c4296c8bbc179cb3f0bc`.
These two raw streams and the three final streams below are preserved as
byte-exact members of `final-raw-streams.zip` (SHA256
`fccac52aa3edb477b981ac58fae46caf703efee8c198da570d3a8399883bc5f1`).
The five ZIP member hashes were independently recomputed after packaging;
they equal the individual hashes in this note. The loose local files remain
available but are not committed as newline-normalized Git text blobs.

## Affected code-only and OTA owner input

The current test file includes four added selectors:
`test_v2_code_only_trusted_config_directories_install`,
`test_v2_code_only_untrusted_config_directory_rejected_before_intent`,
`test_v2_ota_owner_untrusted_config_directory_rejected_before_mutation_barrier`,
and `test_v2_ota_owner_trusted_config_directories_complete`.
Command: `wsl --cd <edge checkout> -u root --exec sh
evidence/20261003-config-publication/run-directory-capture.sh` (script SHA256
`1ebd376fb6f5362075ac04d9463b0aded084b84c5539d93f6ec7856f4e7c86e6`).
The script lists the four fully qualified `ProductionRuntimeTest` selectors
and redirects Python output inside WSL. Host exit 0 and independent child
exit file `0`; four individually named tests passed in 0.715s. Before and
after SHA256 matched:
`deploy/offline-runtime-upgrade.py`
`bf4fe14375e6fc36be355427d79ff158ceb2ad4e2f1a7663b33664b1479f1b35`,
`deploy/runtime-upgrade-guard.py`
`2c330256122c5f7745dfd3f0202a80724c352cfed8a7863c9ce2a366052071ff`,
and `tools/production_runtime_baseline_test.py`
`67c5ac390cdf1a0621d2f149a3bb03ed8252c35980ba1a2eda13f1c289652fd0`.
Complete raw separate Python streams: `directory-complete.stdout.raw` SHA256
`edecc79819c65751a0e3aaad1f78587a380ad50185cf332f25ad0d59d5799704`;
`directory-complete.stderr.raw` SHA256
`3f74bc772159730f7338fcf954b6deb03182644405568b03716a8187ca369ee0`;
`directory-complete.exit.txt` SHA256
`9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa`.
Earlier Windows-side capture drafts `directory-focused.*` and
`directory-final.*` remain unmodified. Their stderr streams have WSL warning
and NUL corruption and lack individual test names, so neither is used as
complete per-case evidence. The WSL host warning is separate from the new
Python streams.

This proves local code-only and private OTA-owner admission behavior only.
Factory package directory metadata and any inventory-backed correction of
already installed directories remain open; no implicit chmod/chown is allowed.
The ordinary MQTT delete-then-restore regression remains red (exit 6), and
formal v2 re-add is a separate passing CLI scenario, not an MQTT fix.

## Checkpoint audit boundary

After a boot-lineage checkpoint, online verification re-reads and validates
the checkpoint edge plus the current authority. Earlier lineage records must
remain retained and protected, but the online path does not traverse and
revalidate every pre-checkpoint edge. This bounds boot cost; it is not a claim
of continuous full-chain audit or permission to delete old proofs.
