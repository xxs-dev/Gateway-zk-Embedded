#!/usr/bin/env python3
"""Offline, non-destructive EventStore migration and compatible rollback.

Only Python's standard library is required. See the Chinese operations document.
"""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import sys
import tempfile
import time
import uuid


EVENT = "mqtt_event_outbox"
STATE = "mqtt_event_state"
DELIVERY_INDEX = "idx_event_store_delivery_order"
DELIVERY_INDEX_COLUMNS = ["target_id", "event_type", "sent", "id"]
DELIVERY_INDEX_SQL = ("CREATE INDEX IF NOT EXISTS idx_event_store_delivery_order "
                      "ON mqtt_event_outbox(target_id,event_type,sent,id)")
EVENT_COLUMNS = ("id event_id target_id claim_token claim_until event_type topic payload "
                 "event_ts event_month created_at sent sent_at retry_count last_error").split()
STATE_COLUMNS = ("state_key event_type point_index alarm_type active value quality "
                 "source_ts lifecycle updated_at").split()
IMMUTABLE = "event_id target_id event_type topic payload event_ts event_month created_at".split()
OPTIONAL = {"event_id": "TEXT", "target_id": "TEXT NOT NULL DEFAULT 'main'",
            "claim_token": "TEXT", "claim_until": "INTEGER"}
ID_MAP = "event_store_migration_id_map"
HISTORY_COLUMNS = ("id event_id point_index ts alarm_type active threshold value quality stale "
                   "persist_value gateway_code device_code point_code").split()
IDENTITY_SQL = """CREATE TABLE event_store_identity(
id INTEGER PRIMARY KEY CHECK(id=1),store_id TEXT NOT NULL,
config_generation TEXT NOT NULL,schema_version INTEGER NOT NULL)"""
VERSION_SQL = """CREATE TABLE event_store_state_version(
state_key TEXT PRIMARY KEY,producer_id TEXT NOT NULL,
version INTEGER NOT NULL CHECK(version>0))"""
HISTORY_META_SQL = ("CREATE TABLE alarm_projection_meta(id INTEGER PRIMARY KEY CHECK(id=1),"
                    "projection_id TEXT NOT NULL,store_id TEXT NOT NULL,journal_generation TEXT NOT NULL,"
                    "last_contiguous_journal_id INTEGER NOT NULL CHECK(last_contiguous_journal_id>=0))")
JOURNAL_SCHEMA = (
    """CREATE TABLE event_journal_meta(id INTEGER PRIMARY KEY CHECK(id=1),
    schema_version INTEGER NOT NULL CHECK(schema_version=1),journal_generation TEXT NOT NULL)""",
    "INSERT OR IGNORE INTO event_journal_meta VALUES(1,1,lower(hex(randomblob(16))))",
    """CREATE TABLE event_local_journal(id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('alarm','change')),
    event_id TEXT NOT NULL UNIQUE CHECK(length(event_id)>0),point_index INTEGER NOT NULL,
    ts INTEGER NOT NULL,alarm_type TEXT NOT NULL,active INTEGER NOT NULL,
    threshold REAL NOT NULL,value REAL NOT NULL,quality INTEGER NOT NULL,stale INTEGER NOT NULL,
    persist_value TEXT NOT NULL,gateway_code TEXT NOT NULL,device_code TEXT NOT NULL,
    point_code TEXT NOT NULL,state_version INTEGER NOT NULL,config_generation TEXT NOT NULL)""",
    """CREATE TRIGGER event_local_journal_immutable BEFORE UPDATE ON event_local_journal
    BEGIN SELECT RAISE(ABORT,'immutable journal'); END""",
    """CREATE TABLE event_history_projection_cursor(id INTEGER PRIMARY KEY CHECK(id=1),
    projection_id TEXT NOT NULL CHECK(projection_id='alarm-history-v1'),
    projected_through INTEGER NOT NULL CHECK(projected_through>=0),
    cleaned_through INTEGER NOT NULL CHECK(cleaned_through>=0))""",
    "INSERT OR IGNORE INTO event_history_projection_cursor VALUES(1,'alarm-history-v1',0,0)",
)


class MigrationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise MigrationError(message)


def identifier(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.:-]{1,96}", value)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_set(path):
    # SHM and empty sidecars carry no persistent records. Opening a clean WAL DB
    # may create an empty WAL even without a single write; do not call that data loss.
    return {suffix: sha256(Path(str(path) + suffix))
            for suffix in ("", "-wal", "-journal") if Path(str(path) + suffix).exists()
            and (not suffix or Path(str(path) + suffix).stat().st_size > 0)}


def checked_source(value):
    path = Path(value).absolute()
    require(not path.is_symlink(), "source symlink is forbidden")
    path = path.resolve(strict=True)
    info = path.stat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
            "source must be a regular file with one hard link")
    return path


def absent(path):
    require(not any(os.path.lexists(str(path) + s)
                    for s in ("", "-wal", "-shm", "-journal")),
            "output or SQLite sidecar already exists: " + str(path))


