#!/usr/bin/env python3
"""Extract a compact legacy workset without modifying the full archive.

preflight is an ONLINE ADVISORY, not a consistent snapshot or cutover receipt.
extract requires --offline --source-frozen: the operator has stopped ALL
accessors, drained their queues, prevented restart, and protected the archive
and its directory from writes/replacement. Read-only permissions and flock
alone do not fence privileged or non-cooperating writers. This tool never
changes permissions, checkpoints the source, or starts/stops services.

Each source query has a time budget and its own ended read transaction. Offline
extraction performs one complete audit while copying into an unpublished stage.
Source physical hashes/identities and data_version are checked before and after;
target row/state digests must match that audit. Online evidence is never reused
as a substitute for the complete offline scan.
The manifest is published before the DB, both without overwrite. An interrupted
publication may leave a manifest alone; ONLY the pair with matching SHA256 is
a candidate for the separate migrate tool. Neither output authorizes deployment.
OTA-idle, actual routing/owners, history migration and compatible rollback are
separate operator gates. No production credentials/configuration are inputs.
"""

import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import sys
import tempfile
import time

import event_store_migrate as migration


EVENT = migration.EVENT
STATE = migration.STATE
EC = migration.EVENT_COLUMNS
SC = migration.STATE_COLUMNS
MAX_GENERATION = (1 << 63) - 2
MAX_ROW_ID = (1 << 63) - 2
TOOL_REVISION = "r3-explicit-rollback-schema"
SELECTION = "sent=0 OR event_type='ota_status'"
EVENT_DDL = """CREATE TABLE mqtt_event_outbox(
id INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT,
target_id TEXT NOT NULL DEFAULT 'main',claim_token TEXT,claim_until INTEGER,
event_type TEXT NOT NULL,topic TEXT NOT NULL,payload TEXT NOT NULL,
event_ts INTEGER NOT NULL,event_month TEXT NOT NULL,created_at INTEGER NOT NULL,
sent INTEGER NOT NULL DEFAULT 0,sent_at INTEGER,
retry_count INTEGER NOT NULL DEFAULT 0,last_error TEXT)"""
STATE_DDL = """CREATE TABLE mqtt_event_state(
state_key TEXT PRIMARY KEY,event_type TEXT NOT NULL,point_index INTEGER NOT NULL,
alarm_type TEXT NOT NULL,active INTEGER NOT NULL,value REAL NOT NULL,
quality INTEGER NOT NULL,source_ts INTEGER NOT NULL,lifecycle TEXT NOT NULL,
updated_at INTEGER NOT NULL)"""
STATS_DDL = """CREATE TABLE mqtt_event_outbox_stats(
target_id TEXT NOT NULL,event_type TEXT NOT NULL,
pending_count INTEGER NOT NULL DEFAULT 0 CHECK(pending_count>=0),
pending_bytes INTEGER NOT NULL DEFAULT 0 CHECK(pending_bytes>=0),
updated_at INTEGER NOT NULL,PRIMARY KEY(target_id,event_type)) WITHOUT ROWID"""
META_DDL = "CREATE TABLE mqtt_event_outbox_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)"
TABLES = {EVENT: EVENT_DDL, STATE: STATE_DDL,
          "mqtt_event_outbox_stats": STATS_DDL, "mqtt_event_outbox_meta": META_DDL,
          "sqlite_sequence": "CREATE TABLE sqlite_sequence(name,seq)"}
