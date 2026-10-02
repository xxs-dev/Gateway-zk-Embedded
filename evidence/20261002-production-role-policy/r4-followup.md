# Role-policy default and protocol follow-up

Base checkpoint: `7227d6f06a7391ef5373b0486d72396a36163d38`.

The resolver now rejects noncanonical device protocol spelling rather than lowercasing to a driver selection that the C++ loader/main does not make. Every camera item uses the C++ `enabled=true` default during admission, including source, custom-command and output-receiver checks. Missing `sourceAuth.enabled` uses the C++ false default.

Final source SHA256: `deploy/production_role_policy.py` `e07596e70f392f58d797dd5f69600d5ce7cff66cd50d58d540aa24cb6067417f`; `tools/production_role_policy_test.py` `5b8eea95842bc0a7f71378da5b318d1712f8fc367bf55cc551c9d364c299a57d`.

`python -m unittest tools/production_role_policy_test.py -v`: exit 0, 29 tests, 1 Windows symlink skip; `r4-followup-unittest.raw.log` SHA256 `015fefedff2eab37a620cce76ffea621cc210bc8f1d6a0a27f3a6867fabbcea3`. The earlier failed local run is retained as `r4-draft-failed.raw.log` SHA256 `dc6f0401283c2cafbef2b9ecdf234f46482b897c588821afbe677bce883a4fda`; it is not a PASS. Python 3.8 AST and `git diff --check` passed.

No production entrypoint uses this policy yet. No Linux, ARM, service, camera subprocess, network, SHM or device operation was executed.