def baseline_digest(db):
    digest = hashlib.sha256()
    for table, order in ((EVENT, "id"), (STATE, "state_key"),
                         ("event_store_state_version", "state_key"),
                         ("event_journal_meta", "id"), ("event_local_journal", "id"),
                         ("event_history_projection_cursor", "id")):
        for row in db.execute(f"SELECT * FROM {table} ORDER BY {order}"):
            digest.update(json.dumps(dict(row), sort_keys=True, ensure_ascii=True,
                                     allow_nan=False).encode("ascii") + b"\n")
    if ID_MAP in tables(db):
        digest.update(ID_MAP.encode("ascii"))
        for row in db.execute(f"SELECT * FROM {ID_MAP} ORDER BY source_kind,source_row_id,target_id"):
            digest.update(canonical(dict(row)))
    return digest.hexdigest()


def canonical(value):
    try:
        return json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError) as error:
        raise MigrationError("unsupported value for deterministic identity: " + str(error)) from error


def create_id_map(db):
    db.execute(f"CREATE TABLE IF NOT EXISTS {ID_MAP}(source_kind TEXT NOT NULL,"
               "source_row_id INTEGER NOT NULL,target_id TEXT NOT NULL,original_event_id TEXT,"
               "event_id TEXT NOT NULL,PRIMARY KEY(source_kind,source_row_id,target_id))")


def stable_id(namespace, row):
    return "legacy:v1:" + hashlib.sha256(canonical([namespace, row])).hexdigest()


def fill_outbox_ids(db, store_id):
    create_id_map(db)
    count = 0
    for row in db.execute(f"SELECT * FROM {EVENT} WHERE event_id IS NULL OR event_id='' ORDER BY id"):
        generated = stable_id("outbox:" + store_id, {k: row[k] for k in ["id"] + IMMUTABLE})
        require(not db.execute(f"SELECT 1 FROM {EVENT} WHERE event_id=?", (generated,)).fetchone(),
                "generated outbox identity collision")
        db.execute(f"INSERT INTO {ID_MAP} VALUES(?,?,?,?,?)",
                   ("outbox", row["id"], row["target_id"], row["event_id"], generated))
        db.execute(f"UPDATE {EVENT} SET event_id=? WHERE id=?", (generated, row["id"]))
        count += 1
    require(not db.execute(f"SELECT 1 FROM {EVENT} WHERE typeof(event_id)<>'text' "
                           "OR length(CAST(event_id AS BLOB))>256 OR instr(event_id,char(0))>0 LIMIT 1").fetchone(),
            "event_id is not a valid IPC identity")
    return count


def connect(path, mode="rw"):
    db = sqlite3.connect(path.as_uri() + "?mode=" + mode, uri=True,
                         timeout=0, isolation_level=None)
    db.row_factory = sqlite3.Row
    return db


@contextlib.contextmanager
def frozen(path):
    # flock matches the Linux runtime, but legacy SQLite clients ignore it.
    with path.open("rb") as handle:
        if os.name == "posix":
            import fcntl
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        journal = Path(str(path) + "-journal")
        require(not journal.exists() or journal.stat().st_size == 0,
                "rollback journal present: recover and stop SQLite offline before migration")
        before = file_set(path)
        # Keep a read-only WAL participant alive until the RW connection closes.
        # This avoids last-writer-close checkpointing/removing the source WAL.
        reader = connect(path, "ro")
        db = None
        try:
            reader.execute("SELECT count(*) FROM sqlite_master").fetchone()
            db = connect(path)
            db.execute("BEGIN IMMEDIATE")
            original = os.fstat(handle.fileno())
            require((original.st_dev, original.st_ino) ==
                    (path.stat().st_dev, path.stat().st_ino), "source replaced during open")
            require(file_set(path) == before, "source changed while acquiring SQLite lock")
            yield db, before
            require(file_set(path) == before, "source content changed during operation")
            require((original.st_dev, original.st_ino) ==
                    (path.stat().st_dev, path.stat().st_ino), "source file replaced")
        finally:
            if db is not None:
                db.close()  # Roll back the write reservation; no source SQL writes.
            reader.close()
        require(file_set(path) == before, "source changed on SQLite close")


def tables(db):
    return {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}


def columns(db, table):
    return [r[1] for r in db.execute('PRAGMA table_info("' + table + '")')]


def integrity(db):
    require([r[0] for r in db.execute("PRAGMA integrity_check")] == ["ok"],
            "SQLite integrity_check failed")


def validate(db):
    integrity(db)
    require({EVENT, STATE} <= tables(db), "missing legacy events/states tables")
    ec = set(columns(db, EVENT))
    require(set(EVENT_COLUMNS) - set(OPTIONAL) <= ec <= set(EVENT_COLUMNS),
            "unsupported event columns; explicit adapter required")
    require(set(columns(db, STATE)) == set(STATE_COLUMNS), "unsupported state columns")
    require(not db.execute(f"SELECT 1 FROM {EVENT} WHERE sent NOT IN (0,1) OR sent IS NULL LIMIT 1").fetchone(),
            "invalid sent status")
    if "target_id" in ec:
        require(not db.execute(f"SELECT 1 FROM {EVENT} WHERE target_id IS NULL OR target_id='' LIMIT 1").fetchone(),
                "empty target_id requires explicit repair")
    if "event_id" in ec:
        target = "target_id" if "target_id" in ec else "'main'"
        require(not db.execute(f"SELECT 1 FROM {EVENT} WHERE event_id IS NOT NULL AND event_id<>'' "
                               f"GROUP BY event_id,{target} HAVING count(*)>1 LIMIT 1").fetchone(),
                "duplicate event identity")


