#!/usr/bin/env python3
"""Offline history/identity regressions; optional actual C++ IPC/projection probe."""
import contextlib
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import unittest
from unittest import mock

import event_store_migrate as m
import event_store_migrate_test as fixtures


class HistoryTests(unittest.TestCase):
    args = fixtures.MigrationTests.args
    query = fixtures.MigrationTests.query

    def setUp(self):
        fixtures.MigrationTests.setUp(self)
        m.run(self.args())
        self.history = self.root / "alarm-history.db"
        self.projection = self.root / "projection.db"
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.executescript("""
                CREATE TABLE alarm_events(id INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT,
                  point_index INTEGER NOT NULL,ts INTEGER NOT NULL,alarm_type TEXT NOT NULL,
                  active INTEGER NOT NULL,threshold REAL NOT NULL,value REAL NOT NULL,
                  quality INTEGER NOT NULL,stale INTEGER NOT NULL,persist_value TEXT NOT NULL,
                  gateway_code TEXT NOT NULL,device_code TEXT NOT NULL,point_code TEXT NOT NULL);
                INSERT INTO alarm_events VALUES(7,'old-alarm',23,100,'high',1,4.5,9.25,1,0,
                  'raw original','gateway','device','point');
                INSERT INTO alarm_events SELECT 8,NULL,point_index,90,alarm_type,0,threshold,1.25,0,1,
                  persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7;
                INSERT INTO alarm_events SELECT 9,'',point_index,80,alarm_type,0,threshold,1.25,1,0,
                  persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7;
                CREATE TABLE legacy_extra(note TEXT);
                INSERT INTO legacy_extra VALUES('must survive initial conversion');
            """)

    def history_args(self, *extra, target=None):
        return m.parser().parse_args(["history", "--source", str(self.history), "--event-store", str(self.target),
            "--target", str(target or self.projection), "--backup-dir", str(self.backups),
            "--history-id", "gateway-old-alarm", "--offline"] + list(extra))

    def journal(self, event_id="post-cutover", ts=200):
        with contextlib.closing(sqlite3.connect(self.target)) as db, db:
            db.execute("INSERT INTO event_local_journal(kind,event_id,point_index,ts,alarm_type,active,threshold,value,"
                       "quality,stale,persist_value,gateway_code,device_code,point_code,state_version,config_generation) "
                       "VALUES('alarm',?,23,?,'high',1,4.5,9.25,1,0,'raw original','gateway','device','point',1,'g1')",
                       (event_id, ts))

    def test_initial_full_history_stable_ids_fields_sources_indexes(self):
        before = {p: m.file_set(p) for p in (self.history, self.target)}
        report = m.run(self.history_args())
        self.assertEqual((report["history_rows"], report["assigned_history_ids"], report["projection_watermark"]), (3, 2, 0))
        for p in before:
            self.assertEqual(before[p], m.file_set(p))
        fields = ",".join(k for k in m.HISTORY_COLUMNS if k != "event_id")
        self.assertEqual(self.query(self.history, f"SELECT {fields} FROM alarm_events ORDER BY id"),
                         self.query(self.projection, f"SELECT {fields} FROM alarm_events ORDER BY id"))
        self.assertEqual(self.query(self.projection, "SELECT event_id FROM alarm_events WHERE id=7"), [("old-alarm",)])
        self.assertEqual(self.query(self.projection, f"SELECT source_row_id,original_event_id FROM {m.ID_MAP} ORDER BY source_row_id"),
                         [(8, None), (9, "")])
        self.assertEqual(self.query(self.projection, "SELECT * FROM legacy_extra"), [("must survive initial conversion",)])
        repeat = self.root / "repeat.db"
        m.run(self.history_args(target=repeat))
        self.assertEqual(self.query(self.projection, "SELECT * FROM alarm_events"), self.query(repeat, "SELECT * FROM alarm_events"))
        generation = self.query(self.target, "SELECT journal_generation FROM event_journal_meta")[0][0]
        self.assertEqual(self.query(repeat, "SELECT * FROM alarm_projection_meta"), [(1, "alarm-history-v1", "store-a", generation, 0)])
        self.assertEqual(self.query(repeat, "PRAGMA index_info(idx_alarm_events_ts_id)"), [(0, 3, "ts"), (1, 0, "id")])
        self.assertEqual(self.query(self.target, "SELECT count(*) FROM event_local_journal"), [(0,)])

    def rollback_history_fixture(self):
        m.run(self.history_args())
        self.journal()
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("INSERT INTO alarm_events SELECT 10,'post-cutover',point_index,200,alarm_type,active,threshold,value,"
                       "quality,stale,persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7")
            db.execute("UPDATE alarm_projection_meta SET last_contiguous_journal_id=1")
            db.execute("UPDATE sqlite_sequence SET seq=500 WHERE name='alarm_events'")
        rollback = self.root / "rollback-history.db"
        m.run(self.history_args("--projection", str(self.projection), target=rollback))
        self.history = rollback
        new_store = self.root / "new-store.db"
        m.run(self.args("--store-id", "new-store", "--config-generation", "g2", target=new_store))
        self.target = new_store
        self.projection = self.root / "rebound.db"

    def test_rollback_rebind_then_increment_and_second_rollback(self):
        self.rollback_history_fixture()
        before = {p: m.file_set(p) for p in (self.history, self.target)}
        with self.assertRaisesRegex(m.MigrationError, "source must be legacy"):
            m.run(self.history_args())
        report = m.run(self.history_args("--rebind-rollback-history"))
        self.assertTrue(report["rollback_history_rebound"])
        self.assertEqual(report["assigned_history_ids"], 0)
        self.assertEqual(report["history_rows"], 4)
        for p, hashes in before.items():
            self.assertEqual(m.file_set(p), hashes)
        for table in ("alarm_events", m.ID_MAP, "sqlite_sequence", "legacy_extra"):
            self.assertEqual(self.query(self.history, f"SELECT * FROM {table}"),
                             self.query(self.projection, f"SELECT * FROM {table}"))
        generation = self.query(self.target, "SELECT journal_generation FROM event_journal_meta")[0][0]
        self.assertEqual(self.query(self.projection, "SELECT * FROM alarm_projection_meta"),
                         [(1, "alarm-history-v1", "new-store", generation, 0)])
        self.journal("new-generation-alarm", 300)
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("INSERT INTO alarm_events SELECT 501,'new-generation-alarm',point_index,300,alarm_type,active,threshold,value,"
                       "quality,stale,persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7")
            db.execute("UPDATE alarm_projection_meta SET last_contiguous_journal_id=1")
        before = {p: m.file_set(p) for p in (self.history, self.target, self.projection)}
        final = self.root / "second-rollback.db"
        result = m.run(self.history_args("--projection", str(self.projection), target=final))
        self.assertEqual((result["history_rows"], result["assigned_history_ids"], result["projection_watermark"]), (5, 0, 1))
        for table in ("alarm_events", m.ID_MAP, "sqlite_sequence", "alarm_projection_meta"):
            self.assertEqual(self.query(self.projection, f"SELECT * FROM {table}"), self.query(final, f"SELECT * FROM {table}"))
        for p, hashes in before.items():
            self.assertEqual(m.file_set(p), hashes)

    def test_rebind_flag_legacy_noop_and_projection_conflict(self):
        report = m.run(self.history_args("--rebind-rollback-history"))
        self.assertFalse(report["rollback_history_rebound"])
        self.assertEqual(report["assigned_history_ids"], 2)
        with self.assertRaisesRegex(m.MigrationError, "conflicts with --projection"):
            m.run(self.history_args("--rebind-rollback-history", "--projection", str(self.projection), target=self.root / "bad.db"))

    def test_rebind_rejects_altered_meta_and_anonymous_ids_without_publication(self):
        self.rollback_history_fixture()
        cases = [
            ("ALTER TABLE alarm_projection_meta ADD COLUMN hidden TEXT", "metadata schema mismatch",
             "ALTER TABLE alarm_projection_meta DROP COLUMN hidden"),
            ("UPDATE alarm_projection_meta SET projection_id='wrong'", "invalid rollback history metadata",
             "UPDATE alarm_projection_meta SET projection_id='alarm-history-v1'"),
            ("UPDATE alarm_events SET event_id=NULL WHERE id=7", "anonymous event_id",
             "UPDATE alarm_events SET event_id='old-alarm' WHERE id=7"),
            (f"ALTER TABLE {m.ID_MAP} ADD COLUMN hidden TEXT", "metadata schema mismatch",
             f"ALTER TABLE {m.ID_MAP} DROP COLUMN hidden"),
        ]
        for change, error, restore in cases:
            with self.subTest(change=change):
                with contextlib.closing(sqlite3.connect(self.history)) as db, db:
                    db.execute(change)
                before = m.file_set(self.history)
                try:
                    with self.assertRaisesRegex(m.MigrationError, error):
                        m.run(self.history_args("--rebind-rollback-history"))
                    self.assertEqual(m.file_set(self.history), before)
                    self.assertFalse(self.projection.exists())
                finally:
                    with contextlib.closing(sqlite3.connect(self.history)) as db, db:
                        db.execute(restore)

    def test_projected_source_mapping_conflict_and_new_identity_rejected(self):
        self.rollback_history_fixture()
        m.run(self.history_args("--rebind-rollback-history"))
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute(f"UPDATE {m.ID_MAP} SET event_id='changed' WHERE source_row_id=8")
        with self.assertRaisesRegex(m.MigrationError, "source mapping conflict"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("UPDATE alarm_projection_meta SET journal_generation='wrong'")
        with self.assertRaisesRegex(m.MigrationError, "projection identity mismatch"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))

    def test_rebind_detects_copy_mutation_and_dry_run_preserves_source(self):
        self.rollback_history_fixture()
        before = m.file_set(self.history)
        report = m.run(self.history_args("--rebind-rollback-history", "--dry-run"))
        self.assertTrue(report["rollback_history_rebound"])
        self.assertFalse(self.projection.exists())
        prepare = m.prepare_history
        def tamper(db, store):
            result = prepare(db, store)
            db.execute("UPDATE alarm_events SET persist_value='changed' WHERE id=7")
            return result
        with mock.patch.object(m, "prepare_history", side_effect=tamper):
            with self.assertRaisesRegex(m.MigrationError, "rows/IDs/mapping/sequence changed"):
                m.run(self.history_args("--rebind-rollback-history"))
        self.assertEqual(m.file_set(self.history), before)
        self.assertFalse(self.projection.exists())
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("DROP INDEX idx_alarm_events_event_id")
            db.execute("UPDATE alarm_events SET event_id='old-alarm' WHERE id=8")
        before = m.file_set(self.history)
        with self.assertRaisesRegex(m.MigrationError, "duplicate history"):
            m.run(self.history_args("--rebind-rollback-history"))
        self.assertEqual(m.file_set(self.history), before)
        self.assertFalse(self.projection.exists())

    def test_pre_event_id_history(self):
        fields = ",".join(k for k in m.HISTORY_COLUMNS if k != "event_id")
        before = self.query(self.history, f"SELECT {fields} FROM alarm_events ORDER BY id")
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            # Construct the old schema without DROP COLUMN (SQLite 3.35+ only).
            db.execute("CREATE TABLE old_alarm_events(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "point_index INTEGER NOT NULL,ts INTEGER NOT NULL,alarm_type TEXT NOT NULL,"
                       "active INTEGER NOT NULL,threshold REAL NOT NULL,value REAL NOT NULL,"
                       "quality INTEGER NOT NULL,stale INTEGER NOT NULL,persist_value TEXT NOT NULL,"
                       "gateway_code TEXT NOT NULL,device_code TEXT NOT NULL,point_code TEXT NOT NULL)")
            db.execute(f"INSERT INTO old_alarm_events({fields}) SELECT {fields} FROM alarm_events")
            db.execute("DROP TABLE alarm_events")
            db.execute("ALTER TABLE old_alarm_events RENAME TO alarm_events")
        report = m.run(self.history_args())
        self.assertEqual(report["assigned_history_ids"], 3)
        self.assertEqual(len({r[0] for r in self.query(self.projection, "SELECT event_id FROM alarm_events")}), 3)
        self.assertNotIn("event_id", [r[1] for r in self.query(self.history, "PRAGMA table_info(alarm_events)")])
        self.assertEqual(before, self.query(self.projection, f"SELECT {fields} FROM alarm_events ORDER BY id"))

    def test_history_indexes_and_generated_collision_rejected(self):
        with mock.patch.object(m, "stable_id", return_value="old-alarm"):
            with self.assertRaisesRegex(m.MigrationError, "identity collision"):
                m.run(self.history_args())
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("CREATE INDEX idx_alarm_events_ts_id ON alarm_events(id,ts)")
        with self.assertRaisesRegex(m.MigrationError, "index definition conflict"):
            m.run(self.history_args())
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("DROP INDEX idx_alarm_events_ts_id")
            db.execute("CREATE UNIQUE INDEX idx_alarm_events_event_id ON alarm_events(event_id) WHERE active=1")
        with self.assertRaisesRegex(m.MigrationError, "identity index definition conflict"):
            m.run(self.history_args())
        self.assertFalse(self.projection.exists())

    def test_duplicate_legacy_rows_rejected_even_with_current_projection(self):
        m.run(self.history_args())
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("INSERT INTO alarm_events SELECT 10,event_id,point_index,ts,alarm_type,active,threshold,value,quality,"
                       "stale,persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7")
        with self.assertRaisesRegex(m.MigrationError, "duplicate history"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))

    def test_incremental_import_idempotent_preserves_watermark_and_mapping(self):
        m.run(self.history_args())
        self.journal()
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("INSERT INTO alarm_events SELECT 10,'post-cutover',point_index,200,alarm_type,active,threshold,value,"
                       "quality,stale,persist_value,gateway_code,device_code,point_code FROM alarm_events WHERE id=7")
            db.execute("UPDATE alarm_projection_meta SET last_contiguous_journal_id=1")
        before = m.file_set(self.projection)
        merged = self.root / "merged.db"
        report = m.run(self.history_args("--projection", str(self.projection), target=merged))
        self.assertEqual((report["history_imported"], report["history_matched"], report["projection_watermark"]), (0, 3, 1))
        self.assertEqual(before, m.file_set(self.projection))
        self.assertEqual(self.query(merged, "SELECT * FROM alarm_events"), self.query(self.projection, "SELECT * FROM alarm_events"))
        self.assertEqual(self.query(merged, f"SELECT * FROM {m.ID_MAP}"), self.query(self.projection, f"SELECT * FROM {m.ID_MAP}"))
        # A legacy row deleted from the current projection is restored by eventId, not old local id.
        with contextlib.closing(sqlite3.connect(merged)) as db, db:
            db.execute("DELETE FROM alarm_events WHERE id=8")
        restored = self.root / "restored.db"
        report = m.run(self.history_args("--projection", str(merged), target=restored))
        self.assertEqual(report["history_imported"], 1)
        self.assertEqual(self.query(restored, "SELECT count(*) FROM alarm_events"), [(4,)])

    def test_history_conflicting_id_duplicate_and_journal_refuse_publication(self):
        self.journal("old-alarm", 999)
        with self.assertRaisesRegex(m.MigrationError, "history/journal content conflict"):
            m.run(self.history_args())
        self.assertFalse(self.projection.exists())
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("UPDATE alarm_events SET event_id='old-alarm' WHERE id=8")
        with self.assertRaisesRegex(m.MigrationError, "duplicate history"):
            m.run(self.history_args())

    def test_merge_conflicting_fields_and_source_mapping_rejected(self):
        m.run(self.history_args())
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("UPDATE alarm_events SET persist_value='tampered' WHERE id=7")
        with self.assertRaisesRegex(m.MigrationError, "immutable-content"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("UPDATE alarm_events SET persist_value='raw original' WHERE id=7")
        with contextlib.closing(sqlite3.connect(self.history)) as db, db:
            db.execute("UPDATE alarm_events SET value=999 WHERE id=8")
        with self.assertRaisesRegex(m.MigrationError, "mapping changed"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))

    def test_projection_identity_and_skipped_watermark_rejected(self):
        m.run(self.history_args())
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("UPDATE alarm_projection_meta SET store_id='wrong'")
        with self.assertRaisesRegex(m.MigrationError, "identity mismatch"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))
        self.journal()
        with contextlib.closing(sqlite3.connect(self.projection)) as db, db:
            db.execute("UPDATE alarm_projection_meta SET store_id='store-a',last_contiguous_journal_id=1")
        with self.assertRaisesRegex(m.MigrationError, "missing alarm history"):
            m.run(self.history_args("--projection", str(self.projection), target=self.root / "bad.db"))

    def test_dry_run_wal_lock_backup_failure(self):
        existing = set(self.backups.iterdir())
        with contextlib.closing(sqlite3.connect(self.history)) as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA wal_autocheckpoint=0")
            db.execute("UPDATE alarm_events SET persist_value='in WAL' WHERE id=7")
            db.commit()
            before = m.file_set(self.history)
            report = m.run(self.history_args("--dry-run"))
            self.assertEqual(before, m.file_set(self.history))
            self.assertTrue(any("-wal" in p["files_sha256"] for p in report["sources"]))
            self.assertFalse(self.projection.exists())
            self.assertEqual(existing, set(self.backups.iterdir()))
            db.execute("BEGIN IMMEDIATE")
            with self.assertRaises(sqlite3.OperationalError):
                m.run(self.history_args())
            db.rollback()
        with mock.patch.object(m, "snapshot", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                m.run(self.history_args())
        self.assertFalse(self.projection.exists())

    @unittest.skipUnless(os.environ.get("EVENT_STORE_MIGRATE_PROBE"), "set EVENT_STORE_MIGRATE_PROBE to compiled C++ probe")
    def test_real_cpp_client_empty_id_migrate_ack_rollback_and_projection(self):
        probe = os.environ["EVENT_STORE_MIGRATE_PROBE"]
        def execute(path, mode, *extra):
            result = subprocess.run([probe, str(path), mode, *map(str, extra)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout
        for empty in (None, ""):
            broken = self.root / ("null.db" if empty is None else "empty.db")
            shutil.copyfile(self.target, broken)
            with contextlib.closing(sqlite3.connect(broken)) as db, db:
                db.execute("UPDATE mqtt_event_outbox SET event_id=?,sent=0 WHERE id=2", (empty,))
            self.assertIn("EMPTY_ID_BAD_RESPONSE_UNKNOWN", execute(broken, "bad"))
        current = self.root / "current.db"
        shutil.copyfile(self.target, current)
        with contextlib.closing(sqlite3.connect(current)) as db, db:
            db.execute("UPDATE mqtt_event_outbox SET sent=0 WHERE id=2")
        # Use a baseline with both events pending, before any runtime access.
        baseline = self.root / "probe-baseline.db"
        with contextlib.closing(m.connect(current)) as db, db:
            db.execute("UPDATE event_store_migration_manifest SET baseline_sha256=?", (m.baseline_digest(db),))
        shutil.copyfile(current, baseline)
        generated = self.query(current, "SELECT event_id FROM mqtt_event_outbox WHERE id=2")[0][0]
        output = execute(current, "claim")
        self.assertIn("CLAIM_ACK_OK 2", output)
        self.assertIn(generated, output)
        restored = self.root / "rollback-ipc.db"
        args = self.args(command="rollback", source=current, target=restored)
        args.baseline = str(baseline)
        m.run(args)
        self.assertEqual(self.query(restored, "SELECT sent FROM mqtt_event_outbox ORDER BY id"), [(1,), (1,)])
        self.assertEqual(self.query(restored, f"SELECT * FROM {m.ID_MAP}"), self.query(current, f"SELECT * FROM {m.ID_MAP}"))
        self.journal("old-alarm", 100)
        self.journal()
        m.run(self.history_args())
        self.assertIn("PROJECTION_OK 2", execute(self.target, "projection", self.projection))
        self.assertIn("PROJECTION_OK 2", execute(self.target, "projection", self.projection))
        self.assertEqual(self.query(self.projection, "SELECT count(*) FROM alarm_events"), [(4,)])
        self.assertEqual(self.query(self.projection, "SELECT count(*) FROM alarm_events WHERE event_id='old-alarm'"), [(1,)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