INDEXES = {
    "idx_mqtt_event_outbox_event_target": "CREATE UNIQUE INDEX idx_mqtt_event_outbox_event_target ON mqtt_event_outbox(event_id,target_id) WHERE event_id IS NOT NULL AND event_id<>''",
    "idx_mqtt_event_outbox_pending": "CREATE INDEX idx_mqtt_event_outbox_pending ON mqtt_event_outbox(sent,event_ts,id)",
    "idx_mqtt_event_outbox_target_pending": "CREATE INDEX idx_mqtt_event_outbox_target_pending ON mqtt_event_outbox(target_id,sent,event_type,event_ts,id)",
    "idx_mqtt_event_outbox_claim": "CREATE INDEX idx_mqtt_event_outbox_claim ON mqtt_event_outbox(target_id,sent,claim_until,event_type,event_ts,id)",
    "idx_mqtt_event_outbox_cleanup": "CREATE INDEX idx_mqtt_event_outbox_cleanup ON mqtt_event_outbox(sent,event_month)",
    migration.DELIVERY_INDEX: migration.DELIVERY_INDEX_SQL,
}
# Only these exact, derived-cache triggers may be omitted. A familiar name with
# a different body is rejected; no SQL from the source is executed on the target.
TRIGGERS = {
    "mqtt_event_outbox_stats_insert": """CREATE TRIGGER mqtt_event_outbox_stats_insert
AFTER INSERT ON mqtt_event_outbox WHEN NEW.sent=0 AND (
COALESCE((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done'),'0')='1' OR
NEW.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_cap') AS INTEGER),0))
BEGIN INSERT OR IGNORE INTO mqtt_event_outbox_stats(target_id,event_type,pending_count,pending_bytes,updated_at)
VALUES(NEW.target_id,NEW.event_type,0,0,NEW.created_at);
UPDATE mqtt_event_outbox_stats SET pending_count=pending_count+1,
pending_bytes=pending_bytes+length(NEW.topic)+length(NEW.payload),updated_at=NEW.created_at
WHERE target_id=NEW.target_id AND event_type=NEW.event_type; END""",
    "mqtt_event_outbox_stats_sent": """CREATE TRIGGER mqtt_event_outbox_stats_sent
AFTER UPDATE OF sent ON mqtt_event_outbox WHEN OLD.sent=0 AND NEW.sent=1 AND (
COALESCE((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done'),'0')='1' OR
NEW.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_cap') AS INTEGER),0) OR
NEW.id<=COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_hw') AS INTEGER),0))
BEGIN UPDATE mqtt_event_outbox_stats SET pending_count=pending_count-1,
pending_bytes=pending_bytes-length(OLD.topic)-length(OLD.payload),
updated_at=COALESCE(NEW.sent_at,NEW.created_at)
WHERE target_id=OLD.target_id AND event_type=OLD.event_type; END""",
    "mqtt_event_outbox_stats_delete": """CREATE TRIGGER mqtt_event_outbox_stats_delete
AFTER DELETE ON mqtt_event_outbox WHEN OLD.sent=0 AND (
COALESCE((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_done'),'0')='1' OR
OLD.id>COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_cap') AS INTEGER),0) OR
OLD.id<=COALESCE(CAST((SELECT value FROM mqtt_event_outbox_meta WHERE key='stats_migrate_hw') AS INTEGER),0))
BEGIN UPDATE mqtt_event_outbox_stats SET pending_count=pending_count-1,
pending_bytes=pending_bytes-length(OLD.topic)-length(OLD.payload),updated_at=OLD.created_at
WHERE target_id=OLD.target_id AND event_type=OLD.event_type; END""",
}


# Closed specification from event_store_migrate.py and the EventStore constructor
# in src/mqtt_event_outbox.cpp. Never populated from a source DB/schema artifact.
ROLLBACK_TABLES = {
    "event_store_identity": migration.IDENTITY_SQL,
    "event_store_state_version": migration.VERSION_SQL,
    "event_journal_meta": migration.JOURNAL_SCHEMA[0],
    "event_local_journal": migration.JOURNAL_SCHEMA[2],
    "event_history_projection_cursor": migration.JOURNAL_SCHEMA[4],
    "event_store_migration_id_map": "CREATE TABLE event_store_migration_id_map(source_kind TEXT NOT NULL,source_row_id INTEGER NOT NULL,target_id TEXT NOT NULL,original_event_id TEXT,event_id TEXT NOT NULL,PRIMARY KEY(source_kind,source_row_id,target_id))",
    "event_store_migration_manifest": "CREATE TABLE event_store_migration_manifest(lineage TEXT PRIMARY KEY,baseline_sha256 TEXT NOT NULL)",
    "event_store_producer": "CREATE TABLE event_store_producer(producer_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,epoch INTEGER NOT NULL CHECK(epoch>0),sequence INTEGER NOT NULL CHECK(sequence>=0),request TEXT NOT NULL,receipt TEXT NOT NULL)",
    "event_store_sender_registry": "CREATE TABLE event_store_sender_registry(sender_id TEXT PRIMARY KEY,scope TEXT NOT NULL)",
    "event_store_sender": "CREATE TABLE event_store_sender(sender_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,epoch INTEGER NOT NULL CHECK(epoch>0),sequence INTEGER NOT NULL CHECK(sequence>=0),request TEXT NOT NULL,receipt TEXT NOT NULL,last_op TEXT NOT NULL)",
    "event_store_claim_batch": "CREATE TABLE event_store_claim_batch(sender_id TEXT PRIMARY KEY,epoch INTEGER NOT NULL,claim_token TEXT NOT NULL UNIQUE,boot_id TEXT NOT NULL,lease_until INTEGER NOT NULL)",
    "event_store_claim_item": "CREATE TABLE event_store_claim_item(sender_id TEXT NOT NULL,row_id INTEGER NOT NULL UNIQUE,event_id TEXT NOT NULL,target_id TEXT NOT NULL,state TEXT NOT NULL,PRIMARY KEY(sender_id,row_id))",
}
ROLLBACK_INDEXES = {
    "event_store_state_owner": "CREATE INDEX event_store_state_owner ON event_store_state_version(producer_id,state_key)",
    "idx_event_local_journal_kind_ts_id": "CREATE INDEX idx_event_local_journal_kind_ts_id ON event_local_journal(kind,ts,id)",
}
ROLLBACK_TRIGGERS = {"event_local_journal_immutable": migration.JOURNAL_SCHEMA[3]}


class ExtractError(RuntimeError):
    pass