def normalize(db):
    for name, declaration in OPTIONAL.items():
        if name not in columns(db, EVENT):
            db.execute(f"ALTER TABLE {EVENT} ADD COLUMN {name} {declaration}")
    ensure_event_identity_index(db)


def ensure_event_identity_index(db):
    name = "idx_mqtt_event_outbox_event_target"
    db.execute(f"CREATE UNIQUE INDEX IF NOT EXISTS {name} "
               f"ON {EVENT}(event_id,target_id) WHERE event_id IS NOT NULL AND event_id<>''")
    entry = next((r for r in db.execute(f"PRAGMA index_list({EVENT})") if r[1] == name), None)
    require(entry is not None and entry[2] == 1 and entry[4] == 1,
            "event identity index definition conflict: expected unique partial outbox index")
    keys = [r for r in db.execute(f"PRAGMA index_xinfo({name})") if r[5]]
    require([r[2] for r in keys] == ["event_id", "target_id"] and
            all(r[3] == 0 and r[4] == "BINARY" for r in keys),
            "event identity index definition conflict: expected event_id,target_id BINARY ASC")
    definition = db.execute("SELECT sql FROM sqlite_master WHERE name=?", (name,)).fetchone()[0]
    predicate = re.split(r"\bWHERE\b", definition, flags=re.IGNORECASE)[-1]
    # Match the agreed predicate without stripping whitespace inside SQL literals.
    require(re.fullmatch(r"\s*event_id\s+IS\s+NOT\s+NULL\s+AND\s+event_id\s*<>\s*''\s*;?\s*",
                         predicate, flags=re.IGNORECASE) is not None,
            "event identity index definition conflict: unexpected partial predicate")


def ensure_order_index(db, table, name, fields):
    existing = db.execute("SELECT 1 FROM sqlite_master WHERE name=?", (name,)).fetchone()
    page_size = db.execute("PRAGMA page_size").fetchone()[0]
    before_pages = db.execute("PRAGMA page_count").fetchone()[0]
    before_free = db.execute("PRAGMA freelist_count").fetchone()[0]
    started = time.monotonic()
    db.execute(f"CREATE INDEX IF NOT EXISTS {name} ON {table}({','.join(fields)})")
    elapsed = time.monotonic() - started
    indexes = {row[1]: row for row in db.execute(f"PRAGMA index_list({table})")}
    entry = indexes.get(name)
    require(entry is not None and entry[2] == 0 and entry[4] == 0,
            name + " index must be non-unique and non-partial")
    keys = [row for row in db.execute(f"PRAGMA index_xinfo({name})") if row[5]]
    require([row[2] for row in keys] == fields and all(row[3] == 0 and row[4] == "BINARY" for row in keys),
            "index definition conflict: " + name + "; explicit offline repair required")
    after_pages = db.execute("PRAGMA page_count").fetchone()[0]
    after_free = db.execute("PRAGMA freelist_count").fetchone()[0]
    return {"name": name, "columns": fields,
            "created": existing is None, "build_seconds": elapsed,
            "database_growth_bytes": (after_pages - before_pages) * page_size,
            "used_page_growth_bytes": ((after_pages - after_free) - (before_pages - before_free)) * page_size,
            "space_scope": "observed page growth; excludes peak sort/journal/temp space and COMMIT time"}


def ensure_delivery_index(db):
    return ensure_order_index(db, EVENT, DELIVERY_INDEX, DELIVERY_INDEX_COLUMNS)


def snapshot(source, target):
    # A separate read connection can back up while BEGIN IMMEDIATE fences writers.
    deadline = time.monotonic() + 3600
    def progress(status, remaining, total):
        # SQLite's stable primary codes; Python before 3.11 does not export these names.
        if status in (5, 6) or time.monotonic() > deadline:
            raise MigrationError("backup busy/locked or exceeded one-hour deadline")
    with contextlib.closing(connect(source, "ro")) as reader:
        with contextlib.closing(sqlite3.connect(target)) as writer:
            reader.backup(writer, pages=1024, progress=progress, sleep=0)
            writer.execute("PRAGMA journal_mode=DELETE")
            writer.execute("PRAGMA synchronous=FULL")
            integrity(writer)
    sync_file(target)


def sync_file(path):
    with path.open("r+b") as stream:
        os.fsync(stream.fileno())


def sync_dir(path):
    if os.name == "posix":
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def publish(stage, target):
    absent(target)
    sync_file(stage)
    # link is an atomic, no-clobber publication; rename/replace would race with creators.
    os.link(stage, target)
    stage.unlink()
    sync_dir(target.parent)


