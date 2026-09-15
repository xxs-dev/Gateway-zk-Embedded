# SQLite 点位历史低空间保护

`SqliteSampleWriter` and `SqliteAlarmWriter` now perform a write-before admission check.
The default reserve is 256 MiB. The check uses filesystem `f_bavail` for the database
path and observes the database plus `-wal`, `-shm`, and `-journal` sidecars. A conservative
4 KiB per incoming row is reserved before the SQLite transaction begins.

The reserve is local to each writer and is not an atomic whole-machine budget. It does
not govern the control ledgers, OTA directories, offline MQTT file, or systemd journal.
The existing 30-day retention default and cleanup behavior are unchanged. Rejected
writes throw through the existing persistence failure paths; gateway and CAN persistence
loops catch the error, retain pending samples, back off, and continue collection.

Tests inject the available-space probe to prove low-space rejection, preservation of
existing rows, recovery after space returns, alarm-history coverage, sidecar accounting,
and the existing SQLite rollback/pressure paths.