def require(ok, message):
    if not ok:
        raise ExtractError(message)


def encoded(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, allow_nan=False,
                      separators=(",", ":")).encode("ascii")


def typed_row(row):
    def cell(value):
        if value is None:
            return ["null"]
        if isinstance(value, int):
            return ["integer", str(value)]
        if isinstance(value, float):
            require(math.isfinite(value), "nonfinite SQLite value")
            return ["real", value.hex()]
        if isinstance(value, str):
            return ["text", value]
        if isinstance(value, bytes):
            return ["blob", value.hex()]
        raise ExtractError("unsupported SQLite value")
    return encoded([cell(v) for v in row]) + b"\n"


def integer(value, name, low=0, high=MAX_ROW_ID):
    require(type(value) is int and low <= value <= high, "invalid " + name)
    return value


def decimal(text):
    require(isinstance(text, str) and re.fullmatch(r"0|[1-9][0-9]*", text),
            "noncanonical generation/index")
    require(len(text) <= 19, "generation/index too large")
    return integer(int(text), "generation/index", high=MAX_GENERATION)


def sql_tokens(sql):
    # Fail-closed lexer for comparison, NOT a general SQL parser or executor.
    pattern = r"\s+|'(?:''|[^'])*'|\"(?:\"\"|[^\"])*\"|`[^`]*`|\[[^\]]*\]|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|!=|==|<>|>=|<=|[(),.;=+*>-]"
    tokens, offset = [], 0
    for match in re.finditer(pattern, sql):
        require(match.start() == offset, "unsupported schema SQL token")
        offset = match.end()
        token = match.group()
        if token.isspace():
            continue
        if token[0] in '"`[':
            token = token[1:-1]
            require(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", token), "complex quoted identifier")
        token = {"!=": "<>", "==": "="}.get(token, token)
        tokens.append(token if token.startswith("'") else token.lower())
    require(offset == len(sql), "unsupported schema SQL suffix")
    if tokens and tokens[-1] == ";":
        tokens.pop()
    for i in range(len(tokens) - 2):
        if tokens[i:i + 3] == ["if", "not", "exists"]:
            del tokens[i:i + 3]
            break
    return tokens


def table_signature(sql):
    tokens = sql_tokens(sql)
    require(tokens[:2] == ["create", "table"] and tokens[3] == "(", "unsupported table DDL")
    parts, part, depth = [], [], 0
    for i in range(4, len(tokens)):
        token = tokens[i]
        if token == ")" and depth == 0:
            parts.append(part)
            return tokens[:3], sorted(parts), tokens[i + 1:]
        if token == "," and depth == 0:
            parts.append(part)
            part = []
        else:
            part.append(token)
            depth += (token == "(") - (token == ")")
    raise ExtractError("unterminated table DDL")


def physical(path, hashes=False, online=False):
    result = {}
    for suffix in ("", "-wal", "-journal"):
        candidate = Path(str(path) + suffix)
        if not os.path.lexists(candidate):
            continue
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            if suffix and online:
                continue  # A live DELETE journal can disappear between probes.
            raise
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                "source/sidecar must be regular, unlinked and not a symlink")
        require(online or suffix != "-journal" or info.st_size == 0,
                "rollback journal present; recover separately before audit")
        if suffix and not info.st_size:
            continue
        item = dict(device=info.st_dev, inode=info.st_ino, size=info.st_size,
                    mtime_ns=info.st_mtime_ns)
        if hashes:
            item["sha256"] = migration.sha256(candidate)
            after = candidate.stat()
            require((after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) ==
                    (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns),
                    "source changed during hashing")
        result[suffix] = item
    require("" in result, "source missing")
    shm = Path(str(path) + "-shm")
    try:
        info = shm.lstat()
    except FileNotFoundError:
        info = None
    if info is not None:
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "unsafe source SHM")
    return result


