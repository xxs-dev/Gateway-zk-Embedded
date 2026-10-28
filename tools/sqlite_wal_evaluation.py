#!/usr/bin/env python3
"""Isolated Linux/eMMC evaluation. Never opens a configured production database."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time


class Database:
    libraries = {}

    def __init__(self, library, path):
        key = str(Path(library).resolve())
        if key not in self.libraries:
            self.libraries[key] = C.CDLL(key, mode=os.RTLD_LOCAL)
        self.lib = self.libraries[key]
        self.db = C.c_void_p()
        self.lib.sqlite3_open_v2.argtypes = [C.c_char_p, C.POINTER(C.c_void_p), C.c_int, C.c_char_p]
        self.lib.sqlite3_close_v2.argtypes = [C.c_void_p]
        self.lib.sqlite3_extended_errcode.argtypes = [C.c_void_p]
        self.lib.sqlite3_busy_timeout.argtypes = [C.c_void_p, C.c_int]
        self.callback_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int,
                                       C.POINTER(C.c_char_p), C.POINTER(C.c_char_p))
        self.lib.sqlite3_exec.argtypes = [C.c_void_p, C.c_char_p, self.callback_type,
                                        C.c_void_p, C.POINTER(C.c_char_p)]
        self.lib.sqlite3_free.argtypes = [C.c_void_p]
        rc = self.lib.sqlite3_open_v2(os.fsencode(path), C.byref(self.db), 6, None)
        if rc:
            self.close()
            raise RuntimeError("open failed: " + str(rc))
        self.lib.sqlite3_busy_timeout(self.db, 100)

    def execute(self, sql, check=True):
        rows = []

        def collect(_, count, values, names):
            rows.append([values[i].decode() if values[i] else None for i in range(count)])
            return 0

        callback = self.callback_type(collect)
        error = C.c_char_p()
        start = time.monotonic()
        rc = self.lib.sqlite3_exec(self.db, sql.encode(), callback, None, C.byref(error))
        message = error.value.decode(errors="replace") if error.value else ""
        self.lib.sqlite3_free(error)
        result = dict(rc=rc, extended=self.lib.sqlite3_extended_errcode(self.db),
                      ms=(time.monotonic() - start) * 1000, rows=rows, error=message)
        if check and rc:
            raise RuntimeError(str(result))
        return result

    def scalar(self, sql):
        return self.execute(sql)["rows"][0][0]

    def close(self):
        if self.db:
            self.lib.sqlite3_close_v2(self.db)
            self.db = C.c_void_p()


def dump(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def wait_file(path, process=None, seconds=30):
    end = time.monotonic() + seconds
    while not Path(path).exists():
        if process and process.poll() is not None:
            raise RuntimeError("worker exited before ready: " + str(path))
        if time.monotonic() > end:
            raise TimeoutError(str(path))
        time.sleep(.01)


def verify_db(library, path):
    d = Database(library, path)
    try:
        result = {key: d.scalar(sql) for key, sql in {
            "integrity": "PRAGMA integrity_check;",
            "rows": "SELECT COUNT(*) FROM mqtt_event_outbox;",
            "distinctIds": "SELECT COUNT(DISTINCT event_id) FROM mqtt_event_outbox;",
            "pending": "SELECT COUNT(*) FROM mqtt_event_outbox WHERE sent=0;",
            "pendingBytes": "SELECT COALESCE(SUM(LENGTH(topic)+LENGTH(payload)),0) FROM mqtt_event_outbox WHERE sent=0;",
            "statsPending": "SELECT COALESCE(SUM(pending_count),0) FROM mqtt_event_outbox_stats;",
            "statsBytes": "SELECT COALESCE(SUM(pending_bytes),0) FROM mqtt_event_outbox_stats;",
            "states": "SELECT COUNT(*) FROM mqtt_event_state;",
            "minId": "SELECT MIN(id) FROM mqtt_event_outbox;",
            "maxId": "SELECT MAX(id) FROM mqtt_event_outbox;",
            "pageSize": "PRAGMA page_size;",
            "cacheSize": "PRAGMA cache_size;",
            "mmapSize": "PRAGMA mmap_size;",
        }.items()}
        result["valid"] = (result["integrity"] == "ok" and result["rows"] == result["distinctIds"]
                           and result["pending"] == result["statsPending"]
                           and result["pendingBytes"] == result["statsBytes"])
        return result
    finally:
        d.close()


def workload(args, library, profile, name, load, repeat, seconds=None):
    folder = args.output / (name + "-" + load + "-" + str(repeat))
    folder.mkdir()
    db = folder / "outbox.db"
    seconds = seconds or args.seconds
    workers, logs, results = [], [], []
    start = time.monotonic()
    peak_wal = 0
    try:
        for role, tag, gap in [("producer", "p0", 100 if load == "fixed" else 0),
                               ("producer", "p1", 100 if load == "fixed" else 0),
                               ("replay", "replay", 0), ("reader", "reader", 5)]:
            output = folder / (tag + ".json")
            log = open(folder / (tag + ".log"), "w")
            logs.append(log)
            command = [str(args.binary), str(library), str(db), profile, role, str(output),
                       str(seconds), str(gap), tag]
            p = subprocess.Popen(command, stdout=log, stderr=log)
            workers.append(p)
            results.append(output)
            wait_file(str(output) + ".ready", p)
        Path(str(db) + ".go").touch()
        end = time.monotonic() + seconds + 30
        while any(p.poll() is None for p in workers):
            if time.monotonic() > end:
                raise TimeoutError("workload deadline")
            wal = Path(str(db) + "-wal")
            try:
                peak_wal = max(peak_wal, wal.stat().st_size)
            except FileNotFoundError:
                pass
            if shutil.disk_usage(folder).free < 2 * 1024**3:
                raise RuntimeError("less than 2GiB free; stopping benchmark")
            time.sleep(.02)
        data = [json.loads(p.read_text()) for p in results]
        expected_version = "3.31.1" if name.startswith("old-") else "3.53.4"
        expected_journal, expected_sync = profile.split("-")
        profile_verified = all(w["version"] == expected_version and w["journal"] == expected_journal
                               and w["synchronous"] == (2 if expected_sync == "full" else 1)
                               for w in data)
        if not profile_verified:
            raise RuntimeError("requested SQLite version/profile did not load; comparison invalid")
        accepted = sum(x["events"] for x in data if x["role"] == "producer")
        before_drain = verify_db(library, db)
        drained = folder / "drain.json"
        subprocess.run([str(args.binary), str(library), str(db), profile, "drain", str(drained),
                        "90", "0", "drain"], check=True, timeout=120, capture_output=True)
        final = verify_db(library, db)
        replayed = sum(x["events"] for x in data if x["role"] == "replay")
        committed_sent = int(before_drain["rows"]) - int(before_drain["pending"])
        drain = json.loads(drained.read_text())
        valid = (profile_verified and all(p.returncode == 0 for p in workers)
                 and final["valid"] and int(final["rows"]) == accepted and int(final["pending"]) == 0
                 and accepted == committed_sent + drain["events"] and int(final["states"]) == 32
                 and sum(x["errors"] for x in data) == 0)
        result = dict(name=name, load=load, repeat=repeat, profile=profile,
                      durationSeconds=seconds, elapsedSeconds=time.monotonic() - start,
                      workers=data, peakWalBytes=peak_wal, mainBytes=db.stat().st_size,
                      beforeDrain=before_drain, final=final, drain=drain, valid=valid,
                      profileVerified=profile_verified, replayApiReturned=replayed,
                      replayCommitted=committed_sent, returnCodes=[p.returncode for p in workers])
        dump(folder / "result.json", result)
        print(json.dumps({k: result[k] for k in ("name", "load", "repeat", "valid", "peakWalBytes")}), flush=True)
        return result
    finally:
        for p in workers:
            if p.poll() is None:
                p.kill()
            p.wait()
        for log in logs:
            log.close()


def probes(library, profile, folder):
    folder.mkdir()
    path = folder / "locks.db"
    wal = profile.startswith("wal")
    sync = "FULL" if profile.endswith("full") else "NORMAL"
    d = Database(library, path)
    d.execute("PRAGMA journal_mode=" + ("WAL" if wal else "DELETE"))
    d.execute("PRAGMA synchronous=" + sync)
    d.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB); INSERT INTO t VALUES(1,'committed');")
    other = Database(library, path)
    other.execute("PRAGMA synchronous=" + sync)
    d.execute("BEGIN; SELECT * FROM t;")
    other.execute("BEGIN IMMEDIATE; INSERT INTO t VALUES(2,'second');")
    blocked_commit = other.execute("COMMIT;", False)
    if blocked_commit["rc"]:
        other.execute("ROLLBACK;")
    d.execute("ROLLBACK;")
    d.execute("BEGIN IMMEDIATE;")
    writer_busy = other.execute("BEGIN IMMEDIATE;", False)
    d.execute("ROLLBACK;")
    result = dict(readerCommit=blocked_commit, writerBusy=writer_busy,
                  sameLibraryObject=d.lib is other.lib, sameLibraryHandle=d.lib._handle == other.lib._handle,
                  version=d.scalar("SELECT sqlite_version();"))
    if wal:
        d.execute("BEGIN; SELECT * FROM t;")
        other.execute("INSERT INTO t(data) VALUES('new');")
        result["snapshotUpgrade"] = d.execute("INSERT INTO t(data) VALUES('upgrade');", False)
        d.execute("ROLLBACK;")
        d.execute("BEGIN; SELECT * FROM t;")
        for _ in range(200):
            other.execute("INSERT INTO t(data) VALUES(zeroblob(4096));")
        result["heldReaderCheckpoint"] = other.execute("PRAGMA wal_checkpoint(PASSIVE);")
        result["heldReaderWalBytes"] = Path(str(path) + "-wal").stat().st_size
        d.execute("ROLLBACK;")
        result["releasedCheckpoint"] = other.execute("PRAGMA wal_checkpoint(TRUNCATE);")
        result["releasedWalBytes"] = Path(str(path) + "-wal").stat().st_size
    other.close()
    pages = int(d.scalar("PRAGMA page_count;"))
    d.execute("PRAGMA max_page_count=" + str(pages + 4))
    d.execute("BEGIN IMMEDIATE;")
    result["full"] = d.execute("INSERT INTO t(data) VALUES(zeroblob(1048576));", False)
    d.execute("ROLLBACK;", False)
    d.execute("PRAGMA max_page_count=" + str(pages + 256))
    d.execute("BEGIN IMMEDIATE; INSERT INTO t(data) VALUES('after full'); COMMIT;")
    result["afterFullIntegrity"] = d.scalar("PRAGMA integrity_check;")
    d.close()
    crashdb = folder / "crash.db"
    process = subprocess.Popen([sys.executable, __file__, "--crash", str(library), str(crashdb), profile])
    try:
        wait_file(str(crashdb) + ".ready", process)
        process.kill()
        process.wait(timeout=10)
        d = Database(library, crashdb)
        result["crashIntegrity"] = d.scalar("PRAGMA integrity_check;")
        result["crashRows"] = d.execute("SELECT id FROM t ORDER BY id;")["rows"]
        d.close()
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
    result["valid"] = (blocked_commit["rc"] == (0 if wal else 5) and writer_busy["rc"] == 5
                       and result["full"]["rc"] == 13 and result["afterFullIntegrity"] == "ok"
                       and result["crashIntegrity"] == "ok" and result["crashRows"] == [["1"], ["2"]])
    if wal:
        checkpoint = result["heldReaderCheckpoint"]["rows"][0]
        result["valid"] &= (result["snapshotUpgrade"]["extended"] == 517
                            and int(checkpoint[1]) > int(checkpoint[2])
                            and result["releasedWalBytes"] == 0)
    dump(folder / "probes.json", result)
    return result


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--crash":
        library, path, profile = sys.argv[2:]
        d = Database(library, path)
        d.execute("PRAGMA journal_mode=" + ("WAL" if profile.startswith("wal") else "DELETE"))
        d.execute("PRAGMA synchronous=" + ("FULL" if profile.endswith("full") else "NORMAL"))
        d.execute("CREATE TABLE t(id INTEGER PRIMARY KEY); INSERT INTO t VALUES(1);")
        d.execute("BEGIN IMMEDIATE; INSERT INTO t VALUES(2); COMMIT;")
        d.execute("BEGIN IMMEDIATE; INSERT INTO t VALUES(3);")
        Path(path + ".ready").touch()
        time.sleep(60)
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--new-library", type=Path, required=True)
    parser.add_argument("--old-library", default="/usr/lib/aarch64-linux-gnu/libsqlite3.so.0")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--soak-seconds", type=int, default=120)
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    args.new_library = args.new_library.resolve(strict=True)
    args.old_library = str(Path(args.old_library).resolve(strict=True))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    matrix = [(args.old_library, "delete-normal", "old-delete-normal")]
    matrix += [(args.new_library, p, "new-" + p) for p in
               ("delete-normal", "delete-full", "wal-normal", "wal-full")]
    order = [(lib, profile, name, load, repeat) for lib, profile, name in matrix
             for load in ("fixed", "saturation") for repeat in range(args.repeats)]
    random.Random(20260907).shuffle(order)
    results = []
    for item in order:
        results.append(workload(args, *item))
    probe_results = {name: probes(lib, profile, args.output / (name + "-probes"))
                     for lib, profile, name in matrix}
    if args.soak_seconds:
        for profile in ("delete-normal", "wal-full"):
            results.append(workload(args, args.new_library, profile, "new-" + profile,
                                    "fixed", "soak", args.soak_seconds))
    summary = dict(results=results, probes=probe_results,
                   valid=all(x["valid"] for x in results) and all(x["valid"] for x in probe_results.values()))
    dump(args.output / "summary.json", summary)
    print("ALL_VALID=" + str(summary["valid"]), flush=True)


if __name__ == "__main__":
    main()