def durable_copy(source, directory, name):
    destination = directory / name
    digest = sha256(source)
    if destination.exists():
        require(not destination.is_symlink() and destination.stat().st_nlink == 1 and
                sha256(destination) == digest, "existing backup hash mismatch")
        require(not any(os.path.lexists(str(destination) + suffix)
                        for suffix in ("-wal", "-shm", "-journal")), "existing backup has SQLite sidecars")
        return destination
    with tempfile.TemporaryDirectory(prefix=".event-store-backup-", dir=directory) as temp:
        stage = Path(temp) / "backup.db"
        shutil.copyfile(source, stage)
        publish(stage, destination)
    require(sha256(destination) == digest, "backup verification failed")
    return destination


def migrate(db, store_id, generation, owners):
    require(identifier(store_id) and identifier(generation), "invalid store identity/generation")
    require(not any(t.startswith("event_store_") for t in tables(db)),
            "source already contains EventStore metadata")
    require(not {"event_journal_meta", "event_local_journal", "event_history_projection_cursor"} & tables(db),
            "partial/existing journal schema requires explicit recovery, not legacy migration")
    require(isinstance(owners, dict) and all(identifier(v) for v in owners.values()),
            "owners must map each exact state_key to a valid producer_id")
    keys = {r[0] for r in db.execute(f"SELECT state_key FROM {STATE}")}
    require(keys == set(owners), "state owner map must cover exactly all state keys")
    require(len(set(owners.values())) <= 64, "too many state owners")
    normalize(db)
    assigned_ids = fill_outbox_ids(db, store_id)
    db.execute(IDENTITY_SQL)
    db.execute("INSERT INTO event_store_identity VALUES(1,?,?,1)", (store_id, generation))
    db.execute(VERSION_SQL)
    db.executemany("INSERT INTO event_store_state_version VALUES(?,?,1)", owners.items())
    db.execute("CREATE INDEX event_store_state_owner ON event_store_state_version(producer_id,state_key)")
    claims = db.execute(f"SELECT count(*) FROM {EVENT} WHERE claim_token IS NOT NULL").fetchone()[0]
    db.execute(f"UPDATE {EVENT} SET claim_token=NULL,claim_until=NULL")
    for statement in JOURNAL_SCHEMA:
        db.execute(statement)
    db.execute("CREATE TABLE event_store_migration_manifest(lineage TEXT PRIMARY KEY,baseline_sha256 TEXT NOT NULL)")
    db.execute("INSERT INTO event_store_migration_manifest VALUES(?,?)",
               (str(uuid.uuid4()), baseline_digest(db)))
    return {"released_legacy_claims": claims, "state_versions_initialized": len(keys),
            "assigned_event_ids": assigned_ids}


def journal_identity(db):
    require({"event_journal_meta", "event_local_journal", "event_history_projection_cursor"} <= tables(db),
            "missing journal schema")
    rows = list(db.execute("SELECT id,schema_version,journal_generation FROM event_journal_meta"))
    require(len(rows) == 1 and rows[0][0] == 1 and rows[0][1] == 1 and rows[0][2],
            "invalid journal identity")
    cursors = list(db.execute("SELECT id,projection_id,projected_through,cleaned_through FROM event_history_projection_cursor"))
    require(len(cursors) == 1 and cursors[0][0] == 1 and cursors[0][1] == "alarm-history-v1"
            and cursors[0][2] >= 0 and cursors[0][3] == 0,
            "invalid projection cursor; journal retention must remain disabled (cleaned_through=0)")
    return rows[0][2]


def identity(db):
    require("event_store_identity" in tables(db), "missing EventStore identity")
    rows = list(db.execute("SELECT * FROM event_store_identity"))
    require(len(rows) == 1 and rows[0]["id"] == 1 and rows[0]["schema_version"] == 1,
            "unsupported EventStore identity/schema")
    return tuple(rows[0])