class Reader:
    def __init__(self, path, args):
        self.path, self.args = path, args
        self.db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True,
                                  timeout=0, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA query_only=ON")
        self.db.execute("PRAGMA trusted_schema=OFF")
        if hasattr(self.db, "setlimit"):
            self.db.setlimit(sqlite3.SQLITE_LIMIT_LENGTH, 1024 * 1024)
        self.pages = 0
        self.busy_retries = 0

    def close(self):
        self.db.close()

    def query(self, sql, params=()):
        require(not self.db.in_transaction, "source transaction leaked")
        retry_deadline = time.monotonic() + self.args.busy_retry_ms / 1000
        while True:
            deadline = time.monotonic() + self.args.page_timeout_ms / 1000
            self.db.set_progress_handler(lambda: int(time.monotonic() > deadline), 1000)
            try:
                self.db.execute("BEGIN")
                rows = self.db.execute(sql, params).fetchall()
                self.db.execute("COMMIT")
                self.pages += 1
                return rows
            except sqlite3.Error as error:
                if sqlite_primary_code(error) not in (5, 6):
                    raise
                if time.monotonic() >= retry_deadline:
                    raise ExtractError("source BUSY/LOCKED retry budget exhausted") from error
            finally:
                # Disable the expired progress handler BEFORE rollback. In
                # particular, never sleep while retaining a read transaction.
                self.db.set_progress_handler(None, 0)
                if self.db.in_transaction:
                    self.db.execute("ROLLBACK")
            self.busy_retries += 1
            remaining = retry_deadline - time.monotonic()
            if remaining <= 0:
                raise ExtractError("source BUSY/LOCKED retry budget exhausted")
            time.sleep(min(0.025, remaining))
    def watermark(self):
        sequence_names = (EVENT, "event_local_journal" if getattr(self.args, "allow_rollback_schema", False) else EVENT)
        row = self.query(f"SELECT (SELECT coalesce(max(id),0) FROM {EVENT}) AS max_id,"
                         "(SELECT seq FROM sqlite_sequence WHERE name=?) AS seq,"
                         "(SELECT count(*) FROM sqlite_sequence) AS sequences,"
                         "(SELECT count(DISTINCT name) FROM sqlite_sequence) AS distinct_sequences,"
                         "(SELECT count(*) FROM sqlite_sequence WHERE name IS NULL OR name NOT IN (?,?)) AS unknown_sequences",
                         (EVENT, *sequence_names))[0]
        integer(row["max_id"], "max row id")
        require(row["sequences"] == row["distinct_sequences"] and not row["unknown_sequences"],
                "unknown/duplicate sqlite_sequence entries")
        seq = 0 if row["seq"] is None else integer(row["seq"], "sqlite_sequence")
        require(seq >= row["max_id"], "sqlite_sequence below max id")
        return dict(max_id=row["max_id"], sqlite_sequence=row["seq"],
                    highwater=seq, data_version=self.query("PRAGMA data_version")[0][0])


def sqlite_primary_code(error):
    code = getattr(error, "sqlite_errorcode", None)
    if isinstance(code, int):
        return code & 255
    # Python 3.9/3.10 lack sqlite_errorcode; only exact native SQLite messages
    # are accepted. INTERRUPT, corruption and arbitrary OperationalError fail.
    return {"database is locked": 5, "database is busy": 5,
            "database table is locked": 6, "database schema is locked": 6}.get(str(error))


def audit_schema(reader):
    rows = reader.query("SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY type,name LIMIT 129")
    require(len(rows) <= 128, "too many schema objects")
    allow_rollback = getattr(reader.args, "allow_rollback_schema", False)
    tables = dict(TABLES, **(ROLLBACK_TABLES if allow_rollback else {}))
    indexes = dict(INDEXES, **(ROLLBACK_INDEXES if allow_rollback else {}))
    triggers = dict(TRIGGERS, **(ROLLBACK_TRIGGERS if allow_rollback else {}))
    # SQLite derives exact implicit-index names/owners from trusted local DDL.
    # Source SQL is compared only, never executed, even in this scratch DB.
    with contextlib.closing(sqlite3.connect(":memory:")) as reference:
        for name, ddl in tables.items():
            if name != "sqlite_sequence":
                reference.execute(ddl)
        for ddl in list(indexes.values()) + list(triggers.values()):
            reference.execute(ddl)
        expected = {(r[0], r[1]): (r[2], r[3]) for r in reference.execute(
            "SELECT type,name,tbl_name,sql FROM sqlite_master")}
    names = set()
    for row in rows:
        kind, name, table, sql = row
        spec = expected.get((kind, name))
        if kind == "table":
            require(name in tables, "unknown table: " + name)
            compare = sql_tokens if name in ROLLBACK_TABLES else table_signature
            require(table == name and compare(sql) == compare(tables[name]), "table schema mismatch: " + name)
            names.add(name)
        elif kind == "index" and sql is None:
            require(spec == (table, None),
                    "unknown implicit index")
        elif kind == "index":
            require(name in indexes and spec[0] == table and sql_tokens(sql) == sql_tokens(indexes[name]),
                    "unknown or altered index: " + name)
        elif kind == "trigger":
            require(name in triggers and spec[0] == table and sql_tokens(sql) == sql_tokens(triggers[name]),
                    "unknown or altered trigger: " + name)
        else:
            raise ExtractError("unsupported schema object: " + name)
    require({EVENT, STATE, "sqlite_sequence"} <= names, "missing legacy tables")
    digest = hashlib.sha256()
    for row in rows:
        digest.update(typed_row(row))
    extensions = [dict(row) for row in rows if row[1] in ROLLBACK_TABLES or
                  row[1] in ROLLBACK_INDEXES or row[1] in ROLLBACK_TRIGGERS or
                  row[0] == "index" and row[3] is None and row[2] in ROLLBACK_TABLES]
    omitted = [dict(row) for row in rows if row[1] in ROLLBACK_TABLES or
               row[1] in ROLLBACK_INDEXES or row[0] == "trigger" or
               row[2] not in {EVENT, STATE, "sqlite_sequence"}]
    return dict(sha256=digest.hexdigest(), objects=[dict(row) for row in rows],
                allow_rollback_schema=allow_rollback, source_extension_objects=extensions,
                not_copied_objects=omitted,
                omitted_derived_tables=sorted(names & {"mqtt_event_outbox_stats", "mqtt_event_outbox_meta"}),
                archive_policy="unselected sent history and all old journal/extension data remain in the original archive; no deletion; extract requires external freeze; preflight does not freeze",
                target_schema="canonical legacy tables; indexes rebuilt; no source SQL executed")


