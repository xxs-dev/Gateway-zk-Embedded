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