def rollback(db, baseline):
    require(identity(db) == identity(baseline), "rollback identity/generation mismatch")
    require(journal_identity(db) == journal_identity(baseline), "journal generation mismatch")
    manifests = []
    for source in (db, baseline):
        require("event_store_migration_manifest" in tables(source), "missing migration lineage")
        rows = list(source.execute("SELECT * FROM event_store_migration_manifest"))
        require(len(rows) == 1, "invalid migration lineage")
        manifests.append(tuple(rows[0]))
    require(manifests[0] == manifests[1], "rollback lineage mismatch")
    require(baseline_digest(baseline) == manifests[1][1], "cutover baseline was modified")
    for source in (db, baseline):
        require(not source.execute(f"SELECT 1 FROM {EVENT} WHERE event_id IS NULL OR event_id='' LIMIT 1").fetchone(),
                "anonymous rollback identity: recreate the offline migration/baseline with stable IDs")
    if ID_MAP in tables(baseline):
        create_id_map(db)
        for row in baseline.execute(f"SELECT * FROM {ID_MAP}"):
            current = db.execute(f"SELECT * FROM {ID_MAP} WHERE source_kind=? AND source_row_id=? AND target_id=?",
                                 tuple(row)[:3]).fetchone()
            require(current is None or tuple(current) == tuple(row), "rollback ID mapping conflict")
            if current is None:
                db.execute(f"INSERT INTO {ID_MAP} VALUES(?,?,?,?,?)", tuple(row))
    require(set(columns(db, EVENT)) == set(EVENT_COLUMNS) and
            set(columns(baseline, EVENT)) == set(EVENT_COLUMNS), "rollback requires normalized schema")
    ensure_event_identity_index(db)
    for source in (db, baseline):
        require("event_store_state_version" in tables(source), "missing state versions")
        require(not source.execute(f"SELECT 1 FROM {STATE} s LEFT JOIN event_store_state_version v "
                                   "ON s.state_key=v.state_key WHERE v.version IS NULL OR v.version<1 LIMIT 1").fetchone(),
                "state without positive version")
    added = matched = 0
    for old in baseline.execute(f"SELECT * FROM {EVENT} ORDER BY id"):
        current = db.execute(f"SELECT * FROM {EVENT} WHERE event_id=? AND target_id=?",
                             (old["event_id"], old["target_id"])).fetchone()
        if current:
            require(all(current[k] == old[k] for k in IMMUTABLE),
                    "event immutable-content conflict at baseline row " + str(old["id"]))
            require(not (old["sent"] and not current["sent"]), "sent status regression")
            matched += 1
        else:
            names = [k for k in EVENT_COLUMNS if k != "id"]
            values = [None if k in ("claim_token", "claim_until") else old[k] for k in names]
            db.execute(f"INSERT INTO {EVENT} ({','.join(names)}) VALUES ({','.join('?' for _ in names)})", values)
            added += 1
    restored = 0
    for old in baseline.execute(f"SELECT * FROM {STATE}"):
        key = old["state_key"]
        ov = baseline.execute("SELECT producer_id,version FROM event_store_state_version WHERE state_key=?", (key,)).fetchone()
        nv = db.execute("SELECT producer_id,version FROM event_store_state_version WHERE state_key=?", (key,)).fetchone()
        current = db.execute(f"SELECT * FROM {STATE} WHERE state_key=?", (key,)).fetchone()
        if current:
            require(nv and nv[0] == ov[0] and nv[1] >= ov[1], "state owner/version conflict")
            require(nv[1] != ov[1] or all(current[k] == old[k] for k in STATE_COLUMNS), "same-version state conflict")
        else:
            require(nv is None, "orphan state version conflict")
            db.execute(f"INSERT INTO {STATE} ({','.join(STATE_COLUMNS)}) VALUES ({','.join('?' for _ in STATE_COLUMNS)})",
                       [old[k] for k in STATE_COLUMNS])
            db.execute("INSERT INTO event_store_state_version VALUES(?,?,?)", (key, ov[0], ov[1]))
            restored += 1
    # Current receipts, claims, journals and projection metadata remain byte-for-byte SQL values.
    return {"events_merged": added, "events_already_present": matched, "states_restored": restored}


def validate_history(db):
    integrity(db)
    require("alarm_events" in tables(db), "missing alarm_events")
    require(set(HISTORY_COLUMNS) - {"event_id"} <= set(columns(db, "alarm_events")) <= set(HISTORY_COLUMNS),
            "unsupported alarm history columns")
    if "event_id" in columns(db, "alarm_events"):
        require(not db.execute("SELECT 1 FROM alarm_events WHERE event_id IS NOT NULL AND event_id<>'' "
                               "GROUP BY event_id HAVING count(*)>1 LIMIT 1").fetchone(), "duplicate history event identity")


def prepare_history(db, store):
    validate_history(db)
    if "event_id" not in columns(db, "alarm_events"):
        db.execute("ALTER TABLE alarm_events ADD COLUMN event_id TEXT")
    require(not db.execute("SELECT 1 FROM alarm_events WHERE event_id IS NOT NULL AND event_id<>'' "
                           "GROUP BY event_id HAVING count(*)>1 LIMIT 1").fetchone(), "duplicate history event identity")
    db.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_alarm_events_event_id ON alarm_events(event_id) "
               "WHERE event_id IS NOT NULL AND event_id<>''")
    index = ensure_order_index(db, "alarm_events", "idx_alarm_events_ts_id", ["ts", "id"])
    identity_index = {row[1]: row for row in db.execute("PRAGMA index_list(alarm_events)")}.get("idx_alarm_events_event_id")
    require(identity_index is not None and identity_index[2] == 1 and identity_index[4] == 1,
            "history identity index must be unique and partial")
    keys = [r for r in db.execute("PRAGMA index_xinfo(idx_alarm_events_event_id)") if r[5]]
    definition = db.execute("SELECT sql FROM sqlite_master WHERE name='idx_alarm_events_event_id'").fetchone()[0]
    predicate = re.split(r"\bWHERE\b", definition, flags=re.IGNORECASE)[-1]
    require(len(keys) == 1 and keys[0][2] == "event_id" and keys[0][4] == "BINARY"
            and re.sub(r"\s+", "", predicate).lower().rstrip(";") == "event_idisnotnullandevent_id<>''",
            "history identity index definition conflict")
    expected = (1, "alarm-history-v1", identity(store)[1], journal_identity(store))
    db.execute(HISTORY_META_SQL.replace("CREATE TABLE ", "CREATE TABLE IF NOT EXISTS ", 1))
    meta = list(db.execute("SELECT * FROM alarm_projection_meta"))
    if not meta:
        db.execute("INSERT INTO alarm_projection_meta VALUES(?,?,?,?,0)", expected)
    else:
        require(len(meta) == 1 and tuple(meta[0])[:4] == expected, "history projection identity mismatch")
        through = store.execute("SELECT coalesce(max(id),0) FROM event_local_journal").fetchone()[0]
        require(0 <= meta[0][4] <= through, "history watermark outside retained journal")
    create_id_map(db)
    return index