def read_states(reader):
    states, cursor = {}, ""
    digest = hashlib.sha256()
    while True:
        rows = reader.query(f"SELECT {','.join(SC)} FROM {STATE} WHERE state_key>? COLLATE BINARY "
                            "ORDER BY state_key COLLATE BINARY LIMIT ?", (cursor, reader.args.page_size))
        if not rows:
            break
        for row in rows:
            s = dict(row)
            require(len(states) < reader.args.max_states, "state count limit exceeded")
            index = integer(s["point_index"], "state index", high=(1 << 32) - 1)
            integer(s["active"], "active", high=1)
            integer(s["quality"], "quality", high=(1 << 31) - 1)
            integer(s["source_ts"], "source_ts")
            integer(s["updated_at"], "updated_at")
            require(type(s["value"]) in (int, float) and math.isfinite(s["value"]), "invalid state value")
            if s["event_type"] == "change":
                key, prefix = "change:" + str(index), "value:"
                require(s["alarm_type"] == "", "change state alarm_type is not empty")
            else:
                require(s["event_type"] == "alarm" and s["alarm_type"] in ("high", "low"), "invalid state kind")
                key = "alarm:" + str(index) + ":" + s["alarm_type"]
                prefix = "raised:" if s["active"] else "cleared:"
            require(s["state_key"] == key and key not in states, "invalid/duplicate state key")
            life = s["lifecycle"]
            require(isinstance(life, str) and life.startswith(prefix), "invalid lifecycle: " + key)
            generation = decimal(life[len(prefix):])
            states[key] = dict(row=s, generation=generation, max_event_generation=None,
                               last_row_id=None, last_event_id=None, event_rows=0)
            digest.update(typed_row(row))
        cursor = rows[-1]["state_key"]
    # NULL/empty keys are excluded by keyset pagination, so check them explicitly.
    require(not reader.query(f"SELECT 1 FROM {STATE} WHERE state_key IS NULL OR state_key='' LIMIT 1"),
            "empty/null state key")
    return states, digest.hexdigest()


def check_event_header(row):
    integer(row["sent"], "sent", high=1)
    require(isinstance(row["target_id"], str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,96}", row["target_id"]),
            "invalid target")
    require(row["event_type"] in ("ota_status", "alarm", "change"), "unknown event type")


def check_event(row, states, machine):
    integer(row["id"], "event row id", low=1)
    check_event_header(row)
    kind, event_id = row["event_type"], row["event_id"]
    if kind == "ota_status":
        require(event_id is None or isinstance(event_id, str) and "\0" not in event_id and
                len(event_id.encode("utf-8")) <= 256, "invalid management ID")
        return
    require(kind in ("alarm", "change"), "unknown event type")
    # Anonymous legacy records never occupied a modern eventId uniqueness key.
    # Preserve pending rows byte-for-byte for migrate's stable legacy ID mapping;
    # sent rows stay in the archive. Neither category proves a generation bound.
    if event_id is None or event_id == "":
        return
    prefix = kind + ":v1:" + machine + ":"
    require(isinstance(event_id, str) and event_id.startswith(prefix), "unrecognized business event ID")
    parts = event_id[len(prefix):].split(":")
    require(len(parts) == (2 if kind == "change" else 3), "malformed business event ID")
    index = decimal(parts[0])
    require(index < (1 << 32), "event point index too large")
    generation = decimal(parts[-1])
    key = kind + ":" + str(index)
    if kind == "alarm":
        require(parts[1] in ("high", "low"), "invalid alarm type in ID")
        key += ":" + parts[1]
    require(key in states, "missing state for historical event: " + key)
    state = states[key]
    require(state["generation"] >= generation, "lifecycle behind historical generation: " + key)
    state["max_event_generation"] = max(state["max_event_generation"] or 0, generation)
    state["last_row_id"], state["last_event_id"] = row["id"], event_id
    state["event_rows"] += 1


def check_selected(row):
    for key in ("topic", "payload", "event_month"):
        require(isinstance(row[key], str) and "\0" not in row[key], "invalid selected " + key)
    require(0 < len(row["topic"].encode("utf-8")) <= 4096 and
            len(row["payload"].encode("utf-8")) <= 256 * 1024, "selected topic/payload exceeds IPC bounds")
    for key in ("event_ts", "created_at", "retry_count"):
        integer(row[key], key, low=1 if key == "event_ts" else 0)
    for key in ("claim_until", "sent_at"):
        if row[key] is not None:
            integer(row[key], key)
    for key in ("claim_token", "last_error"):
        require(row[key] is None or isinstance(row[key], str), "invalid selected " + key)


