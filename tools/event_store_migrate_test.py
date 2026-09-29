#!/usr/bin/env python3
"""Deterministic, temporary-database tests; no production services or CMake."""

import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

import event_store_migrate as m


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "legacy.db"
        self.target = self.root / "cutover.db"
        self.backups = self.root / "backups"
        self.backups.mkdir()
        self.owners = self.root / "owners.json"
        self.owners.write_text(json.dumps({"state-a": "producer-a"}), encoding="utf-8")
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.executescript("""
                CREATE TABLE mqtt_event_outbox(
                  id INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT,
                  target_id TEXT NOT NULL DEFAULT 'main',claim_token TEXT,claim_until INTEGER,
                  event_type TEXT NOT NULL,topic TEXT NOT NULL,payload TEXT NOT NULL,
                  event_ts INTEGER NOT NULL,event_month TEXT NOT NULL,created_at INTEGER NOT NULL,
                  sent INTEGER NOT NULL DEFAULT 0,sent_at INTEGER,retry_count INTEGER NOT NULL DEFAULT 0,last_error TEXT);
                CREATE TABLE mqtt_event_state(state_key TEXT PRIMARY KEY,event_type TEXT NOT NULL,
                  point_index INTEGER NOT NULL,alarm_type TEXT NOT NULL,active INTEGER NOT NULL,
                  value REAL NOT NULL,quality INTEGER NOT NULL,source_ts INTEGER NOT NULL,
                  lifecycle TEXT NOT NULL,updated_at INTEGER NOT NULL);
                INSERT INTO mqtt_event_outbox VALUES(1,'e1','main','old-claim',999,'alarm','original/topic',
                  '{"raw": 1}',100,'202609',101,0,NULL,2,'timeout');
                INSERT INTO mqtt_event_outbox VALUES(2,NULL,'main',NULL,NULL,'change','legacy/topic',
                  'original bytes',90,'202609',91,1,95,0,NULL);
                INSERT INTO mqtt_event_state VALUES('state-a','alarm',1,'high',1,4.5,0,100,'cycle',101);
            """)

    def args(self, *extra, command="migrate", source=None, target=None):
        args = [command, "--source", str(source or self.source), "--target", str(target or self.target),
                "--backup-dir", str(self.backups), "--offline"]
        if command == "migrate":
            args += ["--store-id", "store-a", "--config-generation", "g1", "--state-owners", str(self.owners)]
        else:
            args += ["--baseline", str(self.target)]
        return m.parser().parse_args(args + list(extra))

    def query(self, path, sql):
        with contextlib.closing(sqlite3.connect(path)) as db:
            return db.execute(sql).fetchall()

    def current(self):
        m.run(self.args())
        current = self.root / "current.db"
        shutil.copyfile(self.target, current)
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.executescript("""
                INSERT INTO mqtt_event_outbox VALUES(3,'after-cutover','main',NULL,NULL,'alarm',
                  'new/topic','increment',110,'202609',111,0,NULL,0,NULL);
                UPDATE mqtt_event_outbox SET sent=1,sent_at=120 WHERE id=1;
                UPDATE mqtt_event_state SET value=9,source_ts=50 WHERE state_key='state-a';
                UPDATE event_store_state_version SET version=2;
                CREATE TABLE event_store_producer(producer_id TEXT PRIMARY KEY,receipt TEXT);
                INSERT INTO event_store_producer VALUES('producer-a','exact receipt bytes');
                CREATE TABLE local_journal(id INTEGER PRIMARY KEY,body TEXT);
                INSERT INTO local_journal VALUES(7,'must survive');
            """)
        return current

    def test_migrate_preserves_source_ids_bytes_and_owners(self):
        before = m.file_set(self.source)
        report = m.run(self.args())
        self.assertEqual(before, m.file_set(self.source))
        self.assertEqual(report["publication"], "published")
        self.assertEqual(self.query(self.target, "SELECT id,event_id,payload,sent FROM mqtt_event_outbox"),
                         [(1, "e1", '{"raw": 1}', 0), (2, self.generated_id(), "original bytes", 1)])
        self.assertEqual(self.query(self.target, "SELECT * FROM event_store_state_version"),
                         [("state-a", "producer-a", 1)])
        self.assertEqual(report["released_legacy_claims"], 1)
        for item in report["sources"]:
            self.assertEqual(m.sha256(Path(item["backup"])), item["snapshot_sha256"])
        self.assertEqual(self.target.stat().st_nlink, 1)

    def test_migrate_without_python311_sqlite_error_constants(self):
        legacy_sqlite = types.SimpleNamespace(**{
            name: getattr(sqlite3, name) for name in dir(sqlite3)
            if name not in ("SQLITE_BUSY", "SQLITE_LOCKED")
        })
        before = m.file_set(self.source)
        with mock.patch.object(m, "sqlite3", legacy_sqlite):
            report = m.run(self.args())
        self.assertEqual(report["publication"], "published")
        self.assertEqual(before, m.file_set(self.source))
        self.assertEqual(self.query(self.target, "SELECT count(*) FROM mqtt_event_outbox"), [(2,)])

    def test_dry_run_full_validation_no_outputs(self):
        before = m.file_set(self.source)
        report = m.run(self.args("--dry-run"))
        self.assertEqual(report["events"], 2)
        self.assertFalse(self.target.exists())
        self.assertEqual(list(self.backups.iterdir()), [])
        self.assertEqual(before, m.file_set(self.source))

    def test_delivery_index_dry_run_space_and_cpp_contract(self):
        source = (Path(__file__).resolve().parents[1] / "src/mqtt_event_outbox.cpp").read_text(encoding="utf-8")
        match = re.search(r'"CREATE INDEX IF NOT EXISTS idx_event_store_delivery_order "\s*"([^"]+)"', source)
        self.assertIsNotNone(match, "C++ delivery index contract changed")
        self.assertEqual("CREATE INDEX IF NOT EXISTS idx_event_store_delivery_order " + match[1],
                         m.DELIVERY_INDEX_SQL + ";")
        report = m.run(self.args("--dry-run"))
        info = report["delivery_index"]
        self.assertTrue(info["created"])
        self.assertEqual(info["columns"], ["target_id", "event_type", "sent", "id"])
        self.assertGreaterEqual(info["build_seconds"], 0)
        self.assertGreater(info["used_page_growth_bytes"], 0)
        self.assertGreater(report["target_bytes"], 0)
        self.assertGreater(report["sources"][0]["snapshot_bytes"], 0)
        self.assertFalse(self.target.exists())
        self.assertEqual(self.query(self.source, "SELECT name FROM sqlite_master WHERE name='idx_event_store_delivery_order'"), [])

    def test_migrated_delivery_order_ignores_regressing_event_ts(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("INSERT INTO mqtt_event_outbox SELECT 3,'earlier-clock','main',NULL,NULL,event_type,topic,payload,"
                       "50,event_month,created_at,0,NULL,0,NULL FROM mqtt_event_outbox WHERE id=1")
        m.run(self.args())
        query = ("SELECT o.id,o.event_ts FROM mqtt_event_outbox o "
                 "WHERE o.target_id=? AND o.event_type=? AND o.sent=0 "
                 "AND (o.claim_token IS NULL OR o.claim_token='') ORDER BY o.id LIMIT ?")
        with contextlib.closing(sqlite3.connect(self.target)) as db:
            self.assertEqual(db.execute(query, ("main", "alarm", 64)).fetchall(), [(1, 100), (3, 50)])
            plan = " ".join(row[3] for row in db.execute("EXPLAIN QUERY PLAN " + query, ("main", "alarm", 64)))
            self.assertIn(m.DELIVERY_INDEX, plan)
            self.assertNotIn("TEMP B-TREE", plan.upper())
            self.assertEqual(db.execute("SELECT id FROM mqtt_event_outbox WHERE event_type='alarm' ORDER BY event_ts,id").fetchall(),
                             [(3,), (1,)])

    def test_rollback_preserves_or_builds_delivery_index_offline(self):
        current = self.current()
        before_rows = self.query(current, "SELECT * FROM mqtt_event_outbox ORDER BY id")
        result = self.root / "indexed-rollback.db"
        report = m.run(self.args(command="rollback", source=current, target=result))
        self.assertFalse(report["delivery_index"]["created"])
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute("DROP INDEX idx_event_store_delivery_order")
        before = m.file_set(current)
        result = self.root / "older-schema-rollback.db"
        report = m.run(self.args(command="rollback", source=current, target=result))
        self.assertTrue(report["delivery_index"]["created"])
        self.assertEqual(m.file_set(current), before)
        self.assertEqual(self.query(result, "SELECT * FROM mqtt_event_outbox ORDER BY id"), before_rows)
        self.assertEqual([row[2] for row in self.query(result, "PRAGMA index_info(idx_event_store_delivery_order)")],
                         m.DELIVERY_INDEX_COLUMNS)

    def test_wrong_named_delivery_index_refuses_publication(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("CREATE INDEX idx_event_store_delivery_order ON mqtt_event_outbox(event_ts,id)")
        before = m.file_set(self.source)
        with self.assertRaisesRegex(m.MigrationError, "index definition conflict"):
            m.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertEqual(m.file_set(self.source), before)

    def test_offline_required(self):
        args = self.args()
        args.offline = False
        with self.assertRaisesRegex(m.MigrationError, "offline"):
            m.run(args)

    def test_identity_index_created_by_migration_and_missing_on_rollback(self):
        current = self.current()
        name = "idx_mqtt_event_outbox_event_target"
        expected = self.query(self.target, f"PRAGMA index_xinfo({name})")
        self.assertEqual([(r[2], r[3], r[4]) for r in expected if r[5]],
                         [("event_id", 0, "BINARY"), ("target_id", 0, "BINARY")])
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute(f"DROP INDEX {name}")
        before = m.file_set(current)
        result = self.root / "identity-rollback.db"
        m.run(self.args("--dry-run", command="rollback", source=current, target=result))
        self.assertFalse(result.exists())
        m.run(self.args(command="rollback", source=current, target=result))
        self.assertEqual(before, m.file_set(current))
        self.assertEqual(self.query(result, f"PRAGMA index_xinfo({name})"), expected)
        self.assertEqual(self.query(result, "SELECT * FROM mqtt_event_outbox ORDER BY id"),
                         self.query(current, "SELECT * FROM mqtt_event_outbox ORDER BY id"))
        with contextlib.closing(sqlite3.connect(result)) as db, db:
            # The repaired index enforces same-target identity while allowing fanout.
            with self.assertRaises(sqlite3.IntegrityError):
                db.execute("UPDATE mqtt_event_outbox SET event_id='e1' WHERE id=3")
            db.execute("UPDATE mqtt_event_outbox SET event_id='e1',target_id='third' WHERE id=3")
            db.execute("UPDATE mqtt_event_outbox SET event_id=NULL")
            db.execute("UPDATE mqtt_event_outbox SET event_id=''")

    def test_identity_index_wrong_definitions_rejected_migrate_and_rollback(self):
        current = self.current()
        name = "idx_mqtt_event_outbox_event_target"
        predicate = " WHERE event_id IS NOT NULL AND event_id<>''"
        definitions = [
            f"CREATE INDEX {name} ON mqtt_event_outbox(event_id,target_id)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id,target_id)",
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(target_id,event_id)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id COLLATE NOCASE,target_id)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id,target_id COLLATE RTRIM)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id DESC,target_id)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id,target_id DESC)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(lower(event_id),target_id)" + predicate,
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id,target_id) WHERE sent=0",
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox(event_id,target_id) WHERE event_id IS NOT NULL AND event_id<>' '",
            f"CREATE UNIQUE INDEX {name} ON mqtt_event_state(state_key) WHERE active=1",
        ]
        for command, source in (("migrate", self.source), ("rollback", current)):
            for i, ddl in enumerate(definitions):
                for dry_run in (False, True):
                    with self.subTest(command=command, ddl=ddl, dry_run=dry_run):
                        candidate = self.root / f"bad-index-{command}-{i}-{dry_run}.db"
                        shutil.copyfile(source, candidate)
                        with contextlib.closing(sqlite3.connect(candidate)) as db, db:
                            db.execute(f"DROP INDEX IF EXISTS {name}")
                            db.execute(ddl)
                        before = m.file_set(candidate)
                        target = self.root / "rejected-index.db"
                        extra = ("--dry-run",) if dry_run else ()
                        with self.assertRaisesRegex(m.MigrationError, "event identity index definition conflict"):
                            m.run(self.args(*extra, command=command, source=candidate, target=target))
                        self.assertFalse(target.exists())
                        self.assertEqual(before, m.file_set(candidate))

    def test_identity_index_explicit_binary_asc_and_predicate_case_preserved(self):
        name = "idx_mqtt_event_outbox_event_target"
        ddl = (f"CREATE UNIQUE INDEX {name} ON mqtt_event_outbox"
               "(event_id COLLATE BINARY ASC,target_id COLLATE BINARY ASC) "
               "WHERE event_id is not null\n AND event_id <> ''")
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute(ddl)
        m.run(self.args())
        self.assertEqual(self.query(self.target, f"SELECT sql FROM sqlite_master WHERE name='{name}'"), [(ddl,)])
        result = self.root / "valid-index-rollback.db"
        current = self.root / "valid-index-current.db"
        shutil.copyfile(self.target, current)
        m.run(self.args(command="rollback", source=current, target=result))
        self.assertEqual(self.query(result, f"SELECT sql FROM sqlite_master WHERE name='{name}'"), [(ddl,)])

    def test_owner_map_exact(self):
        self.owners.write_text("{}")
        with self.assertRaisesRegex(m.MigrationError, "cover exactly"):
            m.run(self.args("--dry-run"))

    def test_rollback_increment_state_receipt_journal_and_idempotency(self):
        current = self.current()
        before = m.file_set(current)
        result = self.root / "rollback.db"
        m.run(self.args(command="rollback", source=current, target=result))
        self.assertEqual(before, m.file_set(current))
        self.assertEqual(self.query(result, "SELECT event_id,sent FROM mqtt_event_outbox ORDER BY id"),
                         [("e1", 1), (self.generated_id(), 1), ("after-cutover", 0)])
        self.assertEqual(self.query(result, "SELECT value,source_ts FROM mqtt_event_state"), [(9.0, 50)])
        self.assertEqual(self.query(result, "SELECT receipt FROM event_store_producer"), [("exact receipt bytes",)])
        self.assertEqual(self.query(result, "SELECT * FROM local_journal"), [(7, "must survive")])
        again = self.root / "again.db"
        report = m.run(self.args(command="rollback", source=result, target=again))
        self.assertEqual(report["events_merged"], 0)
        self.assertEqual(self.query(again, "SELECT count(*) FROM mqtt_event_outbox"), [(3,)])

    def test_rollback_restores_deleted_history_by_event_identity(self):
        current = self.current()
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute("DELETE FROM mqtt_event_outbox WHERE id=1")
        result = self.root / "rollback.db"
        report = m.run(self.args(command="rollback", source=current, target=result))
        self.assertEqual(report["events_merged"], 1)
        self.assertEqual(self.query(result, "SELECT count(*) FROM mqtt_event_outbox WHERE event_id='e1'"), [(1,)])

    def test_conflicts_refuse_publish(self):
        changes = ["UPDATE mqtt_event_outbox SET payload='different' WHERE id=1",
                   "UPDATE mqtt_event_outbox SET sent=0 WHERE id=2",
                   "UPDATE event_store_state_version SET version=1",
                   "UPDATE event_store_state_version SET producer_id='other'",
                   "UPDATE event_store_identity SET config_generation='other'"]
        current = self.current()
        for index, sql in enumerate(changes):
            with self.subTest(sql=sql):
                variant = self.root / f"variant-{index}.db"
                shutil.copyfile(current, variant)
                with contextlib.closing(sqlite3.connect(variant)) as db, db:
                    db.execute(sql)
                output = self.root / f"result-{index}.db"
                with self.assertRaises(m.MigrationError):
                    m.run(self.args(command="rollback", source=variant, target=output))
                self.assertFalse(output.exists())

    def test_modified_baseline_rejected(self):
        current = self.current()
        with contextlib.closing(sqlite3.connect(self.target)) as db, db:
            db.execute("UPDATE mqtt_event_outbox SET payload='edited' WHERE id=2")
        with self.assertRaisesRegex(m.MigrationError, "baseline was modified"):
            m.run(self.args("--dry-run", command="rollback", source=current, target=self.root / "r.db"))

    def test_writer_lock_rejected(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.execute("BEGIN IMMEDIATE")
            with self.assertRaises(sqlite3.OperationalError):
                m.run(self.args("--dry-run"))

    def test_abandoned_wal_source_hash_unchanged_after_close(self):
        code = """import sqlite3,os,sys
db=sqlite3.connect(sys.argv[1])
db.execute('PRAGMA journal_mode=WAL')
db.execute('PRAGMA wal_autocheckpoint=0')
db.execute("UPDATE mqtt_event_outbox SET payload='crash WAL' WHERE id=1")
db.commit()
os._exit(0)
"""
        subprocess.run([sys.executable, "-c", code, str(self.source)], check=True)
        before = m.file_set(self.source)
        report = m.run(self.args())
        self.assertEqual(before, m.file_set(self.source))
        self.assertEqual(report["sources"][0]["files_sha256"], before)
        self.assertEqual(self.query(self.target, "SELECT payload FROM mqtt_event_outbox WHERE id=1"), [("crash WAL",)])

    def test_cleanly_closed_wal_database(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.execute("PRAGMA journal_mode=WAL")
        m.run(self.args())
        self.assertTrue(self.target.exists())

    def test_automatic_baseline_is_independent_and_usable(self):
        report = m.run(self.args())
        baseline = Path(report["cutover_baseline"])
        self.assertEqual(m.sha256(baseline), report["target_sha256"])
        self.assertFalse(os.path.samefile(baseline, self.target))
        args = self.args(command="rollback", source=self.target, target=self.root / "r.db")
        args.baseline = str(baseline)
        m.run(args)

    def test_journal_rejected_before_recovery(self):
        Path(str(self.source) + "-journal").write_bytes(b"requires operator recovery")
        before = m.file_set(self.source)
        with self.assertRaisesRegex(m.MigrationError, "journal present"):
            m.run(self.args())
        self.assertEqual(before, m.file_set(self.source))

    def test_journal_schema_and_rollback_preserve_generation_cursor_rows(self):
        current = self.current()
        generation = self.query(current, "SELECT journal_generation FROM event_journal_meta")
        self.assertEqual(len(generation[0][0]), 32)
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            for row_id, kind in ((1, "alarm"), (2, "change")):
                db.execute("INSERT INTO event_local_journal VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           (row_id, kind, "local-" + kind, 1, 120, "high", 1, 5.0, 6.0, 0, 0,
                            "raw", "gateway", "device", "point", 2, "g1"))
            db.execute("UPDATE event_history_projection_cursor SET projected_through=2")
            with self.assertRaisesRegex(sqlite3.IntegrityError, "immutable journal"):
                db.execute("UPDATE event_local_journal SET value=0")
        result = self.root / "r.db"
        m.run(self.args(command="rollback", source=current, target=result))
        for table in ("event_local_journal", "event_history_projection_cursor", "event_journal_meta"):
            self.assertEqual(self.query(result, "SELECT * FROM " + table), self.query(current, "SELECT * FROM " + table))
        self.assertEqual(self.query(result, "SELECT journal_generation FROM event_journal_meta"), generation)
        self.assertEqual(self.query(result, "SELECT schema_version FROM event_store_identity"), [(1,)])

    def test_journal_retention_and_generation_conflicts_rejected(self):
        current = self.current()
        for index, sql in enumerate(("UPDATE event_history_projection_cursor SET cleaned_through=1",
                                     "UPDATE event_journal_meta SET journal_generation='other'")):
            variant = self.root / f"journal-{index}.db"
            shutil.copyfile(current, variant)
            with contextlib.closing(sqlite3.connect(variant)) as db, db:
                db.execute(sql)
            with self.assertRaises(m.MigrationError):
                m.run(self.args("--dry-run", command="rollback", source=variant, target=self.root / "r.db"))

    def test_actual_cpp_journal_schema_contract(self):
        source = (Path(__file__).resolve().parents[1] / "src/mqtt_event_outbox.cpp").read_text(encoding="utf-8")
        names = ("event_journal_meta", "event_local_journal", "event_history_projection_cursor")
        statements = []
        for match in re.finditer(r'execOnceOrThrow\(db,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\);', source):
            sql = "".join(json.loads(token) for token in re.findall(r'"(?:[^"\\]|\\.)*"', match[1]))
            if sql.startswith(("CREATE TABLE", "CREATE TRIGGER", "INSERT OR IGNORE")) and any(n in sql for n in names):
                statements.append(sql)
        self.assertEqual(len(statements), 6, "C++ journal schema contract changed; inspect adapter")
        m.run(self.args())
        with contextlib.closing(sqlite3.connect(":memory:")) as expected:
            for sql in statements:
                expected.execute(sql)
            for name in names:
                self.assertEqual(self.query(self.target, "PRAGMA table_info(" + name + ")"),
                                 expected.execute("PRAGMA table_info(" + name + ")").fetchall())
        generation = self.query(self.target, "SELECT journal_generation FROM event_journal_meta")
        with contextlib.closing(sqlite3.connect(self.target)) as target, target:
            for sql in statements:
                target.execute(sql)
        self.assertEqual(self.query(self.target, "SELECT journal_generation FROM event_journal_meta"), generation)

    def test_empty_events_nonempty_states(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("DELETE FROM mqtt_event_outbox")
        report = m.run(self.args())
        self.assertEqual((report["events"], report["states"]), (0, 1))
        self.assertEqual(self.query(self.target, "SELECT count(*) FROM event_local_journal"), [(0,)])

    def test_pre_identity_legacy_columns_added_with_mapped_ids(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.executescript("""ALTER TABLE mqtt_event_outbox RENAME TO old_events;
                CREATE TABLE mqtt_event_outbox(id INTEGER PRIMARY KEY AUTOINCREMENT,
                  event_type TEXT NOT NULL,topic TEXT NOT NULL,payload TEXT NOT NULL,
                  event_ts INTEGER NOT NULL,event_month TEXT NOT NULL,created_at INTEGER NOT NULL,
                  sent INTEGER NOT NULL DEFAULT 0,sent_at INTEGER,retry_count INTEGER NOT NULL DEFAULT 0,last_error TEXT);
                INSERT INTO mqtt_event_outbox SELECT id,event_type,topic,payload,event_ts,event_month,
                  created_at,sent,sent_at,retry_count,last_error FROM old_events;
                DROP TABLE old_events;""")
        m.run(self.args())
        rows = self.query(self.target, "SELECT event_id,target_id FROM mqtt_event_outbox")
        self.assertEqual(len({r[0] for r in rows}), 2)
        self.assertTrue(all(r[0].startswith("legacy:v1:") and r[1] == "main" for r in rows))
        self.assertEqual(self.query(self.target, f"SELECT original_event_id FROM {m.ID_MAP}"), [(None,), (None,)])

    def generated_id(self):
        return self.query(self.target, f"SELECT event_id FROM {m.ID_MAP} WHERE source_row_id=2")[0][0]

    def test_generated_identity_collision_refuses_publication(self):
        before = m.file_set(self.source)
        with mock.patch.object(m, "stable_id", return_value="e1"):
            with self.assertRaisesRegex(m.MigrationError, "identity collision"):
                m.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertEqual(before, m.file_set(self.source))

    def test_journal_time_index_rollback_and_definition_conflict(self):
        current = self.current()
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute("DROP INDEX idx_event_local_journal_kind_ts_id")
        restored = self.root / "restored.db"
        report = m.run(self.args(command="rollback", source=current, target=restored))
        self.assertTrue(report["journal_time_index"]["created"])
        self.assertEqual([r[2] for r in self.query(restored, "PRAGMA index_info(idx_event_local_journal_kind_ts_id)")],
                         ["kind", "ts", "id"])
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute("CREATE INDEX idx_event_local_journal_kind_ts_id ON event_local_journal(ts,id)")
        with self.assertRaisesRegex(m.MigrationError, "index definition conflict"):
            m.run(self.args(command="rollback", source=current, target=self.root / "bad.db"))

    def test_stable_ids_null_empty_repeat_and_rollback_mapping(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("UPDATE mqtt_event_outbox SET event_id='' WHERE id=1")
        m.run(self.args())
        again = self.root / "repeat.db"
        m.run(self.args(target=again))
        original = self.query(self.target, f"SELECT * FROM {m.ID_MAP} ORDER BY source_row_id")
        self.assertEqual([r[3] for r in original], ["", None])
        self.assertEqual(original, self.query(again, f"SELECT * FROM {m.ID_MAP} ORDER BY source_row_id"))
        shutil.copyfile(self.target, again)
        with contextlib.closing(sqlite3.connect(again)) as db, db:
            db.execute("UPDATE mqtt_event_outbox SET id=40 WHERE id=2")
        restored = self.root / "restored.db"
        report = m.run(self.args(command="rollback", source=again, target=restored))
        self.assertEqual(report["events_merged"], 0)
        self.assertEqual(original, self.query(restored, f"SELECT * FROM {m.ID_MAP} ORDER BY source_row_id"))
        with contextlib.closing(sqlite3.connect(again)) as db, db:
            db.execute(f"UPDATE {m.ID_MAP} SET original_event_id='forged' WHERE source_row_id=2")
        with self.assertRaisesRegex(m.MigrationError, "mapping conflict"):
            m.run(self.args(command="rollback", source=again, target=self.root / "bad.db"))

    def test_mapping_is_covered_by_baseline_digest(self):
        current = self.current()
        with contextlib.closing(sqlite3.connect(self.target)) as db, db:
            db.execute(f"DELETE FROM {m.ID_MAP}")
        with self.assertRaisesRegex(m.MigrationError, "baseline was modified"):
            m.run(self.args(command="rollback", source=current, target=self.root / "bad.db"))

    def test_same_event_multiple_targets_and_duplicate_rejection(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("INSERT INTO mqtt_event_outbox SELECT 3,event_id,'third',NULL,NULL,event_type,topic,payload,"
                       "event_ts,event_month,created_at,sent,sent_at,retry_count,last_error FROM mqtt_event_outbox WHERE id=1")
        report = m.run(self.args("--dry-run"))
        self.assertEqual(report["events"], 3)
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute("UPDATE mqtt_event_outbox SET target_id='main' WHERE id=3")
        with self.assertRaisesRegex(m.MigrationError, "duplicate event identity"):
            m.run(self.args("--dry-run"))

    @unittest.skipUnless(os.name == "posix", "Linux runtime flock")
    def test_runtime_flock_rejected(self):
        import fcntl
        with self.source.open("rb") as file:
            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                m.run(self.args("--dry-run"))

    def test_wal_snapshot_includes_uncheckpointed_rows(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA wal_autocheckpoint=0")
            db.execute("UPDATE mqtt_event_outbox SET payload='in WAL' WHERE id=1")
            db.commit()
            report = m.run(self.args())
            self.assertIn("-wal", report["sources"][0]["files_sha256"])
            self.assertEqual(self.query(self.target, "SELECT payload FROM mqtt_event_outbox WHERE id=1"), [("in WAL",)])

    def test_hardlink_alias_rejected(self):
        os.link(self.source, self.root / "alias.db")
        with self.assertRaisesRegex(m.MigrationError, "one hard link"):
            m.run(self.args())

    def test_existing_target_and_sidecar_never_overwritten(self):
        for suffix in ("", "-wal"):
            path = Path(str(self.target) + suffix)
            path.write_bytes(b"untouched")
            with self.assertRaises(m.MigrationError):
                m.run(self.args())
            self.assertEqual(path.read_bytes(), b"untouched")
            path.unlink()

    def test_publish_race_no_clobber(self):
        stage, target = self.root / "stage", self.root / "result"
        stage.write_bytes(b"new")
        real_link = os.link
        def racing_link(src, dst):
            Path(dst).write_bytes(b"other owner")
            real_link(src, dst)
        with mock.patch.object(m.os, "link", side_effect=racing_link):
            with self.assertRaises(FileExistsError):
                m.publish(stage, target)
        self.assertEqual(target.read_bytes(), b"other owner")

    def test_backup_failure_no_publication(self):
        before = m.file_set(self.source)
        with mock.patch.object(m, "snapshot", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                m.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertEqual(before, m.file_set(self.source))

    def test_existing_backup_sidecars_rejected(self):
        report = m.run(self.args())
        backup = Path(report["sources"][0]["backup"])
        Path(str(backup) + "-wal").write_bytes(b"not part of verified snapshot")
        with self.assertRaisesRegex(m.MigrationError, "backup has SQLite sidecars"):
            m.run(self.args(target=self.root / "other.db"))

    def test_corrupt_source_rejected_and_connection_closed(self):
        self.source.write_bytes(b"not a SQLite file")
        before = m.sha256(self.source)
        with self.assertRaises(sqlite3.DatabaseError):
            m.run(self.args("--dry-run"))
        self.assertEqual(m.sha256(self.source), before)
        self.assertFalse(self.target.exists())

    def test_cli_error_is_nonzero_json(self):
        result = subprocess.run([sys.executable, str(Path(m.__file__)), "migrate", "--source", str(self.source),
                                 "--target", str(self.target), "--backup-dir", str(self.backups),
                                 "--store-id", "s", "--config-generation", "g", "--state-owners", str(self.owners)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("offline", json.loads(result.stderr)["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