def rollback_history_digest(db):
    digest = hashlib.sha256()
    for table, order in (("alarm_events", "id"), (ID_MAP, "source_kind,source_row_id,target_id"),
                         ("sqlite_sequence", "name,seq")):
        digest.update(table.encode("ascii"))
        for row in db.execute(f"SELECT * FROM {table} ORDER BY {order}"):
            digest.update(canonical(dict(row)) + b"\n")
    return digest.hexdigest()


def validate_rollback_history(db):
    # Compare trusted metadata definitions; never replay source SQL. Dropping the
    # old meta must not silently discard custom columns, indexes, or triggers.
    with contextlib.closing(sqlite3.connect(":memory:")) as reference:
        reference.execute(HISTORY_META_SQL)
        create_id_map(reference)
        for name in ("alarm_projection_meta", ID_MAP):
            expected = reference.execute("SELECT sql FROM sqlite_master WHERE name=?", (name,)).fetchone()[0]
            actual = db.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name=?", (name,)).fetchone()
            normalize_sql = lambda s: re.sub(r"\s+", "", s).lower()
            require(actual and normalize_sql(actual[0]) == normalize_sql(expected),
                    "rollback history metadata schema mismatch: " + name)
    require(not db.execute("SELECT 1 FROM sqlite_master WHERE tbl_name='alarm_projection_meta' "
                           "AND type<>'table' LIMIT 1").fetchone(), "custom projection metadata objects")
    meta = list(db.execute("SELECT * FROM alarm_projection_meta"))
    require(len(meta) == 1 and tuple(meta[0])[:2] == (1, "alarm-history-v1")
            and isinstance(meta[0][2], str) and bool(meta[0][2])
            and isinstance(meta[0][3], str) and bool(meta[0][3])
            and type(meta[0][4]) is int and meta[0][4] >= 0, "invalid rollback history metadata")
    require("event_id" in columns(db, "alarm_events"), "rollback history requires existing event_id")
    require(not db.execute("SELECT 1 FROM alarm_events WHERE event_id IS NULL OR event_id='' LIMIT 1").fetchone(),
            "rollback history contains anonymous event_id")
    return tuple(meta[0])