def capacity(args, directory):
    require(shutil.disk_usage(directory).free >= args.reserve_free_bytes + 6 * args.max_workset_bytes,
            "insufficient free space for configured workset/migration/rollback budget")


def audit(reader, sink=None, page_hook=None):
    schema = audit_schema(reader)
    start = reader.watermark()
    states, state_digest = read_states(reader)
    selected_digest = hashlib.sha256()
    selected_count = total = selected_bytes = 0
    anonymous = {"sent_archived": 0, "pending_preserved": 0}
    groups, cursor = {}, 0
    lengths = "+".join("coalesce(length(CAST(" + c + " AS BLOB)),0)"
                       for c in ("event_id", "target_id", "claim_token", "topic", "payload", "event_month", "last_error"))
    byte_expression = f"CASE WHEN ({SELECTION}) THEN ({lengths}) ELSE 0 END"
    anonymous_predicate = "(event_id IS NULL OR event_id='')"
    def add_group(row, count):
        group = (row["target_id"], row["event_type"], row["sent"])
        require(group in groups or len(groups) < 4096, "too many event groups")
        groups[group] = groups.get(group, 0) + count
    if sink is not None:
        sink.executemany(f"INSERT INTO {STATE}({','.join(SC)}) VALUES({','.join('?' for _ in SC)})",
                         ([s["row"][c] for c in SC] for s in states.values()))
    while cursor < start["max_id"]:
        # Numeric windows bound SQL aggregation work even for a dense anonymous
        # archive. Seek first so arbitrarily large rowid holes cost one query.
        first = reader.query(f"SELECT id FROM {EVENT} WHERE id>? AND id<=? ORDER BY id LIMIT 1",
                             (cursor, start["max_id"]))
        if not first:
            break
        next_id = min(first[0]["id"] + reader.args.page_size - 1, start["max_id"])
        bounds = (first[0]["id"], next_id)
        archived = reader.query(f"SELECT target_id,event_type,sent,count(*) AS n,"
                                f"sum({byte_expression}) AS selected_bytes FROM {EVENT} "
                                f"WHERE id>=? AND id<=? AND {anonymous_predicate} "
                                "GROUP BY target_id,event_type,sent,typeof(target_id),typeof(event_type),typeof(sent)",
                                bounds)
        rows = reader.query(f"SELECT id,event_id,target_id,event_type,sent,"
                            f"{byte_expression} AS selected_bytes FROM {EVENT} "
                            f"WHERE id>=? AND id<=? AND NOT {anonymous_predicate} ORDER BY id",
                            bounds)
        require(sum(row["selected_bytes"] for row in archived) +
                sum(row["selected_bytes"] for row in rows) <= 8 * 1024 * 1024,
                "selected page exceeds 8 MiB; reduce --page-size")
        selected = reader.query(f"SELECT {','.join(EC)} FROM {EVENT} WHERE id>=? AND id<=? "
                                f"AND ({SELECTION}) ORDER BY id LIMIT ?",
                                (*bounds, reader.args.page_size + 1))
        require(len(selected) <= reader.args.page_size, "page interval changed during selection")
        # No source transaction spans processing, target writes, or page hooks.
        for row in archived:
            check_event_header(row)
            if row["event_type"] in ("alarm", "change"):
                anonymous["sent_archived" if row["sent"] else "pending_preserved"] += row["n"]
            add_group(row, row["n"])
        for row in rows:
            check_event(row, states, reader.args.machine_code)
            add_group(row, 1)
        for row in selected:
            check_selected(row)
            data = typed_row(row)
            selected_digest.update(data)
            selected_bytes += len(data)
        selected_count += len(selected)
        total += len(rows) + sum(row["n"] for row in archived)
        if sink is not None:
            capacity(reader.args, Path(reader.args.target).parent)
            sink.executemany(f"INSERT INTO {EVENT}({','.join(EC)}) VALUES({','.join('?' for _ in EC)})",
                             (tuple(row) for row in selected))
            pages = sink.execute("PRAGMA page_count").fetchone()[0]
            page_size = sink.execute("PRAGMA page_size").fetchone()[0]
            require(pages * page_size <= reader.args.max_workset_bytes, "workset size limit exceeded")
        cursor = next_id
        if page_hook:
            page_hook(reader, cursor)
    require(not reader.query(f"SELECT id FROM {EVENT} WHERE id<=0 LIMIT 1"), "nonpositive event id")
    end = reader.watermark()
    require(audit_schema(reader)["sha256"] == schema["sha256"], "schema changed during audit")
    return dict(schema=schema, watermark_start=start, watermark_end=end,
                observation_changed=start != end, scanned_through=cursor, scanned_count=total,
                selected_count=selected_count, selected_typed_bytes=selected_bytes,
                selected_sha256=selected_digest.hexdigest(), scanned_ids_sha256=None,
                scanned_ids_digest_policy="omitted; anonymous SQL groups validated, all nonempty IDs checked; offline physical hashes fence source bytes",
                state_count=len(states), states_sha256=state_digest,
                anonymous_legacy=anonymous,
                not_copied_outbox_rows=total - selected_count,
                generation_coverage=[dict(state_key=k, lifecycle=v["row"]["lifecycle"],
                    generation=v["generation"], max_event_generation=v["max_event_generation"],
                    last_row_id=v["last_row_id"], last_event_id=v["last_event_id"],
                    event_rows=v["event_rows"], covered=True) for k, v in sorted(states.items())],
                groups=[dict(target=k[0], event_type=k[1], sent=k[2], count=v)
                        for k, v in sorted(groups.items())])


