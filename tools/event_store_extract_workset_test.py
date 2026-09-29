#!/usr/bin/env python3
"""Deterministic local fixtures only; no live devices, network or service control."""

import contextlib
import argparse
import base64
import cProfile
import hashlib
import json
import os
import pstats
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

import event_store_extract_workset as e
import event_store_migrate as m


MACHINE = "FIXTURE104"


class WorksetTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.addCleanup(self.cleanup)
        self.source = self.root / "archive.db"
        self.target = self.root / "work.db"
        self.manifest = Path(str(self.target) + ".manifest.json")
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.executescript(e.EVENT_DDL + ";" + e.STATE_DDL + ";")
            for ddl in e.INDEXES.values():
                db.execute(ddl)
            # Latest row does NOT carry the maximum business generation.
            events = [
                self.event(2, "change:v1:FIXTURE104:1:10", "change", 1),
                self.event(5, None, "ota_status", 1),
                self.event(7, "alarm:v1:FIXTURE104:2:high:4", "alarm", 1),
                self.event(9, "management:v1:fixture", "ota_status", 0),
                self.event(50, "change:v1:FIXTURE104:1:3", "change", 0),
                self.event(51, "change:v1:FIXTURE104:1:3", "change", 0, "secondary"),
            ]
            db.executemany("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * len(e.EC)) + ")", events)
            db.execute("INSERT INTO mqtt_event_state VALUES('change:1','change',1,'',0,1.5,1,100,'value:10',101)")
            db.execute("INSERT INTO mqtt_event_state VALUES('alarm:2:high','alarm',2,'high',1,42.0,1,100,'raised:4',101)")
            db.execute("UPDATE sqlite_sequence SET seq=500 WHERE name='mqtt_event_outbox'")
            db.commit()

    def cleanup(self):
        for path in self.root.rglob("*"):
            if path.is_file():
                path.chmod(0o600)
        self.temporary.cleanup()

    @staticmethod
    def event(row_id, event_id, kind, sent, target="main"):
        return (row_id, event_id, target, "old-claim" if not sent else None,
                900 if not sent else None, kind, "fixture/topic", '{"exact": "bytes", "n": 1}',
                100, "202609", 101, sent, 102 if sent else None, 2, "timeout")

    def args(self, command="extract", extra=()):
        args = [command, "--source", str(self.source), "--machine-code", MACHINE,
                "--page-size", "2", "--page-timeout-ms", "1000"]
        if command == "extract":
            args += ["--target", str(self.target), "--offline", "--source-frozen",
                     "--max-workset-bytes", "1048576", "--reserve-free-bytes", "0"]
        return e.parser().parse_args(args + list(extra))

    def freeze(self):
        for suffix in ("", "-wal", "-journal"):
            path = Path(str(self.source) + suffix)
            if path.exists():
                path.chmod(0o444)

    def mutate(self, sql, params=()):
        self.source.chmod(0o600)
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.execute(sql, params)

    @staticmethod
    def rows(path, sql):
        with contextlib.closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as db:
            return db.execute(sql).fetchall()

    def fails(self, pattern):
        self.freeze()
        before = e.physical(self.source, hashes=True)
        with self.assertRaisesRegex((e.ExtractError, m.MigrationError, sqlite3.Error, OSError), pattern):
            e.run(self.args())
        self.assertEqual(before, e.physical(self.source, hashes=True))
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    def test_extract_exact_selection_states_sequence_manifest_and_source(self):
        self.freeze()
        before = e.physical(self.source, hashes=True)
        result = e.run(self.args())
        self.assertEqual(result["publication"], "published")
        self.assertFalse(result["deployment_accepted"])
        self.assertEqual(result["selected_count"], 4)
        self.assertEqual(result["scanned_count"], 6)
        self.assertEqual(result["state_count"], 2)
        columns = ",".join(e.EC)
        self.assertEqual(self.rows(self.target, f"SELECT {columns} FROM {e.EVENT} ORDER BY id"),
                         self.rows(self.source, f"SELECT {columns} FROM {e.EVENT} WHERE ({e.SELECTION}) ORDER BY id"))
        self.assertEqual(self.rows(self.target, f"SELECT * FROM {e.STATE} ORDER BY state_key"),
                         self.rows(self.source, f"SELECT * FROM {e.STATE} ORDER BY state_key"))
        self.assertEqual(self.rows(self.target, "SELECT seq FROM sqlite_sequence"), [(500,)])
        self.assertEqual(before, e.physical(self.source, hashes=True))
        receipt = json.loads(self.manifest.read_text())
        self.assertEqual(receipt["target_sha256"], m.sha256(self.target))
        self.assertEqual(receipt["origin"], before)
        state = next(s for s in receipt["generation_coverage"] if s["state_key"] == "change:1")
        self.assertEqual((state["generation"], state["max_event_generation"], state["last_row_id"]), (10, 10, 51))
        self.assertEqual(self.target.stat().st_nlink, 1)
        with contextlib.closing(sqlite3.connect(self.target)) as db, db:
            db.execute("INSERT INTO mqtt_event_outbox(event_type,topic,payload,event_ts,event_month,created_at) "
                       "VALUES('ota_status','next','{}',100,'202609',101)")
            self.assertEqual(db.execute("SELECT max(id) FROM mqtt_event_outbox").fetchone()[0], 501)

    def test_r2_one_audit_two_source_hash_passes(self):
        self.freeze()
        with mock.patch.object(e, "audit", wraps=e.audit) as audit, \
                mock.patch.object(e, "physical", wraps=e.physical) as physical:
            result = e.run(self.args())
        self.assertEqual(audit.call_count, 1)
        self.assertIsNotNone(audit.call_args.kwargs["sink"])
        self.assertEqual(sum(bool(call.kwargs.get("hashes")) for call in physical.call_args_list), 2)
        self.assertEqual(result["timing"]["source_audit_passes"], 1)
        self.assertEqual(result["timing"]["source_hash_passes"], 2)
        self.assertEqual(result["timing"]["source_hashed_bytes"], 2 * self.source.stat().st_size)

    def test_archive_rows_validated_without_per_row_json_digest(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.executemany("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")",
                           (self.event(i, None, "change", 1) for i in range(1000, 2000)))
        self.freeze()
        with mock.patch.object(e, "typed_row", wraps=e.typed_row) as typed, \
                mock.patch.object(e, "check_event", wraps=e.check_event) as check:
            result = e.run(self.args())
        self.assertEqual(result["scanned_count"], 1006)
        self.assertEqual(check.call_count, 5)
        self.assertLess(typed.call_count, 200)
        self.assertIsNone(result["scanned_ids_sha256"])
        self.assertEqual(result["anonymous_legacy"]["sent_archived"], 1000)

    def test_anonymous_groups_reject_invalid_headers_and_nonempty_ids(self):
        cases = [("event_type", "unknown", "unknown event type"),
                 ("target_id", "invalid target!", "invalid target"),
                 ("sent", 2, "sent"), ("sent", 0.5, "sent"),
                 ("event_id", "unknown", "unrecognized business"),
                 ("event_id", b"", "unrecognized business")]
        for field, value, message in cases:
            with self.subTest(field=field, value=value):
                self.mutate("UPDATE mqtt_event_outbox SET event_id=NULL,event_type='change',target_id='main',sent=1 WHERE id=2")
                self.mutate("UPDATE mqtt_event_outbox SET " + field + "=? WHERE id=2", (value,))
                self.fails(message)

    def test_numeric_windows_skip_large_gaps_and_end_transactions(self):
        self.mutate("UPDATE mqtt_event_outbox SET id=9000000000000000000 WHERE id=51")
        self.mutate("UPDATE sqlite_sequence SET seq=9000000000000000000 WHERE name='mqtt_event_outbox'")
        self.freeze()
        queries, windows = [], []
        original = e.Reader.query
        def query(reader, sql, params=()):
            result = original(reader, sql, params)
            self.assertFalse(reader.db.in_transaction)
            queries.append(sql)
            return result
        with mock.patch.object(e.Reader, "query", new=query):
            result = e.run(self.args("preflight"), page_hook=lambda r, c: windows.append(c))
        self.assertEqual(result["scanned_count"], 6)
        self.assertEqual(result["selected_count"], 4)
        self.assertLessEqual(len(windows), 6)
        self.assertLess(len(queries), 80)
        self.assertEqual(result["scanned_through"], 9000000000000000000)

    def test_grouped_null_and_empty_rows_match_full_selection_and_groups(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.executemany("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")",
                           (self.event(100 + i, None if i % 2 else "", kind, sent, target)
                            for i, (kind, sent, target) in enumerate(
                                (k, s, t) for k in ("change", "alarm", "ota_status")
                                for s in (0, 1) for t in ("main", "secondary"))))
        self.freeze()
        result = e.run(self.args(extra=("--page-size", "256")))
        expected = self.rows(self.source, f"SELECT target_id,event_type,sent,count(*) FROM {e.EVENT} GROUP BY target_id,event_type,sent ORDER BY 1,2,3")
        self.assertEqual([(g["target"], g["event_type"], g["sent"], g["count"]) for g in result["groups"]], expected)
        self.assertEqual(self.rows(self.target, f"SELECT * FROM {e.EVENT} ORDER BY id"),
                         self.rows(self.source, f"SELECT * FROM {e.EVENT} WHERE {e.SELECTION} ORDER BY id"))
        self.assertEqual(result["anonymous_legacy"], {"sent_archived": 4, "pending_preserved": 4})

    def test_archived_null_row_payload_change_detected_without_logical_ids_digest(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_id=NULL WHERE id=2")
        self.freeze()
        original = e.target_digests
        def change_archived_payload(db, page_size):
            self.mutate("UPDATE mqtt_event_outbox SET payload='changed archived bytes' WHERE id=2")
            self.freeze()
            return original(db, page_size)
        with mock.patch.object(e, "target_digests", side_effect=change_archived_payload):
            with self.assertRaisesRegex(e.ExtractError, "frozen source changed"):
                e.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    def test_late_audit_failure_discards_partially_populated_stage(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_id='unknown-modern-id' WHERE id=51")
        self.freeze()
        original = e.audit
        copied_counts = []
        def audit(reader, sink=None, page_hook=None):
            def inspect(_reader, _cursor):
                copied_counts.append(sink.execute("SELECT count(*) FROM mqtt_event_outbox").fetchone()[0])
            return original(reader, sink=sink, page_hook=inspect)
        with mock.patch.object(e, "audit", side_effect=audit):
            with self.assertRaisesRegex(e.ExtractError, "unrecognized business"):
                e.run(self.args())
        self.assertTrue(any(copied_counts))
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())
        self.assertEqual(list(self.root.glob(".event-workset-*")), [])

    def test_target_tamper_rejected_before_publication(self):
        self.freeze()
        original = e.target_digests
        cases = [("UPDATE mqtt_event_outbox SET payload='tampered' WHERE id=50", "selected target digest"),
                 ("UPDATE mqtt_event_state SET updated_at=999", "state target digest"),
                 ("UPDATE sqlite_sequence SET seq=1", "target sequence")]
        for sql, message in cases:
            with self.subTest(sql=sql):
                def tamper(db, page_size):
                    db.execute(sql)
                    return original(db, page_size)
                with mock.patch.object(e, "target_digests", side_effect=tamper):
                    with self.assertRaisesRegex(e.ExtractError, message):
                        e.run(self.args())
                self.assertFalse(self.target.exists())
                self.assertFalse(self.manifest.exists())

    def test_final_source_data_version_checked_independently_of_hashes(self):
        self.freeze()
        before = e.physical(self.source, hashes=True)
        original_physical, original_digests = e.physical, e.target_digests
        def same_hash(*args, **kwargs):
            return before if kwargs.get("hashes") else original_physical(*args, **kwargs)
        def mutate_after_audit(db, page_size):
            self.mutate("UPDATE mqtt_event_state SET updated_at=777")
            self.freeze()
            return original_digests(db, page_size)
        with mock.patch.object(e, "physical", side_effect=same_hash), \
                mock.patch.object(e, "target_digests", side_effect=mutate_after_audit):
            with self.assertRaisesRegex(e.ExtractError, "data_version changed"):
                e.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    @unittest.skipUnless(os.name == "posix", "open inode replacement uses POSIX rename")
    def test_final_source_identity_replacement_rejected(self):
        self.freeze()
        original = e.target_digests
        def replace_source(db, page_size):
            replacement = self.root / "replacement.db"
            shutil.copyfile(self.source, replacement)
            replacement.chmod(0o444)
            replacement.replace(self.source)
            return original(db, page_size)
        with mock.patch.object(e, "target_digests", side_effect=replace_source):
            with self.assertRaisesRegex(e.ExtractError, "frozen source changed"):
                e.run(self.args())
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    def test_online_advisory_short_transactions_watermark_and_no_outputs(self):
        commits = []
        def hook(reader, cursor):
            self.assertFalse(reader.db.in_transaction)
            if commits:
                return
            with contextlib.closing(sqlite3.connect(self.source, timeout=0)) as writer, writer:
                writer.execute("UPDATE mqtt_event_state SET updated_at=999")
                writer.execute("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")",
                               self.event(700, "change:v1:FIXTURE104:1:11", "change", 0))
            commits.append(cursor)
        result = e.run(self.args("preflight"), page_hook=hook)
        self.assertTrue(commits)
        self.assertTrue(result["observation_changed"])
        self.assertEqual(result["watermark_start"]["max_id"], 51)
        self.assertEqual(result["watermark_end"]["max_id"], 700)
        self.assertEqual(result["scanned_count"], 6)
        self.assertEqual(result["status"], "ONLINE_ADVISORY_REAUDIT_OFFLINE")
        self.assertFalse(result["consistent_snapshot"])
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())
        self.freeze()
        with self.assertRaisesRegex(e.ExtractError, "behind historical"):
            e.run(self.args())

    def test_source_query_timeout_rolls_back_transaction(self):
        with contextlib.closing(e.Reader(self.source, self.args("preflight"))) as reader:
            reader.args.page_timeout_ms = 1
            with self.assertRaisesRegex(sqlite3.OperationalError, "interrupted"):
                reader.query("WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<10000000) SELECT sum(x) FROM n")
            self.assertFalse(reader.db.in_transaction)
            self.assertEqual(reader.query("SELECT 1")[0][0], 1)

    def test_busy_page_retries_after_ending_transaction(self):
        with contextlib.closing(e.Reader(self.source, self.args("preflight"))) as reader:
            with contextlib.closing(sqlite3.connect(self.source, timeout=0, isolation_level=None)) as writer:
                writer.execute("BEGIN EXCLUSIVE")
                sleeps = []
                def release(_delay):
                    self.assertFalse(reader.db.in_transaction)
                    sleeps.append(_delay)
                    writer.execute("ROLLBACK")
                with mock.patch.object(e.time, "sleep", side_effect=release):
                    result = reader.query("SELECT id FROM mqtt_event_outbox ORDER BY id LIMIT 1")
                self.assertEqual(result[0][0], 2)
                self.assertEqual(reader.busy_retries, 1)
                self.assertEqual(len(sleeps), 1)
                self.assertFalse(reader.db.in_transaction)

    def test_busy_budget_exhaustion_is_bounded_and_rolls_back(self):
        with contextlib.closing(e.Reader(self.source, self.args("preflight"))) as reader:
            reader.args.busy_retry_ms = 0
            with contextlib.closing(sqlite3.connect(self.source, timeout=0, isolation_level=None)) as writer:
                writer.execute("BEGIN EXCLUSIVE")
                with self.assertRaisesRegex(e.ExtractError, "retry budget exhausted"):
                    reader.query("SELECT id FROM mqtt_event_outbox LIMIT 1")
                self.assertFalse(reader.db.in_transaction)
                writer.execute("ROLLBACK")

    def test_extended_locked_code_retried_other_errors_not_retried(self):
        error = sqlite3.OperationalError("synthetic shared-cache lock")
        error.sqlite_errorcode = 262  # SQLITE_LOCKED_SHAREDCACHE, primary 6.
        with contextlib.closing(e.Reader(self.source, self.args("preflight"))) as reader:
            db = reader.db
            class Once:
                raised = False
                def __getattr__(self, name):
                    return getattr(db, name)
                def execute(self, sql, params=()):
                    if sql == "SELECT 1" and not self.raised:
                        self.raised = True
                        raise error
                    return db.execute(sql, params)
            reader.db = Once()
            def outside(_delay):
                self.assertFalse(db.in_transaction)
            with mock.patch.object(e.time, "sleep", side_effect=outside):
                self.assertEqual(reader.query("SELECT 1")[0][0], 1)
            self.assertEqual(reader.busy_retries, 1)
            with mock.patch.object(e.time, "sleep") as sleep:
                with self.assertRaises(sqlite3.OperationalError):
                    reader.query("SELECT no_such_column")
                sleep.assert_not_called()
        self.assertEqual(e.sqlite_primary_code(sqlite3.OperationalError("database is locked")), 5)
        self.assertEqual(e.sqlite_primary_code(sqlite3.OperationalError("database table is locked")), 6)
        self.assertIsNone(e.sqlite_primary_code(sqlite3.OperationalError("interrupted")))

    def test_generation_numeric_not_last_id_or_lexicographic(self):
        self.mutate("UPDATE mqtt_event_state SET lifecycle='value:9' WHERE state_key='change:1'")
        self.fails("behind historical")

    def test_historical_sent_row_without_state_blocks(self):
        self.mutate("DELETE FROM mqtt_event_state WHERE state_key='alarm:2:high'")
        self.fails("missing state")

    def test_strict_lifecycle_variants(self):
        for life in ("value:", "value:01", "value:-1", "value:3junk", "raised:10", "value:9223372036854775807"):
            with self.subTest(life=life):
                self.mutate("UPDATE mqtt_event_state SET lifecycle=? WHERE state_key='change:1'", (life,))
                self.fails("lifecycle|generation")

    def test_alarm_active_lifecycle_must_agree(self):
        self.mutate("UPDATE mqtt_event_state SET lifecycle='cleared:4' WHERE state_key='alarm:2:high'")
        self.fails("lifecycle")

    def test_all_targets_and_sent_types_are_audited(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_id='change:v1:OTHER:1:10',target_id='other' WHERE id=2")
        self.fails("unrecognized business")

    def test_unknown_sent_event_type_is_not_silently_archived(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_type='unknown' WHERE id=2")
        self.fails("unknown event type")

    def test_anonymous_sent_archived_and_pending_preserved_for_migrate(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_id=NULL WHERE id=2")
        self.mutate("UPDATE mqtt_event_outbox SET event_id='' WHERE id=50")
        self.freeze()
        result = e.run(self.args())
        self.assertEqual(result["anonymous_legacy"], {"sent_archived": 1, "pending_preserved": 1})
        self.assertEqual(self.rows(self.target, "SELECT id,event_id FROM mqtt_event_outbox WHERE id IN (2,50)"), [(50, "")])
        state = next(s for s in result["generation_coverage"] if s["state_key"] == "change:1")
        self.assertEqual(state["max_event_generation"], 3)

    def test_unknown_nonempty_business_id_still_blocks(self):
        self.mutate("UPDATE mqtt_event_outbox SET event_id='legacy-unknown-nonempty' WHERE id=2")
        self.fails("unrecognized business")

    def test_inactive_historical_states_preserved_without_enabling_routes(self):
        self.mutate("INSERT INTO mqtt_event_state VALUES('change:999','change',999,'',0,2.0,1,100,'value:12',101)")
        self.freeze()
        result = e.run(self.args())
        self.assertEqual(result["state_count"], 3)
        unused = next(s for s in result["generation_coverage"] if s["state_key"] == "change:999")
        self.assertIsNone(unused["max_event_generation"])
        self.assertEqual(self.rows(self.target, "SELECT lifecycle FROM mqtt_event_state WHERE state_key='change:999'"), [("value:12",)])

    def test_nonpositive_rowid_and_invalid_sent_block(self):
        self.mutate("UPDATE mqtt_event_outbox SET id=0 WHERE id=2")
        self.fails("nonpositive")
        self.mutate("UPDATE mqtt_event_outbox SET id=2,sent=3 WHERE id=0")
        self.fails("invalid sent")

    def test_highwater_regression_blocks(self):
        self.mutate("UPDATE sqlite_sequence SET seq=10")
        self.fails("sequence below")

    def test_nullable_or_empty_state_key_blocks(self):
        self.mutate("UPDATE mqtt_event_state SET state_key=NULL WHERE state_key='alarm:2:high'")
        self.fails("empty/null")

    def test_state_bound_blocks(self):
        self.freeze()
        with self.assertRaisesRegex(e.ExtractError, "state count limit"):
            e.run(self.args(extra=("--max-states", "1")))
        self.assertFalse(self.target.exists())

    def test_unknown_schema_objects_and_changed_known_trigger_block(self):
        cases = [("CREATE TABLE unrelated(x)", "unknown table", "DROP TABLE unrelated"),
                 ("CREATE VIEW sneaky AS SELECT * FROM mqtt_event_outbox", "unsupported schema", "DROP VIEW sneaky"),
                 ("CREATE TRIGGER mqtt_event_outbox_stats_insert AFTER INSERT ON mqtt_event_outbox BEGIN DELETE FROM mqtt_event_state; END",
                  "altered trigger", "DROP TRIGGER mqtt_event_outbox_stats_insert"),
                 ("CREATE INDEX mystery ON mqtt_event_outbox(payload)", "altered index", "DROP INDEX mystery")]
        for create, pattern, drop in cases:
            with self.subTest(create=create):
                self.mutate(create)
                self.fails(pattern)
                self.mutate(drop)

    def test_known_derived_caches_dropped_not_copied(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.executescript(e.STATS_DDL + ";" + e.META_DDL + ";")
            db.execute("INSERT INTO mqtt_event_outbox_stats VALUES('main','change',22240032,999999,1)")
            db.execute("INSERT INTO mqtt_event_outbox_meta VALUES('stats_migrate_done','1')")
            for sql in e.TRIGGERS.values():
                db.execute(sql)
            db.commit()
        self.freeze()
        result = e.run(self.args())
        self.assertEqual(result["schema"]["omitted_derived_tables"], ["mqtt_event_outbox_meta", "mqtt_event_outbox_stats"])
        self.assertEqual(self.rows(self.target, "SELECT name FROM sqlite_master WHERE type='trigger' OR name LIKE '%stats%'"), [])

    def test_trigger_allowlist_matches_actual_cpp_contract(self):
        source = (Path(__file__).resolve().parents[1] / "src/mqtt_event_outbox.cpp").read_text(encoding="utf-8")
        for name, expected in e.TRIGGERS.items():
            match = re.search(r'"CREATE TRIGGER ' + name + r' "[\s\S]*?\n\s*\);', source)
            self.assertIsNotNone(match, name)
            actual = "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', match.group()))
            self.assertEqual(e.sql_tokens(actual), e.sql_tokens(expected), name)

    def test_known_sql_synonyms_accepted_without_weakening_bodies(self):
        with contextlib.closing(sqlite3.connect(self.source)) as db:
            db.executescript(e.STATS_DDL + ";" + e.META_DDL + ";")
            for sql in e.TRIGGERS.values():
                # SQLite-equivalent operators, identifier quoting, formatting.
                variant = sql.replace("CREATE TRIGGER", "create trigger IF NOT EXISTS")
                variant = variant.replace("mqtt_event_outbox_stats ", '"mqtt_event_outbox_stats" ')
                variant = variant.replace("=0", "==0").replace("=1", "==1")
                db.execute(variant)
            db.execute("DROP INDEX idx_mqtt_event_outbox_event_target")
            db.execute(e.INDEXES["idx_mqtt_event_outbox_event_target"].replace("<>", "!="))
            db.commit()
        self.freeze()
        e.run(self.args())

    def test_changed_known_index_blocks(self):
        self.mutate("DROP INDEX idx_mqtt_event_outbox_pending")
        self.mutate("CREATE INDEX idx_mqtt_event_outbox_pending ON mqtt_event_outbox(payload)")
        self.fails("altered index")

    def test_unknown_table_constraint_blocks(self):
        self.mutate("ALTER TABLE mqtt_event_state ADD COLUMN hidden_data TEXT")
        self.fails("schema mismatch")

    def test_offline_declarations_and_external_freeze_required(self):
        with self.assertRaisesRegex(e.ExtractError, "writable"):
            e.run(self.args())
        self.freeze()
        for flag in ("offline", "source_frozen"):
            args = self.args()
            setattr(args, flag, False)
            with self.assertRaisesRegex(e.ExtractError, "requires --offline"):
                e.run(args)

    def test_frozen_source_mutation_aborts_without_publication(self):
        self.freeze()
        changed = []
        def hook(reader, cursor):
            if not changed:
                self.mutate("UPDATE mqtt_event_state SET updated_at=888")
                self.freeze()
                changed.append(cursor)
        with self.assertRaisesRegex(e.ExtractError, "frozen source changed"):
            e.run(self.args(), page_hook=hook)
        self.assertFalse(self.target.exists())
        self.assertFalse(self.manifest.exists())

    def test_low_disk_and_workset_budget_abort(self):
        self.freeze()
        with mock.patch.object(e.shutil, "disk_usage", return_value=shutil._ntuple_diskusage(100, 99, 1)):
            with self.assertRaisesRegex(e.ExtractError, "insufficient free"):
                e.run(self.args())
        with self.assertRaisesRegex(e.ExtractError, "workset size limit"):
            e.run(self.args(extra=("--max-workset-bytes", "16384")))
        self.assertFalse(self.target.exists())

    def test_existing_target_or_manifest_not_overwritten(self):
        self.freeze()
        for path in (self.target, self.manifest):
            path.write_bytes(b"owned by someone else")
            with self.assertRaises((e.ExtractError, m.MigrationError)):
                e.run(self.args())
            self.assertEqual(path.read_bytes(), b"owned by someone else")
            path.unlink()

    def test_hot_rollback_journal_rejected_readonly(self):
        journal = Path(str(self.source) + "-journal")
        journal.write_bytes(b"unrecovered journal")
        self.freeze()
        with self.assertRaisesRegex(e.ExtractError, "rollback journal"):
            e.run(self.args())
        self.assertEqual(journal.read_bytes(), b"unrecovered journal")

    def test_online_active_delete_journal_is_not_an_offline_corruption_gate(self):
        with contextlib.closing(sqlite3.connect(self.source, isolation_level=None)) as writer:
            writer.execute("BEGIN IMMEDIATE")
            writer.execute("UPDATE mqtt_event_state SET updated_at=123456")
            journal = Path(str(self.source) + "-journal")
            self.assertGreater(journal.stat().st_size, 0)
            def end_write(reader, cursor):
                if writer.in_transaction:
                    writer.execute("ROLLBACK")
            result = e.run(self.args("preflight"), page_hook=end_write)
            self.assertEqual(result["status"], "ONLINE_ADVISORY_REAUDIT_OFFLINE")
            self.assertIn("-journal", result["origin_start"])
            self.assertEqual(self.rows(self.source, "SELECT DISTINCT updated_at FROM mqtt_event_state"), [(101,)])

    @unittest.skipUnless(os.name == "posix", "POSIX flock and file links")
    def test_symlink_hardlink_and_cooperative_writer_lock(self):
        alias = self.root / "alias.db"
        os.link(self.source, alias)
        with self.assertRaisesRegex(m.MigrationError, "one hard link"):
            e.run(self.args("preflight"))
        alias.unlink()
        alias.symlink_to(self.source)
        args = self.args("preflight")
        args.source = str(alias)
        with self.assertRaisesRegex(m.MigrationError, "symlink"):
            e.run(args)
        alias.unlink()
        import fcntl
        self.freeze()
        with self.source.open("rb") as writer:
            fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                e.run(self.args())

    def test_wal_committed_rows_included_without_checkpoint(self):
        with contextlib.closing(sqlite3.connect(self.source)) as writer:
            writer.execute("PRAGMA journal_mode=WAL")
            writer.execute("PRAGMA wal_autocheckpoint=0")
            writer.execute("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")",
                           self.event(600, "change:v1:FIXTURE104:1:9", "change", 0))
            writer.commit()
            self.freeze()
            before = e.physical(self.source, hashes=True)
            self.assertIn("-wal", before)
            result = e.run(self.args())
            self.assertEqual(result["selected_count"], 5)
            self.assertEqual(result["watermark_start"]["max_id"], 600)
            self.assertEqual(before, e.physical(self.source, hashes=True))
            # Allow SQLite fixture cleanup only after archive-preservation assertions.
            self.source.chmod(0o600)
            Path(str(self.source) + "-wal").chmod(0o600)

    def test_extract_migrate_increment_rollback_end_to_end(self):
        self.freeze()
        e.run(self.args())
        owners = self.root / "owners.json"
        owners.write_text(json.dumps({"change:1": "event-engine", "alarm:2:high": "event-engine"}))
        backups = self.root / "backups"
        backups.mkdir()
        migrated = self.root / "migrated.db"
        args = m.parser().parse_args(["migrate", "--source", str(self.target), "--target", str(migrated),
            "--backup-dir", str(backups), "--offline", "--state-owners", str(owners),
            "--store-id", "fixture-store", "--config-generation", "fixture-v1"])
        report = m.run(args)
        self.assertEqual(report["events"], 4)
        self.assertEqual(self.rows(migrated, "SELECT seq FROM sqlite_sequence WHERE name='mqtt_event_outbox'"), [(500,)])
        with contextlib.closing(sqlite3.connect(migrated)) as db, db:
            db.execute("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")",
                       self.event(501, "change:v1:FIXTURE104:1:11", "change", 0))
            db.execute("UPDATE mqtt_event_state SET lifecycle='value:11' WHERE state_key='change:1'")
            db.execute("UPDATE event_store_state_version SET version=2 WHERE state_key='change:1'")
            db.execute("UPDATE mqtt_event_outbox SET sent=1 WHERE id=50")
        rollback = self.root / "rollback.db"
        result = m.run(m.parser().parse_args(["rollback", "--source", str(migrated),
            "--baseline", report["cutover_baseline"], "--target", str(rollback),
            "--backup-dir", str(backups), "--offline"]))
        self.assertEqual(result["events"], 5)
        self.assertEqual(self.rows(rollback, "SELECT lifecycle FROM mqtt_event_state WHERE state_key='change:1'"), [("value:11",)])
        self.assertEqual(self.rows(rollback, "SELECT id,sent FROM mqtt_event_outbox WHERE id IN (2,50,501) ORDER BY id"), [(50, 1), (501, 0)])

    def test_cli_preflight_json_and_error_exit(self):
        tool = Path(e.__file__)
        run = subprocess.run([sys.executable, "-B", str(tool), "preflight", "--source", str(self.source),
                              "--machine-code", MACHINE], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertFalse(json.loads(run.stdout)["deployment_accepted"])
        failed = subprocess.run([sys.executable, "-B", str(tool), "extract", "--source", str(self.source),
                                 "--machine-code", MACHINE, "--target", str(self.target)], capture_output=True, text=True)
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(json.loads(failed.stderr)["status"], "FAILED")

    def rollback_fixture(self):
        owners = self.root / "rollback-owners.json"
        owners.write_text(json.dumps({"change:1": "event-engine", "alarm:2:high": "event-engine"}))
        backups = self.root / "rollback-backups"
        backups.mkdir()
        migrated = self.root / "modern.db"
        report = m.run(m.parser().parse_args([
            "migrate", "--source", str(self.source), "--target", str(migrated),
            "--backup-dir", str(backups), "--offline", "--state-owners", str(owners),
            "--store-id", "fixture-store", "--config-generation", "fixture-v1"]))
        # Read only checked-in runtime literals, not the extractor's whitelist or
        # captured production DDL. This independently catches specification drift.
        runtime = (Path(e.__file__).resolve().parents[1] / "src/mqtt_event_outbox.cpp").read_text(encoding="utf-8")
        statements = []
        for match in re.finditer(r'execOnceOrThrow\(db,\s*((?:"(?:\\.|[^"\\])*"\s*)+)\);', runtime):
            sql = "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
            if re.match(r"CREATE (TABLE|INDEX|TRIGGER) IF NOT EXISTS (event_store_|event_local_journal|event_journal_meta|event_history_projection_cursor|idx_event_local_journal)", sql):
                statements.append(sql)
        self.assertEqual(len(statements), 13)
        with contextlib.closing(sqlite3.connect(migrated)) as db, db:
            for sql in statements:
                db.execute(sql)
            db.execute("INSERT INTO event_local_journal VALUES(1,'change','old-journal',1,100,'',0,0,1,1,0,'1','gw','dev','point',1,'fixture-v1')")
        rollback = self.root / "rollback-source.db"
        m.run(m.parser().parse_args([
            "rollback", "--source", str(migrated), "--baseline", report["cutover_baseline"],
            "--target", str(rollback), "--backup-dir", str(backups), "--offline"]))
        self.source = rollback

    def test_real_rollback_explicit_opt_in_preserves_archive_and_selection(self):
        self.rollback_fixture()
        with self.assertRaisesRegex(e.ExtractError, "unknown or altered index: event_store_state_owner"):
            e.run(self.args("preflight"))
        advisory = e.run(self.args("preflight", ("--allow-rollback-schema",)))
        self.assertFalse(advisory["consistent_snapshot"])
        self.freeze()
        before = e.physical(self.source, hashes=True)
        result = e.run(self.args(extra=("--allow-rollback-schema",)))
        self.assertEqual(before, e.physical(self.source, hashes=True))
        self.assertEqual(result["selected_count"], 4)
        self.assertEqual(result["not_copied_outbox_rows"], 2)
        self.assertEqual(self.rows(self.source, "SELECT event_id FROM event_local_journal"), [("old-journal",)])
        self.assertEqual(self.rows(self.target, f"SELECT * FROM {e.EVENT} ORDER BY id"),
                         self.rows(self.source, f"SELECT * FROM {e.EVENT} WHERE ({e.SELECTION}) ORDER BY id"))
        self.assertEqual(self.rows(self.target, f"SELECT * FROM {e.STATE} ORDER BY state_key"),
                         self.rows(self.source, f"SELECT * FROM {e.STATE} ORDER BY state_key"))
        receipt = json.loads(self.manifest.read_text())
        extensions = {r["name"] for r in receipt["schema"]["source_extension_objects"]}
        self.assertTrue(set(e.ROLLBACK_TABLES) <= extensions)
        omitted = {r["name"] for r in receipt["schema"]["not_copied_objects"]}
        self.assertTrue(extensions <= omitted)
        self.assertNotIn("event_local_journal", receipt["schema"]["omitted_derived_tables"])
        target_names = {r[0] for r in self.rows(self.target, "SELECT name FROM sqlite_master")}
        self.assertFalse(extensions & target_names)

    def test_rollback_rejects_every_altered_extension_and_unknown_object(self):
        self.rollback_fixture()
        cases = []
        for name in e.ROLLBACK_TABLES:
            cases.append(([f"ALTER TABLE {name} ADD COLUMN unexpected TEXT"],
                          [f"ALTER TABLE {name} DROP COLUMN unexpected"], "schema mismatch"))
        for name, ddl in e.ROLLBACK_INDEXES.items():
            cases.append(([f"DROP INDEX {name}", f"CREATE INDEX {name} ON mqtt_event_outbox(payload)"],
                          [f"DROP INDEX {name}", ddl], "altered index"))
        name = "event_local_journal_immutable"
        cases.append(([f"DROP TRIGGER {name}", f"CREATE TRIGGER {name} BEFORE UPDATE ON event_local_journal BEGIN SELECT 1; END"],
                      [f"DROP TRIGGER {name}", m.JOURNAL_SCHEMA[3]], "altered trigger"))
        cases += [(["CREATE TABLE event_unknown(x)"], ["DROP TABLE event_unknown"], "unknown table"),
                  (["CREATE INDEX event_unknown ON mqtt_event_outbox(payload)"], ["DROP INDEX event_unknown"], "altered index"),
                  (["CREATE TRIGGER event_unknown AFTER INSERT ON mqtt_event_state BEGIN SELECT 1; END"], ["DROP TRIGGER event_unknown"], "altered trigger"),
                  (["CREATE VIEW event_unknown AS SELECT 1"], ["DROP VIEW event_unknown"], "unsupported schema object")]
        for changes, restore, error in cases:
            with self.subTest(sql=changes):
                for sql in changes:
                    self.mutate(sql)
                self.freeze()
                before = e.physical(self.source, hashes=True)
                for command in ("preflight", "extract"):
                    with self.assertRaisesRegex(e.ExtractError, error):
                        e.run(self.args(command, ("--allow-rollback-schema",)))
                self.assertEqual(before, e.physical(self.source, hashes=True))
                self.assertFalse(self.target.exists())
                self.assertFalse(self.manifest.exists())
                for sql in restore:
                    self.mutate(sql)

    def test_captured_42mb_schema_matches_local_spec_without_executing_source_sql(self):
        artifact = Path(e.__file__).resolve().parents[1] / "artifacts/builds/event-store-104-retry-20260909/schema-01.json"
        if not artifact.exists():
            self.skipTest("optional captured schema artifact unavailable")
        captured = json.loads(artifact.read_text(encoding="utf-8"))
        self.assertEqual(captured["bytes"], 42266624)
        class SchemaRow(tuple):
            def keys(self):
                return ("type", "name", "tbl_name", "sql")
            def __getitem__(self, key):
                return super().__getitem__(self.keys().index(key) if isinstance(key, str) else key)
        rows = [SchemaRow(r) for r in captured["schema"]]
        rows.append(SchemaRow(("table", "sqlite_sequence", "sqlite_sequence", e.TABLES["sqlite_sequence"])))
        reader = types.SimpleNamespace(args=self.args("preflight", ("--allow-rollback-schema",)),
                                       query=lambda sql: rows)
        result = e.audit_schema(reader)
        self.assertIn("event_local_journal", {r["name"] for r in result["source_extension_objects"]})

    def test_zero_pending_preserves_all_953_states_including_dynamic_points(self):
        self.rollback_fixture()
        self.mutate("UPDATE mqtt_event_outbox SET sent=1")
        with contextlib.closing(sqlite3.connect(self.source)) as db, db:
            db.executemany("INSERT INTO mqtt_event_state VALUES(?,?,?,?,?,?,?,?,?,?)",
                [(f"change:{i}", "change", i, "", 0, 1.5, 1, 100, "value:0", 101)
                 for i in range(100000, 100951)])
        self.freeze()
        before = e.physical(self.source, hashes=True)
        result = e.run(self.args(extra=("--allow-rollback-schema",)))
        self.assertEqual(result["state_count"], 953)
        self.assertEqual(result["selected_count"], 2)  # Sent management remains selected.
        self.assertEqual(self.rows(self.target, "SELECT count(*) FROM mqtt_event_outbox WHERE sent=0"), [(0,)])
        self.assertEqual(self.rows(self.target, f"SELECT * FROM {e.STATE} ORDER BY state_key"),
                         self.rows(self.source, f"SELECT * FROM {e.STATE} ORDER BY state_key"))
        self.assertEqual(before, e.physical(self.source, hashes=True))

    def test_rollback_unknown_and_duplicate_sequences_are_rejected(self):
        self.rollback_fixture()
        for name in ("event_unknown", "event_local_journal", e.EVENT):
            with self.subTest(name=name):
                self.mutate("INSERT INTO sqlite_sequence VALUES(?,1)", (name,))
                try:
                    with self.assertRaisesRegex(e.ExtractError, "unknown/duplicate sqlite_sequence"):
                        e.run(self.args("preflight", ("--allow-rollback-schema",)))
                finally:
                    self.mutate("DELETE FROM sqlite_sequence WHERE rowid=(SELECT max(rowid) FROM sqlite_sequence)")


R1_SHA256 = "19ed796d3572303888d5a28735f78b6b77821de91fb0d2f907803f2ba5f6ec44"


def benchmark(options):
    """Opt-in local throughput experiment, never a production acceptance test.

    Uses the exact hash-verified r1 implementation, not a reimplementation of its
    double scan. Both implementations use the same frozen fixture, page settings
    and output checks. No cache flushing or performance pass/fail threshold.
    """
    if options.baseline_base64_stdin:
        chunks = []
        for line in sys.stdin.buffer:
            if line.strip() == b".":
                break
            chunks.append(line.strip())
        source = base64.b64decode(b"".join(chunks), validate=True)
    else:
        source = Path(options.baseline_file).read_bytes()
    if hashlib.sha256(source).hexdigest() != R1_SHA256:
        raise ValueError("benchmark baseline is not the frozen r1 SHA256")
    baseline = types.ModuleType("workset_r1_benchmark")
    baseline.__file__ = "<hash-verified-r1>"
    exec(compile(source, baseline.__file__, "exec"), baseline.__dict__)
    anonymous, identified = options.anonymous_rows, options.identified_rows
    if not (0 <= anonymous <= 5000000 and 0 <= identified <= 5000000 and anonymous + identified > 0):
        raise ValueError("invalid benchmark row counts")
    state_count, pending = 953, 8
    generations = [0] * state_count
    with tempfile.TemporaryDirectory(prefix="workset-throughput-") as temp:
        root = Path(temp)
        archive = root / "archive.db"
        try:
            with contextlib.closing(sqlite3.connect(archive)) as db:
                db.executescript(e.EVENT_DDL + ";" + e.STATE_DDL + ";")
                def fixture_rows():
                    payload = '{"fixture":"' + "x" * 160 + '"}'
                    for i in range(anonymous + identified + pending):
                        sent = int(i < anonymous + identified)
                        event_id = None
                        if i >= anonymous:
                            slot = (i - anonymous) % state_count
                            generations[slot] += 1
                            event_id = f"change:v1:{MACHINE}:{slot + 1}:{generations[slot]}"
                        yield (i + 1, event_id, "main", None, None, "change", "fixture/topic",
                               payload, 100, "202609", 101, sent, 102 if sent else None, 0, None)
                db.executemany("INSERT INTO mqtt_event_outbox VALUES(" + ",".join("?" * 15) + ")", fixture_rows())
                db.executemany("INSERT INTO mqtt_event_state VALUES(?,?,?,?,?,?,?,?,?,?)",
                    ((f"change:{i + 1}", "change", i + 1, "", 0, 1.5, 1, 100, f"value:{generation}", 101)
                     for i, generation in enumerate(generations)))
                db.execute("UPDATE sqlite_sequence SET seq=seq+1000 WHERE name='mqtt_event_outbox'")
                for ddl in e.INDEXES.values():
                    db.execute(ddl)
                db.commit()
            archive.chmod(0o444)
            results = {}
            for label, module in (("r1", baseline), ("r2", e)):
                args = module.parser().parse_args(["extract", "--source", str(archive), "--machine-code", MACHINE,
                    "--target", str(root / (label + ".db")), "--offline", "--source-frozen",
                    "--page-size", str(options.page_size), "--page-timeout-ms", "1000",
                    "--max-workset-bytes", "1048576", "--reserve-free-bytes", "0"])
                started = time.monotonic()
                report = module.run(args)
                elapsed = time.monotonic() - started
                results[label] = dict(elapsed_seconds=elapsed, source_queries=report["source_queries"],
                    scanned_count=report["scanned_count"], selected_count=report["selected_count"],
                    state_count=report["state_count"], selected_sha256=report["selected_sha256"],
                    states_sha256=report["states_sha256"], scanned_ids_sha256=report["scanned_ids_sha256"],
                    generation_coverage_sha256=hashlib.sha256(e.encoded(report["generation_coverage"])).hexdigest(),
                    target_sha256=report["target_sha256"], timing=report["timing"],
                    source_bytes=archive.stat().st_size, target_bytes=report["target_bytes"])
            for key in ("scanned_count", "selected_count", "state_count", "selected_sha256",
                        "states_sha256", "generation_coverage_sha256", "target_sha256"):
                if results["r1"][key] != results["r2"][key]:
                    raise AssertionError("benchmark r1/r2 mismatch: " + key)
            if results["r2"]["selected_count"] != pending:
                raise AssertionError("benchmark selection mismatch")
            n = anonymous + identified + pending
            scan = results["r2"]["timing"]["audit_with_copy_seconds"]
            profile_report = None
            if options.profile_r2:
                args.target = str(root / "r2-profile.db")
                profiler = cProfile.Profile()
                profiler.runcall(e.run, args)
                stats = pstats.Stats(profiler)
                top = sorted(stats.stats.items(), key=lambda item: item[1][3], reverse=True)[:18]
                profile_report = [dict(function=key[2], file=Path(key[0]).name, line=key[1],
                    calls=value[1], self_seconds=value[2], cumulative_seconds=value[3]) for key, value in top]
            return dict(status="BENCHMARK_COMPLETE", baseline_sha256=R1_SHA256,
                r2_sha256=m.sha256(Path(e.__file__)), python=sys.version, sqlite=sqlite3.sqlite_version,
                anonymous_sent=anonymous, identified_sent=identified, pending=pending,
                page_size=options.page_size, results=results,
                r2_rows_per_second=n / scan,
                elapsed_speedup=results["r1"]["elapsed_seconds"] / results["r2"]["elapsed_seconds"],
                hypothetical_same_mix_scan_seconds_22240032=22240032 * scan / n,
                separate_profile_run=profile_report,
                production_estimate="r2: one measured offline-equivalent scan + two device hash passes + small target validation; r1: two scans + three hashes",
                limitations="local synthetic mixed IDs; cached fixture; r1 then r2, one run each; not target-device I/O or proof of a 20-minute cutover")
        finally:
            archive.chmod(0o600) if archive.exists() else None


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--benchmark":
        p = argparse.ArgumentParser(description=benchmark.__doc__)
        group = p.add_mutually_exclusive_group(required=True)
        group.add_argument("--baseline-file")
        group.add_argument("--baseline-base64-stdin", action="store_true")
        p.add_argument("--anonymous-rows", type=int, default=500000)
        p.add_argument("--identified-rows", type=int, default=500000)
        p.add_argument("--page-size", type=int, default=256)
        p.add_argument("--profile-r2", action="store_true",
                       help="extra local profile run; excluded from r1/r2 timing comparison")
        print(json.dumps(benchmark(p.parse_args(sys.argv[2:])), ensure_ascii=True, sort_keys=True))
    else:
        unittest.main(verbosity=2)