def import_history(output, legacy, store, history_id, initial, rebind=False):
    require(identifier(history_id), "invalid history-id: stable source namespace required")
    projected_source = "alarm_projection_meta" in tables(legacy)
    require(rebind or not initial or not projected_source, "history source must be legacy; use --projection for current history")
    if projected_source:
        old_meta = validate_rollback_history(legacy)
    if rebind:
        require(initial, "rollback history rebind cannot merge a projection")
        before = rollback_history_digest(legacy)
        require(rollback_history_digest(output) == before, "rollback history snapshot differs")
        output.execute("DROP TABLE alarm_projection_meta")
    index = prepare_history(output, store)
    if projected_source and not initial:
        for record in legacy.execute(f"SELECT * FROM {ID_MAP}"):
            mapping = tuple(record)
            prior = output.execute(f"SELECT * FROM {ID_MAP} WHERE source_kind=? AND source_row_id=? AND target_id=?",
                                   mapping[:3]).fetchone()
            require(prior is None or tuple(prior) == mapping, "rollback history source mapping conflict")
            if prior is None:
                output.execute(f"INSERT INTO {ID_MAP} VALUES(?,?,?,?,?)", mapping)
    imported = matched = assigned = 0
    has_id = "event_id" in columns(legacy, "alarm_events")
    kind = "history:" + history_id
    for record in legacy.execute("SELECT * FROM alarm_events ORDER BY id"):
        row = dict(record)
        original = row.get("event_id") if has_id else None
        row["event_id"] = original
        if original is None or original == "":
            row["event_id"] = stable_id(kind, row)
            mapping = (kind, row["id"], "", original, row["event_id"])
            prior = output.execute(f"SELECT * FROM {ID_MAP} WHERE source_kind=? AND source_row_id=? AND target_id=''",
                                   (kind, row["id"])).fetchone()
            require(prior is None or tuple(prior) == mapping, "history source row/mapping changed")
            if prior is None:
                require(not output.execute("SELECT 1 FROM alarm_events WHERE event_id=?", (row["event_id"],)).fetchone(),
                        "generated history identity collision")
                output.execute(f"INSERT INTO {ID_MAP} VALUES(?,?,?,?,?)", mapping)
            assigned += 1
        require(isinstance(row["event_id"], str) and 0 < len(row["event_id"].encode("utf-8")) <= 256
                and "\0" not in row["event_id"], "invalid history event_id")
        if initial:
            if not rebind:
                output.execute("UPDATE alarm_events SET event_id=? WHERE id=?", (row["event_id"], row["id"]))
            imported += 1
        else:
            prior = output.execute("SELECT * FROM alarm_events WHERE event_id=?", (row["event_id"],)).fetchone()
            if prior:
                require(all(prior[k] == row[k] for k in HISTORY_COLUMNS if k != "id"), "history immutable-content conflict")
                matched += 1
            else:
                names = HISTORY_COLUMNS[1:]
                output.execute(f"INSERT INTO alarm_events({','.join(names)}) VALUES({','.join('?' for _ in names)})",
                               [row[k] for k in names])
                imported += 1
    # Existing journal identities must agree before the runtime projector resumes.
    for row in store.execute("SELECT * FROM event_local_journal WHERE kind='alarm'"):
        prior = output.execute("SELECT * FROM alarm_events WHERE event_id=?", (row["event_id"],)).fetchone()
        if prior:
            require(all(prior[k] == row[k] for k in HISTORY_COLUMNS if k != "id"), "history/journal content conflict")
    watermark = output.execute("SELECT last_contiguous_journal_id FROM alarm_projection_meta").fetchone()[0]
    for row in store.execute("SELECT event_id FROM event_local_journal WHERE kind='alarm' AND id<=?", (watermark,)):
        require(output.execute("SELECT 1 FROM alarm_events WHERE event_id=?", (row[0],)).fetchone(),
                "projection watermark skips missing alarm history")
    require(not output.execute("SELECT 1 FROM alarm_events WHERE event_id IS NULL OR event_id='' LIMIT 1").fetchone(),
            "current projection contains anonymous rows; migrate that legacy history first")
    report = {"history_imported": imported, "history_matched": matched, "assigned_history_ids": assigned,
              "projection_watermark": watermark, "history_time_index": index,
              "rollback_history_rebound": rebind}
    if rebind:
        require(assigned == 0 and rollback_history_digest(output) == before,
                "rollback history rows/IDs/mapping/sequence changed")
        report.update(previous_projection_meta=old_meta,
                      rebound_projection_meta=tuple(output.execute("SELECT * FROM alarm_projection_meta").fetchone()),
                      preserved_rows_sha256=before)
    return report


def run_history(args):
    require(args.offline, "--offline is required: stop ALL accessors and prevent restart first")
    source, store = checked_source(args.source), checked_source(args.event_store)
    projection = checked_source(args.projection) if args.projection else None
    allow_rebind = getattr(args, "rebind_rollback_history", False)
    require(not (projection and allow_rebind), "--rebind-rollback-history conflicts with --projection")
    paths = [source, store] + ([projection] if projection else [])
    require(len(set(paths)) == len(paths), "history inputs must be independent files")
    target = Path(args.target).absolute()
    absent(target)
    target = target.resolve()
    backups = Path(args.backup_dir).absolute().resolve()
    require(target.parent.is_dir() and backups.is_dir(), "output and backup parent directories must exist")
    report = {"command": "history", "dry_run": args.dry_run, "sources": [], "target": str(target),
              "publication": "not published", "lock_scope": "SQLite writer reservation/flock; NOT proof of offline accessors"}
    with contextlib.ExitStack() as stack:
        locked = {p: stack.enter_context(frozen(p)) for p in sorted(paths)}
        validate_history(locked[source][0])
        validate(locked[store][0])
        with tempfile.TemporaryDirectory(prefix=".event-store-stage-", dir=target.parent) as temp:
            stage = Path(temp) / "history.db"
            for i, path in enumerate(locked):
                copy = Path(temp) / (str(i) + ".db")
                snapshot(path, copy)
                entry = {"path": str(path), "files_sha256": locked[path][1], "snapshot_sha256": sha256(copy),
                         "snapshot_bytes": copy.stat().st_size}
                if not args.dry_run:
                    entry["backup"] = str(durable_copy(copy, backups, path.name + "." + entry["snapshot_sha256"][:16] + ".backup.db"))
                report["sources"].append(entry)
                if path == (projection or source):
                    shutil.copyfile(copy, stage)
            started = time.monotonic()
            before_bytes = stage.stat().st_size
            with contextlib.closing(connect(stage)) as output:
                output.execute("PRAGMA synchronous=FULL")
                output.execute("BEGIN IMMEDIATE")
                rebind = allow_rebind and "alarm_projection_meta" in tables(locked[source][0])
                report.update(import_history(output, locked[source][0], locked[store][0], args.history_id, not projection, rebind))
                output.execute("COMMIT")
                integrity(output)
                report["history_rows"] = output.execute("SELECT count(*) FROM alarm_events").fetchone()[0]
            report.update(transform_seconds=time.monotonic() - started, database_growth_bytes=stage.stat().st_size - before_bytes,
                          target_bytes=stage.stat().st_size, target_sha256=sha256(stage))
            for path, (_, before) in locked.items():
                require(file_set(path) == before, "history source changed before publication")
                checked_source(path)
            if not args.dry_run:
                publish(stage, target)
                report["publication"] = "published"
    return report


