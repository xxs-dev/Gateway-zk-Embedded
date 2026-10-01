# R10 Fixture Compatibility Correction

Local-only,2026-10-02. Parent68de70f. The fixed Allwinner build entry uses
Linaro6.3.1. New system_monitor_diagnostic_test used the later standard
filesystem header; this is an avoidable source compatibility risk, not a
measured remote compiler failure.

Only this test changes: private directory creation uses mkdir; cleanup uses
fd-relative POSIX traversal with O_NOFOLLOW and dev/inode checks before unlink.
No diagnostic product logic, command vocabulary or expected behavior changes.
The Windows syntax command below returned0, but excludes the POSIX fixture body.
Actual fixed-toolchain compilation and Linux cleanup are still unverified.

`g++ -std=c++17 -Wall -Wextra -I include -fsyntax-only tools/system_monitor_diagnostic_test.cpp`

`git diff --check -- tools/system_monitor_diagnostic_test.cpp`:0.

No SSH, WSL or build/device command executed. Follow-up preparation must archive
the new test commit's Git bytes, bind unchanged product blobs to68de70f, and use
the existing CMake entry (which selects the compiler's supported C++17 spelling).
The supplemental ownership probe uses -std=c++1z for this fixed GCC6 toolchain;
this is not an alternate product build or toolchain change.
