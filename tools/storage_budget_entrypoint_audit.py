#!/usr/bin/env python3
"""Keep the StorageBudget integration audit tied to real source entry points."""

from pathlib import Path
import sys


def require(text: str, needle: str, path: Path) -> None:
    if needle not in text:
        raise AssertionError(f"missing {needle!r} in {path}")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    event_store = root / "src/mqtt_event_outbox.cpp"
    event_text = event_store.read_text(encoding="utf-8")
    for needle in ("EventStoreDatabase::capacityStatus", "EventStoreDatabase::append", "statvfs", "CAPACITY_REJECTED"):
        require(event_text, needle, event_store)

    owners = {
        "history": ("src/sqlite_sample_writer.cpp", "writeSamples", "retentionDays_"),
        "alarm-history": ("src/sqlite_alarm_writer.cpp", "writeEvents", "retentionDays_"),
        "control-dedup": ("src/control_dedup_store.cpp", "ControlDedupStore::bind", "sqlite3_open_v2"),
        "control-result": ("src/mqtt_control_result_store.cpp", "reservePending", "sqlite3_open_v2"),
        "ota": ("src/ota_service.cpp", "ensureFreeDiskSpace", "maxArtifactBytes"),
        "offline": ("src/builtin_mqtt_driver_publisher.cpp", "flushOfflineBuffer", "offlineBufferMaxDiskBytes"),
    }
    for name, (relative, *needles) in owners.items():
        path = root / relative
        text = path.read_text(encoding="utf-8")
        for needle in needles:
            require(text, needle, path)
        if "checkStorageAdmission" in text:
            raise AssertionError(f"{name} unexpectedly claims unified admission")

    print("storage budget entrypoint audit: 7 owners and EventStore gate PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
