# Gateway-zk Agent Instructions

## AArch64 cross compilation

- The production target is Allwinner aarch64. Do not use RK3568 toolchains, RK3568 sysroots, or Windows MinGW Qt builds.
- The cross-compile environment is the remote server `192.168.22.11`, reached over SSH.
- The remote source workspace is `/srv/build/Gateway-zk` and the installed toolchain/sysroot live only on that server.
- The Windows checkout and WSL paths are not mounted or mirrored into `192.168.22.11`. Never assume a local edit is visible to the compiler.
- Before compiling, explicitly synchronize source through Git or upload an isolated source snapshot, then verify the remote branch, commit, and dirty state.
- Never run `git reset`, `git clean`, force checkout, or `rsync --delete` against the shared remote workspace. Preserve changes whose ownership is unknown.
- Run `tools/build_edge_aarch64.sh` and `tools/build_scada_qt_aarch64.sh` on `192.168.22.11`, not from the local Windows/WSL checkout.
- After a successful build, copy the selected artifacts back explicitly and verify SHA256. A remote build does not update local `build-aarch64` automatically.
- Compiling and copying artifacts are separate from deployment. Do not replace binaries on an edge device unless the user explicitly requests deployment.

See `doc/交叉编译教程/192.168.22.11边端交叉编译教程.md` for the current commands and paths.

## Codex and Grok cross-review

- Architecture designs, implementation plans, safety or control-policy changes, and code audits must be independently reviewed by both Codex and Grok.
- Codex prepares the first evidence-based proposal or audit. Grok then reviews a redacted problem statement and the relevant code through the API configured by CC Switch. Do not send passwords, private keys, certificates, tokens, or production credentials to the external reviewer.
- Reconcile disagreements explicitly, revise the implementation, run deterministic regression tests, and use Grok for a final acceptance review when code changed. Report material disagreements and the final decision to the user.
- A timeout, authentication error, partial answer, or CLI session that stops before a final report is not a completed review. State the failure and retry through the configured direct API when practical.
- Grok is a reviewer, not a production operator. Codex remains responsible for checking repository state, applying edits, running tests, controlling deployment scope, and verifying live devices.
- Emergency containment may precede review only when delaying it would increase equipment or data risk. Record the exception and complete retrospective review before fleet-wide rollout.
