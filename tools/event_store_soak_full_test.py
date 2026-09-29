#!/usr/bin/env python3
"""Synthetic journal/projection fixtures only. Does not claim runtime or 24h coverage."""
import argparse
import contextlib
import json
from pathlib import Path
import shutil
import sqlite3
import sys
import types
import unittest

sys.dont_write_bytecode = True
import event_store_soak as soak
import event_store_soak_journal as journal
import event_store_soak_library as library_check


IDENTITY = dict(storeId=soak.ipc.STORE_ID, configGeneration=soak.ipc.GENERATION, journalGeneration='fixture-generation')


def fixture(output):
    output.mkdir()
    with contextlib.closing(sqlite3.connect(str(output / 'ledger.sqlite'))) as db:
        db.executescript('CREATE TABLE events(eventId TEXT PRIMARY KEY);'
            'CREATE TABLE local_expected(eventId TEXT PRIMARY KEY,ordinal INTEGER,expected TEXT);')
        for i in range(1, 7):
            event = soak.ipc.event('fixture-' + str(i), event_type='alarm' if i % 2 else 'change')
            local = journal.local_event(event, i, 1, IDENTITY['configGeneration'])
            db.execute('INSERT INTO events VALUES(?)', (event['eventId'],))
            db.execute('INSERT INTO local_expected VALUES(?,?,?)', (event['eventId'], i, json.dumps(local)))
        db.commit()
    with contextlib.closing(sqlite3.connect(str(output / 'events.db'))) as db:
        db.executescript('''CREATE TABLE event_store_identity(id INTEGER, store_id TEXT, config_generation TEXT, schema_version INTEGER);
            CREATE TABLE event_journal_meta(id INTEGER,schema_version INTEGER,journal_generation TEXT);
            CREATE TABLE event_history_projection_cursor(id INTEGER,projection_id TEXT,projected_through INTEGER,cleaned_through INTEGER);
            CREATE TABLE event_local_journal(id INTEGER,kind TEXT,event_id TEXT,point_index INTEGER,ts INTEGER,
                alarm_type TEXT,active INTEGER,threshold REAL,value REAL,quality INTEGER,stale INTEGER,persist_value TEXT,
                gateway_code TEXT,device_code TEXT,point_code TEXT,state_version INTEGER,config_generation TEXT);''')
        db.execute('INSERT INTO event_store_identity VALUES(1,?,?,1)', (IDENTITY['storeId'], IDENTITY['configGeneration']))
        db.execute('INSERT INTO event_journal_meta VALUES(1,1,?)', (IDENTITY['journalGeneration'],))
        db.execute("INSERT INTO event_history_projection_cursor VALUES(1,'alarm-history-v1',6,0)")
        with contextlib.closing(journal.readonly(output / 'ledger.sqlite')) as ledger:
            for row in ledger.execute('SELECT * FROM local_expected'):
                local = json.loads(row['expected'])
                mapping = journal.JOURNAL_FIELDS
                columns = ['id'] + list(mapping.values())
                values = [row['ordinal']] + [local[key] for key in mapping]
                db.execute('INSERT INTO event_local_journal(' + ','.join(columns) + ') VALUES(' +
                           ','.join('?' for _ in values) + ')', values)
        db.commit()
    with contextlib.closing(sqlite3.connect(str(output / 'history.db'))) as db:
        db.executescript('''CREATE TABLE alarm_projection_meta(id INTEGER,projection_id TEXT,store_id TEXT,
            journal_generation TEXT,last_contiguous_journal_id INTEGER);
            CREATE TABLE alarm_events(id INTEGER,event_id TEXT,point_index INTEGER,ts INTEGER,alarm_type TEXT,
                active INTEGER,threshold REAL,value REAL,quality INTEGER,stale INTEGER,persist_value TEXT,
                gateway_code TEXT,device_code TEXT,point_code TEXT);''')
        db.execute("INSERT INTO alarm_projection_meta VALUES(1,'alarm-history-v1',?,?,6)",
                   (IDENTITY['storeId'], IDENTITY['journalGeneration']))
        with contextlib.closing(journal.readonly(output / 'events.db')) as source:
            for row in source.execute("SELECT * FROM event_local_journal WHERE kind='alarm'"):
                columns = ['id'] + list(journal.FIELDS.values())
                db.execute('INSERT INTO alarm_events(' + ','.join(columns) + ') VALUES(' +
                           ','.join('?' for _ in columns) + ')', [row[key] for key in columns])
        db.commit()
    soak.dump(output / 'FIXTURE.json', dict(synthetic=True, actualRuntimeExecuted=False, accepted24h=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    baseline = args.output / 'valid'
    fixture(baseline)
    cases = []

    def check(name, action):
        try:
            action()
            cases.append(dict(name=name, passed=True))
        except Exception as error:
            cases.append(dict(name=name, passed=False, error=repr(error)))

    def valid():
        report = journal.reconcile(baseline, IDENTITY)
        assert report['passed'], report
        assert report['expectedLocal'] == 6 and report['historyRows'] == 3 and report['historyWatermark'] == 6
        soak.dump(baseline / 'reconciliation.json', report)

    check('alarm only history with change-inclusive watermark', valid)
    mutations = [
        ('missing-journal', 'events.db', "DELETE FROM event_local_journal WHERE event_id='fixture-2'", 'missingJournal'),
        ('wrong-state-version', 'events.db', 'UPDATE event_local_journal SET state_version=99 WHERE id=2', 'journalMismatch'),
        ('wrong-config-generation', 'events.db', "UPDATE event_local_journal SET config_generation='wrong' WHERE id=2", 'journalMismatch'),
        ('missing-alarm', 'history.db', 'DELETE FROM alarm_events WHERE id=1', 'missingHistory'),
        ('wrong-alarm-value', 'history.db', 'UPDATE alarm_events SET value=99 WHERE id=1', 'historyMismatch'),
        ('wrong-alarm-codes', 'history.db', "UPDATE alarm_events SET gateway_code='wrong' WHERE id=1", 'historyMismatch'),
        ('wrong-history-generation', 'history.db', "UPDATE alarm_projection_meta SET journal_generation='wrong'", 'errors'),
        ('wrong-source-generation', 'events.db', "UPDATE event_journal_meta SET journal_generation='wrong'", 'errors'),
        ('lagging-watermark', 'history.db', 'UPDATE alarm_projection_meta SET last_contiguous_journal_id=5', 'errors'),
        ('watermark-past-source', 'history.db', 'UPDATE alarm_projection_meta SET last_contiguous_journal_id=7', 'errors'),
        ('lagging-source-cursor', 'events.db', 'UPDATE event_history_projection_cursor SET projected_through=5', 'errors'),
        ('cleaned-source', 'events.db', 'UPDATE event_history_projection_cursor SET cleaned_through=1', 'errors'),
        ('duplicate-history', 'history.db', 'INSERT INTO alarm_events SELECT * FROM alarm_events WHERE id=1', 'historyMismatch'),
        ('extra-journal', 'events.db', "UPDATE event_local_journal SET event_id='extra' WHERE id=2", 'extraJournal'),
        ('change-in-history', 'history.db', "UPDATE alarm_events SET event_id='fixture-2' WHERE id=1", 'extraHistory'),
    ]
    for name, database, sql, field in mutations:
        def mutate(name=name, database=database, sql=sql, field=field):
            target = args.output / name
            shutil.copytree(baseline, target)
            with contextlib.closing(sqlite3.connect(str(target / database))) as db:
                db.execute(sql)
                db.commit()
            report = journal.reconcile(target, IDENTITY)
            soak.dump(target / 'reconciliation.json', report)
            assert not report['passed'] and report[field], report
        check(name, mutate)

    def local_payload():
        # Exercise the real producer builder, replacing only its RPC transport.
        runner = soak.Runner.__new__(soak.Runner)
        runner.args = types.SimpleNamespace(mode='full', payload_bytes=8)
        runner.pseq = [0]
        runner.meta = dict(runId='builder-fixture', hello=dict(configGeneration=IDENTITY['configGeneration']))
        runner.db = sqlite3.connect(':memory:')
        runner.db.executescript('CREATE TABLE events(eventId TEXT,digest TEXT,generated REAL,committed REAL);'
            'CREATE TABLE local_expected(eventId TEXT,ordinal INTEGER,expected TEXT);CREATE TABLE latency(kind TEXT,ms REAL);'
            'CREATE TABLE state_updates(producerId TEXT,sequence INTEGER,expected TEXT,confirmed INTEGER DEFAULT 0);')
        captured = []
        def rpc(op, payload):
            captured.append(payload)
            return dict(receipt=dict(committed=True, sequence=payload['sequence'])), 1
        runner.rpc = rpc
        try:
            runner.produce(0, 6, 0)
            payload = captured[0]
            assert len(payload['events']) == len(payload['localEvents']) == 6
            assert len(payload['states']) == 1 and payload['states'][0]['expectedVersion'] == '0'
            assert payload['states'][0]['sourceTs'] == payload['localEvents'][-1]['ts']
            for i, (event, local) in enumerate(zip(payload['events'], payload['localEvents']), 1):
                assert local['eventId'] == event['eventId'] and local['kind'] == event['eventType']
                assert local['stateVersion'] == str(i) and local['configGeneration'] == IDENTITY['configGeneration']
                assert set(local) == set(journal.JOURNAL_FIELDS)
                for key in ('index', 'quality', 'ts', 'stateVersion'):
                    assert isinstance(local[key], str)
        finally:
            runner.db.close()
    check('real producer localEvents encoder with stub transport', local_payload)

    def private_library():
        target = args.output / 'private-library-inputs'
        target.mkdir()
        file = target / 'libsqlite3.so'
        file.write_bytes(b'synthetic hash-validation input; not a real shared library')
        expected = soak.sha(file)
        manifest = target / 'SHA256SUMS'
        manifest.write_text(expected + '  libsqlite3.so\n', encoding='ascii')
        assert library_check.validate(target, expected) == file.resolve()
        file.write_bytes(b'changed')
        with unittest.TestCase().assertRaises(ValueError):
            library_check.validate(target, expected)
        expected = soak.sha(file)
        manifest.write_text(expected + '  libsqlite3.so\n' + expected + '  libsqlite3.so\n', encoding='ascii')
        with unittest.TestCase().assertRaises(ValueError):
            library_check.validate(target, expected)
        manifest.write_text(expected + '  libsqlite3.so\n', encoding='ascii')
        real = target / 'other.so'
        file.rename(real)
        file.symlink_to(real.resolve())
        with unittest.TestCase().assertRaises(ValueError):
            library_check.validate(target, expected)
    check('fixed private SQLite hash, tamper, duplicate manifest, symlink rejection', private_library)

    def full_report():
        target = args.output / 'synthetic-full-report'
        shutil.copytree(baseline, target)
        with contextlib.closing(sqlite3.connect(str(target / 'ledger.sqlite'))) as db:
            for column in ('digest TEXT', 'generated REAL', 'committed REAL', 'claimed REAL',
                           'acked REAL', 'rowId TEXT', 'claims INTEGER'):
                db.execute('ALTER TABLE events ADD COLUMN ' + column)
            db.executescript('UPDATE events SET generated=1,committed=2,claimed=3,acked=4,claims=1;'
                             'CREATE TABLE latency(kind TEXT,ms REAL);CREATE TABLE anomalies(kind TEXT,eventId TEXT,detail TEXT);')
            db.commit()
        meta = dict(schemaVersion=1, coverageMode='full', journalIdentity=IDENTITY, syntheticReportFixture=True,
            status='completed', purpose='soak', scope='SYNTHETIC REPORT FIXTURE; no runtime executed',
            actualLoadSeconds=86400, targetEvents=6, pendingAtEnd=0, restarts=[], notCovered=['actual runtime', '24h'],
            config=dict(duration=86400, min_rate_ratio=.99))
        soak.dump(target / 'manifest.json', meta)
        (target / 'resources.jsonl').write_text('', encoding='utf-8')
        result = soak.report(target)
        assert not result['reconciliationPassed'] and result['journalProjection']['passed'] and not result['accepted24h']
        assert not result['stateReconciliation']['passed'] and result['stateReconciliation']['errors']
        assert 'journalGeneration' in (target / 'report.html').read_text()
        assert 'journalStatus' in (target / 'journal_reconciliation.csv').read_text()
        assert soak.report(target) == result
        with contextlib.closing(sqlite3.connect(str(target / 'history.db'))) as db:
            db.execute('DELETE FROM alarm_events WHERE id=1')
            db.commit()
        damaged = soak.report(target)
        assert not damaged['reconciliationPassed'] and damaged['journalProjection']['missingHistory'] == 1
    check('legacy journal-only fixture cannot pass full state CAS; regeneration rechecks history', full_report)
    summary = dict(syntheticFixtureTests=True, runtimeFullModeVerified=False, accepted24h=False,
                   passed=all(case['passed'] for case in cases), cases=cases)
    soak.dump(args.output / 'selftest.json', summary)
    print(json.dumps(summary, indent=2))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
