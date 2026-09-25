# A Camera Device-List Default Gate, Local Only

Base `09fedd1dccd54124a57ea518a06ccbc31b5dbca1`. Pinned R3 `cd6b52949317f1640611d690128c5c5928f62b20`: `src/config_loader.cpp:343` returns an empty vector for absent/null `deviceConfigFiles`, and `:4143` assigns it. `src/system_monitor_runtime_discovery.cpp:65` merges only that vector's entries. The A camera app gate now accepts absent/null/`[]` only; a nonempty list or non-array is refused. Monitor/mqtt still require exactly the approved one-element absolute device path. No device, ARM build, or Windows package was touched.

RED: `wsl -u root --exec python3 tools/install_isolation_test.py --evidence evidence/production-evolution-20260924/a-camera-device-list-20260925/red --test test_offline_a_camera_device_list_absent --test test_offline_a_camera_device_list_null`; exit 1, 2 failures at A camera shape gate. Raw `red/result.txt` SHA256 `31c9a45eec9ba1d09d996c3d8acf46c419aed614314664b0dae610885d0f9c8d`.

GREEN: same runner and `--evidence .../green`, tests `test_offline_a_camera_device_list_absent`, `test_offline_a_camera_device_list_null`, `test_offline_a_camera_device_list_nonempty_refused`, `test_offline_a_camera_device_list_wrong_type_refused`, `test_offline_a_joint_absent_camera_default_observe`; exit 0, 5/5 PASS. Raw `green/result.txt` SHA256 `283762c94422dece51149dc46416bd7f8a9ac7abd7f5e23c4625e2f3c7558c68`.

Private-namespace native fixture binaries are archived outside Git at `C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/a-camera-device-list-20260925/{red,green}-offline-upgrade-fixture`, each SHA256 `f621efe7f6e2523718fcc52988cd1da941e8ecff9421be911fa9b5cab09523d0`. The in-tree provenance and original logs remain. This is script qualification only, not real A observe authority.
