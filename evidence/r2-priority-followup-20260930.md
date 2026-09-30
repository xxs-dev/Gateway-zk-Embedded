# R2 TCP priority-gap followup (2026-09-30)

Base: R2 commit `35189f4d735acdf5fba37132429ae7784960de3b`.

Defect: an ordinary read acquired the active transaction before waiting for its next request slot. A later priority write could not preempt the unsent read. The followup waits for the request gap while the transaction mutex is released and rechecks pending priority before claiming the transaction. The pre-send write guard remains in `sendAll`.

Red: updated `modbus_tcp_client_limit_test` built with native exit 0, then ran with native exit 1: `priority write must precede ordinary read already waiting for gap`.

Green: targeted native rebuild of `modbus_tcp_client_limit_test` and `ModbusRtu` exited 0. The isolated loopback test exited 0. It observes read/write/read order after a second read begins waiting for the gap, retains a start-to-start minimum interval, and verifies the write guard runs after that wait. The existing expired-priority-write case also passed. Test accept and socket reads have bounded timeouts; reader startup has a one-second bound.

Limits: local x86_64 and loopback only; no physical device, target-board build, or deployment.