def target_digests(db, page_size):
    result = {}
    for table, columns, key, initial in ((EVENT, EC, "id", 0), (STATE, SC, "state_key", "")):
        cursor, count, digest = initial, 0, hashlib.sha256()
        while True:
            rows = db.execute(f"SELECT {','.join(columns)} FROM {table} WHERE {key}>? ORDER BY {key} LIMIT ?",
                              (cursor, page_size)).fetchall()
            if not rows:
                break
            for row in rows:
                digest.update(typed_row(row))
            count += len(rows)
            cursor = rows[-1][0]
        result[table] = (count, digest.hexdigest())
    return result


def source_unchanged(reader, before, version):
    require(physical(reader.path, hashes=True) == before, "frozen source changed")
    require(reader.watermark()["data_version"] == version, "frozen source data_version changed")


def run(args, page_hook=None):
    started = time.monotonic()
    require(1 <= args.page_size <= 4096 and 1 <= args.max_states <= 100000,
            "invalid page/state bounds")
    require(1 <= args.page_timeout_ms <= 10000, "invalid page time budget")
    require(0 <= args.busy_retry_ms <= 30000, "invalid BUSY retry budget")
    require(re.fullmatch(r"[A-Za-z0-9_.-]{1,96}", args.machine_code), "invalid machine code")
    path = migration.checked_source(args.source)
    initial = physical(path, online=args.command == "preflight")
    base = dict(format="event-store-workset-v1", tool_revision=TOOL_REVISION, mode=args.command, source=str(path),
                machine_code=args.machine_code, selection=SELECTION, source_preserved=True,
                deployment_accepted=False, required_external_gates=["all accessors stopped and restart fenced",
                "active routes/rules/owners unchanged; disabled states remain disabled; no index reuse",
                "OTA pending journal empty and no in-flight OTA", "history migration and reader routing",
                "compatible rollback rehearsal and runtime capacity limits"])
    with contextlib.closing(Reader(path, args)) as reader:
        if args.command == "preflight":
            result = audit(reader, page_hook=page_hook)
            base.update(result, status="ONLINE_ADVISORY_REAUDIT_OFFLINE", consistent_snapshot=False,
                        origin_start=initial, origin_end=physical(path, online=True), source_queries=reader.pages,
                        busy_retries=reader.busy_retries,
                        source_hashes="not taken online; mutable file hashes are not a frozen receipt")
            base["elapsed_seconds"] = time.monotonic() - started
            return base
        require(args.offline and args.source_frozen, "extract requires --offline --source-frozen")
        require(args.max_workset_bytes >= 16384 and args.reserve_free_bytes >= 0, "invalid disk budget")
        # Permission check is a tripwire, not a claim to fence root. Directory
        # protection and service shutdown remain explicit operator obligations.
        for suffix in initial:
            candidate = Path(str(path) + suffix)
            readonly_mount = os.name == "posix" and bool(os.statvfs(candidate).f_flag & os.ST_RDONLY)
            require(readonly_mount or not candidate.stat().st_mode & 0o222,
                    "source/sidecar is writable; freeze externally before extract")
        target = Path(args.target).absolute()
        require(target.parent.is_dir() and not target.is_symlink(), "invalid target directory/path")
        target = target.parent.resolve() / target.name
        require(target.name not in ("", ".", ".."), "invalid target filename")
        args.target = str(target)
        manifest = Path(str(target) + ".manifest.json")
        migration.absent(target)
        require(not os.path.lexists(manifest), "manifest already exists")
        for suffix in ("", "-wal", "-shm", "-journal"):
            require(target != Path(str(path) + suffix) and manifest != Path(str(path) + suffix), "source/output alias")
        capacity(args, target.parent)
        with path.open("rb") as lock:
            if os.name == "posix":
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            frozen_watermark = reader.watermark()
            hash_start = time.monotonic()
            before = physical(path, hashes=True)
            first_hash_seconds = time.monotonic() - hash_start
            require(initial == {k: {f: v for f, v in item.items() if f != "sha256"}
                                for k, item in before.items()}, "source changed before freeze")
            require((os.fstat(lock.fileno()).st_dev, os.fstat(lock.fileno()).st_ino) ==
                    (path.stat().st_dev, path.stat().st_ino), "source replaced before freeze")
            require(reader.watermark() == frozen_watermark, "source changed while hashing freeze")
            version = frozen_watermark["data_version"]
            with tempfile.TemporaryDirectory(prefix=".event-workset-", dir=target.parent) as temporary:
                stage = Path(temporary) / "workset.db"
                with contextlib.closing(sqlite3.connect(stage, isolation_level=None)) as output:
                    output.executescript(EVENT_DDL + ";" + STATE_DDL + ";")
                    output.execute("PRAGMA synchronous=FULL")
                    output.execute("BEGIN IMMEDIATE")
                    audit_start = time.monotonic()
                    result = audit(reader, sink=output, page_hook=page_hook)
                    audit_seconds = time.monotonic() - audit_start
                    require(result["watermark_start"] == frozen_watermark and
                            result["watermark_end"] == frozen_watermark,
                            "frozen source changed during audit")
                    seq = frozen_watermark["sqlite_sequence"]
                    output.execute("DELETE FROM sqlite_sequence WHERE name=?", (EVENT,))
                    if seq is not None:
                        output.execute("INSERT INTO sqlite_sequence VALUES(?,?)", (EVENT, seq))
                    for ddl in INDEXES.values():
                        output.execute(ddl)
                    output.execute("COMMIT")
                    migration.integrity(output)
                    digests = target_digests(output, args.page_size)
                    require(digests[EVENT] == (result["selected_count"], result["selected_sha256"]),
                            "selected target digest mismatch")
                    require(digests[STATE] == (result["state_count"], result["states_sha256"]),
                            "state target digest mismatch")
                    actual = output.execute("SELECT seq FROM sqlite_sequence WHERE name=?", (EVENT,)).fetchone()
                    require((actual[0] if actual else None) == seq, "target sequence mismatch")
                require(stage.stat().st_size <= args.max_workset_bytes, "indexed workset size limit exceeded")
                capacity(args, target.parent)
                target_hash = migration.sha256(stage)
                final_hash_start = time.monotonic()
                source_unchanged(reader, before, version)
                final_hash_seconds = time.monotonic() - final_hash_start
                base.update(result, status="OFFLINE_WORKSET_CANDIDATE", consistent_snapshot=True,
                            origin=before, source_queries=reader.pages, target=str(target),
                            busy_retries=reader.busy_retries,
                            target_sha256=target_hash, target_bytes=stage.stat().st_size,
                            manifest=str(manifest), source_freeze="operator fence + readonly tripwire + cooperative flock + hashes",
                            disk_budget=dict(max_workset_bytes=args.max_workset_bytes,
                                reserve_free_bytes=args.reserve_free_bytes, future_copy_budget_factor=6),
                            publication_contract="manifest plus matching DB required; not deployment acceptance")
                base["timing"] = dict(first_hash_seconds=first_hash_seconds,
                                      audit_with_copy_seconds=audit_seconds,
                                      final_hash_seconds=final_hash_seconds,
                                      before_publication_seconds=time.monotonic() - started,
                                      source_audit_passes=1, source_hash_passes=2,
                                      source_hashed_bytes=2 * sum(v["size"] for v in before.values()))
                receipt = Path(temporary) / "manifest.json"
                receipt.write_bytes(encoded(base) + b"\n")
                migration.publish(receipt, manifest)
                migration.publish(stage, target)
                require(migration.sha256(target) == target_hash and target.stat().st_nlink == 1,
                        "published target verification failed; inspect candidate before retry")
                base["publication"] = "published"
                return base


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    for command in ("preflight", "extract"):
        c = sub.add_parser(command)
        c.add_argument("--source", required=True)
        c.add_argument("--allow-rollback-schema", action="store_true",
                       help="accept only exact known migration/runtime rollback schema; extensions stay in source")
        c.add_argument("--machine-code", required=True)
        c.add_argument("--page-size", type=int, default=256)
        c.add_argument("--page-timeout-ms", type=int, default=500)
        c.add_argument("--busy-retry-ms", type=int, default=3000,
                       help="per-query BUSY/LOCKED retry window, always outside a transaction")
        c.add_argument("--max-states", type=int, default=10000)
        if command == "extract":
            c.add_argument("--target", required=True)
            c.add_argument("--offline", action="store_true")
            c.add_argument("--source-frozen", action="store_true")
            c.add_argument("--max-workset-bytes", type=int, default=256 * 1024 * 1024)
            c.add_argument("--reserve-free-bytes", type=int, default=1024 * 1024 * 1024)
    return p


if __name__ == "__main__":
    try:
        print(json.dumps(run(parser().parse_args()), ensure_ascii=True, sort_keys=True))
    except (ExtractError, migration.MigrationError, sqlite3.Error, OSError, ValueError) as error:
        print(json.dumps(dict(status="FAILED", deployment_accepted=False,
                              error=str(error)), ensure_ascii=True), file=sys.stderr)
        sys.exit(1)
