# Driver Physical Write Guard

Base: 9df2ddf41633caaeab544a123f9057aed86420df.
Branch/worktree: ems-driver-send-guard-20260924.
Product head: 2cac8f6bc44ed37c9e0948298f84ad95ba5615a8.

## Final Evidence Index

**The passing unsupported result is green-unsupported-bound-test.log (exit 0).**
Files named green-unsupported-test.log, green-unsupported-final-test.log,
green-unsupported-ownership-test.log and diagnose-unsupported-can-test.log
are preserved FAILED attempts, not qualification evidence. Their fixtures first
used an invalid zero UDP listen port, then lacked an active MQTT generation,
then supplied an invalid queue token, and finally lacked the ordinary durable
command's ingress ledger.bind(). The final fixture retains both successful
receipt and actual recv > 0 assertions; no product send assertion was relaxed.

- red-queue-test.log: expired queued command actually sent 12 TCP bytes.
- green-queue-test.log: expired queued command sent zero bytes.
- red-daemon-test.log: TCP guards passed, daemon still sent expired command.
- green-daemon-test.log: 10 TCP/queue scenarios passed.
- red-serial-test.log: guarded RTU valid command unsupported.
- green-serial-test.log: 10 TCP plus 8 real PTY scenarios passed.
- red-unsupported-fixed-fixture-test.log: DIO/IEC/DLT and CAN bypasses reproduced.
- green-unsupported-bound-test.log: all four unsupported protocol groups passed.

TCP/PTY coverage: expired, revoked, epoch changed, valid, ordinary, after actual
readiness/inter-frame wait, partial syscall continuation, and EINTR continuation.
TCP additionally covers missing context and GatewayDaemon queue dispatch.
The partial tests inject a real short send/write and publish revocation before
the next iteration: TCP total 4 bytes, RTU total 3 bytes, zero further calls
transmitting bytes. TCP then successfully sends an ordinary command on the
same client, proving no callback retention between commands.

Unsupported actual executors: DIO GPIO, IEC client, and DLT serial fake I/O each
receive zero writes for tagged and configured graph commands lacking context,
then one ordinary write. CAN uses the real UDP test transport in a private
network namespace: missing/tagged x ordinary/durable all send zero datagrams.
No dedup ledger is created for rejected cluster commands. Stale MQTT generation
is rejected; current generation with durable ingress binding receives a success
receipt and one real UDP datagram.

## Implementation

PendingWriteCommand travels intact via executePending. BeforePhysicalWrite is a
per-call argument through CommandExecutor, guarded IModbusClient write overloads,
TCP transact/sendAll or RTU transact/ISerialPort/PosixSerialPort. Captured command
copies are immutable; no global/client/thread-local authorization callback exists
in product code. Each check reads current authority and monotonic time through
the existing ClusterWriteGuard. Existing implementations reject nonempty callbacks
by default rather than silently delegating an unchecked write.

TCP checks after readiness and immediately before each send, disconnecting on
rejection. Serial checks before every write, including partial/EINTR continuation,
and before drain; rejection flushes queued output best-effort and closes the port.
These checks cannot retract already accepted kernel/device bytes, make a blocking
syscall atomically revocable, or guarantee CPU timing under arbitrary preemption.

GatewayDaemon retains existing ownership and durable dedup behavior. CAN is NOT
read-only: it independently drains scheduled commands and calls sendCanWrite in
both durable and ordinary paths. The new required-context rejection precedes both
branches and dedup handling. CAN startup writes remain the existing untagged,
non-queue path; this patch does not implement cluster CAN support.

All five driver mains value-copy appConfig.emsCluster. Device physical SHM names
and cluster virtualSharedMemoryName remain independent and are not overwritten.

## Files And Targets

Product files:
- main.cpp, can_driver_main.cpp, dio_driver_main.cpp, dlt645_main.cpp, iec_driver_main.cpp
- include/edge_gateway/common/command_executor_interface.hpp
- include/edge_gateway/{command_executor,interfaces,modbus_tcp_client,modbus_rtu_client,serial_port,posix_serial_port,iec_command_executor,dio_command_executor,dlt645_command_executor}.hpp
- src/{command_executor,writeback_service,modbus_tcp_client,modbus_rtu_client,posix_serial_port,can_driver_service}.cpp
- src/common/gateway_daemon.cpp

New tests: tools/cluster_driver_send_guard_test.cpp and
tools/cluster_driver_rejection_test.cpp. Other changes are this evidence directory.
No models/config_loader/router/Graph/points/helper/CMake edits.

Native Debug GCC 15.2: edge_gateway, ModbusRtu, CanDriver, DioDriver, Dlt645Driver,
IecDriver and six existing regression test targets built successfully
(affected-targets-rebuild.log). Initial affected-targets-build.log failed because
the WSL /tmp build directory no longer existed; configure/rebuild resolved it.

Executed necessary existing regressions:
- modbus_rtu_client_resync_test: PASS
- posix_serial_port_recovery_test: PASS
- dlt645_command_executor_test: PASS
- control_dedup_protocol_test: PASS (DIO/DLT/IEC)
- control_dedup_integration_test: FAIL, old shared-memory ABI fixture requires
  ABI 11. Coordinator has assigned that fixture repair to Edge; unchanged here.
- can_signal_codec_test: built but not executed because verify.sh stopped at
  the preceding known fixture failure.

C++14 syntax-only compilation passed for 15 affected source/main translation
units; see cxx14-syntax.log, which also records compiler and test/CAN SHA256.
This is NOT a full C++14 linked-build claim. Driver-stage ASAN, Linaro GCC6.3.1,
old-sysroot and device tests were NOT run. No remote build/deployment/release.
Tests used private net/mount/IPC/PID namespaces and private /dev/shm. Previously
green TCP suites were not rerun after the coordinator requested targeted closure.

## Integration Registration

Coordinator-owned CMake snippet; Linux only:

```cmake
if(NOT WIN32)
    add_executable(cluster_driver_send_guard_test tools/cluster_driver_send_guard_test.cpp)
    target_link_libraries(cluster_driver_send_guard_test PRIVATE edge_gateway
        "-Wl,--wrap=send,--wrap=select,--wrap=write,--wrap=nanosleep")
    add_executable(cluster_driver_rejection_test tools/cluster_driver_rejection_test.cpp)
    target_link_libraries(cluster_driver_rejection_test PRIVATE edge_gateway)
endif()
```

Run through the existing private-namespace runner. The send-guard executable
accepts a single scenario name, e.g. rtu-partial; the rejection executable runs
only its four unsupported protocol groups. Do not register a duplicate target.

## Stage Commits

- 57a8ee9fdb637243b0fcbe8779abe2084c74e739: expired queue RED
- c1f524a728c1ced7e7c6254bf30103ee1e571e16: TCP queue GREEN
- 7701d1b84d9bb4fe995196358a398ea33145c1ca: continuation tests / daemon RED
- e68df2870e335e35b10c8ae886fabf9b4e2f73fe: daemon and main config binding
- 3bcaaf15730098651ceea7d4cc3cf7fc82330c90: real PTY tests RED
- 5cfdc6e0fcf1981e1e1be47fe2456cb3d66918c1: RTU GREEN
- 86be365d37fcffcb1304b792242951aad8bc235c: unsupported RED
- 2cac8f6bc44ed37c9e0948298f84ad95ba5615a8: unsupported GREEN
