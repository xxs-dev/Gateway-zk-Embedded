# S1/S4 local reconciliation checkpoint

The final private x86_64 harness compiles the product OTA service and both product MQTT encoders. It exercises a durable terminal outbox intent before the final marker, replay of a completed or safe-failed final marker before pending status append, acknowledgement without reappend, legacy synchronous-tail timestamp stability, and a real private lifecycle-lock inode replacement. The same-instance reconcile/clear smoke path ran under a 15-second process timeout. This is a private state-window simulation, not a process hard-kill or device test.

Final compile and run exited 0. Exact commands, exit codes, and raw-stream SHA-256 values are in `s1-s4-final-local/capture.json`. Nineteen named compile inputs were snapshotted; their before/after SHA-256 lists agree. Earlier drafts and raw outputs remain local. The harness uses only its own `/tmp/ota-m3-local-*` directory and does not access host `/opt`.

The result does not prove owner/systemd execution, a Broker delivery, a signed TAR, the platform inbox bridge, or a complete crash-consistency model. Authenticated original-job query, bounded failed-safe recovery, and the full three-end production gates remain open.