def run(args):
    if args.command == "history":
        return run_history(args)
    require(args.offline, "--offline is required: stop ALL accessors and prevent restart first")
    source = checked_source(args.source)
    baseline = checked_source(args.baseline) if args.command == "rollback" else None
    require(baseline != source, "baseline must be independent of current source")
    absent(Path(args.target).absolute())
    target = Path(args.target).absolute().resolve()
    backup_dir = Path(args.backup_dir).absolute().resolve()
    require(target.parent.is_dir() and backup_dir.is_dir(), "output and backup parent directories must exist")
    absent(target)
    owners = json.loads(Path(args.state_owners).read_text(encoding="utf-8")) if args.command == "migrate" else None
    report = {"command": args.command, "dry_run": args.dry_run, "source": str(source),
              "target": str(target), "sqlite_version": sqlite3.sqlite_version,
              "lock_scope": "SQLite writer reservation; Linux database flock; NOT proof of offline accessors",
              "publication": "not published", "sources": []}
    with contextlib.ExitStack() as stack:
        locked = {}
        for path in sorted([source] + ([baseline] if baseline else [])):
            locked[path] = stack.enter_context(frozen(path))
            validate(locked[path][0])
        # Dry-run executes the identical transform in disposable staging, including conflicts.
        with tempfile.TemporaryDirectory(prefix=".event-store-stage-", dir=target.parent) as temp:
            stage = Path(temp) / "target.db"
            for index, path in enumerate(locked):
                copy = Path(temp) / ("source-" + str(index) + ".db")
                snapshot(path, copy)
                entry = {"path": str(path), "files_sha256": locked[path][1],
                         "snapshot_sha256": sha256(copy), "snapshot_bytes": copy.stat().st_size}
                if not args.dry_run:
                    destination = durable_copy(copy, backup_dir,
                        path.name + "." + entry["snapshot_sha256"][:16] + ".backup.db")
                    entry["backup"] = str(destination)
                report["sources"].append(entry)
                if path == source:
                    shutil.copyfile(copy, stage)
            with contextlib.closing(connect(stage)) as output:
                output.execute("PRAGMA synchronous=FULL")
                output.execute("BEGIN IMMEDIATE")
                if args.command == "migrate":
                    report.update(migrate(output, args.store_id, args.config_generation, owners))
                else:
                    report.update(rollback(output, locked[baseline][0]))
                report["delivery_index"] = ensure_delivery_index(output)
                report["journal_time_index"] = ensure_order_index(output, "event_local_journal",
                    "idx_event_local_journal_kind_ts_id", ["kind", "ts", "id"])
                output.execute("COMMIT")
                validate(output)
                report["events"] = output.execute(f"SELECT count(*) FROM {EVENT}").fetchone()[0]
                report["states"] = output.execute(f"SELECT count(*) FROM {STATE}").fetchone()[0]
            report["target_sha256"] = sha256(stage)
            report["target_bytes"] = stage.stat().st_size
            if args.command == "migrate" and not args.dry_run:
                report["cutover_baseline"] = str(durable_copy(stage, backup_dir,
                    target.name + "." + report["target_sha256"][:16] + ".baseline.db"))
            for path, (_, before) in locked.items():
                require(file_set(path) == before, "source changed before publication")
                checked_source(path)
            if not args.dry_run:
                publish(stage, target)
                report["publication"] = "published"
    return report


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    for command in ("migrate", "rollback", "history"):
        p = sub.add_parser(command)
        p.add_argument("--source", required=True)
        p.add_argument("--target", required=True)
        p.add_argument("--backup-dir", required=True)
        p.add_argument("--offline", action="store_true")
        p.add_argument("--dry-run", action="store_true")
        if command == "migrate":
            p.add_argument("--store-id", required=True)
            p.add_argument("--config-generation", required=True)
            p.add_argument("--state-owners", required=True)
        elif command == "rollback":
            p.add_argument("--baseline", required=True,
                           help="frozen migrated cutover baseline, not the raw legacy DB")
        else:
            p.add_argument("--event-store", required=True, help="offline current EventStore database")
            p.add_argument("--history-id", required=True, help="stable namespace for this legacy history source")
            p.add_argument("--projection", help="offline current projection; merge into a new target without losing increments")
            p.add_argument("--rebind-rollback-history", action="store_true",
                           help="explicitly rebind known rollback history metadata on the target copy; preserve existing IDs/rows/map; legacy inputs unchanged")
    return result


def main():
    try:
        print(json.dumps(run(parser().parse_args()), ensure_ascii=False, indent=2))
        return 0
    except (MigrationError, OSError, sqlite3.Error, ValueError) as error:
        print(json.dumps({"error": str(error), "status": "failed; inspect output paths before retry"}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
