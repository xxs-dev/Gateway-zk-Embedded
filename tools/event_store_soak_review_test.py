#!/usr/bin/env python3
"""Focused r13 regression checks; no remote access or build commands."""
import argparse
import contextlib
import copy
import io
import json
from pathlib import Path
import sqlite3
import sys
import threading
import time
import types
from unittest import mock

sys.dont_write_bytecode = True
import event_store_soak as soak
import event_store_soak_journal as journal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report-source', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []
    def passed(name):
        cases.append(dict(name=name, passed=True))
    source = args.output / 'busy.sqlite'
    with contextlib.closing(sqlite3.connect(str(source))) as db:
        db.execute('CREATE TABLE sample(value INTEGER)')
        db.execute('INSERT INTO sample VALUES(1)')
        db.commit()
    ready = threading.Event()
    def hold_lock():
        with contextlib.closing(sqlite3.connect(str(source))) as db:
            db.execute('BEGIN EXCLUSIVE')
            ready.set()
            time.sleep(1.4)
            db.rollback()
    thread = threading.Thread(target=hold_lock)
    thread.start()
    assert ready.wait(5)
    calls = []
    def read_locked():
        calls.append(True)
        with contextlib.closing(journal.readonly(source)) as db:
            return db.execute('SELECT value FROM sample').fetchone()[0]
    try:
        assert journal.retry_read(read_locked) == 1 and len(calls) >= 2
    finally:
        thread.join(5)
    assert not thread.is_alive()
    passed('real DELETE SQLite lock released after 1s succeeds through bounded retry')
    for message, expected_calls in (('database is locked', 4), ('no such table: invalid', 1)):
        count = []
        def failure():
            count.append(True)
            raise sqlite3.OperationalError(message)
        try:
            journal.retry_read(failure)
            raise AssertionError('expected read error')
        except sqlite3.OperationalError:
            pass
        assert len(count) == expected_calls
        passed('BUSY finite four attempts' if expected_calls == 4 else 'non-BUSY error is not retried')
    results = iter([dict(passed=False, retryableReadError=True), dict(passed=True)])
    assert journal.retry_read(lambda: next(results))['passed']
    passed('state reconciler BUSY result restarts whole read transaction')
    runner = soak.Runner.__new__(soak.Runner)
    runner.args = types.SimpleNamespace(mode='full', drain_timeout=0)
    runner.meta = {}
    runner.store = types.SimpleNamespace(database=source, history=source)
    runner.db = sqlite3.connect(':memory:')
    runner.db.executescript('CREATE TABLE local_expected(id INTEGER); INSERT INTO local_expected VALUES(1);')
    status = dict(sourceRows=1, sourceHighId=1, historyWatermark=1, projectedThrough=1, cleanedThrough=1)
    with mock.patch.object(journal, 'progress', return_value=status):
        try:
            runner.wait_projection()
            raise AssertionError('nonzero cleaned cursor passed')
        except RuntimeError:
            pass
    status['cleanedThrough'] = 0
    with mock.patch.object(journal, 'progress', return_value=status):
        runner.wait_projection()
    runner.db.close()
    passed('projection wait requires cleanedThrough zero, aligned with final reconciliation')
    store = soak.Store.__new__(soak.Store)
    store.rpc_timeout = 47
    with mock.patch.object(soak.ipc.LabStore, 'call', return_value='stub') as call:
        assert store.call('GetStats', {}) == 'stub'
        assert call.call_args.kwargs['timeout'] == 47
        store.call('Hello', {}, timeout=.25)
        assert call.call_args.kwargs['timeout'] == .25
    assert soak.ipc.RPC_TIMEOUT == 5.0
    passed('soak timeout independent from unchanged IPC 5s; startup explicit timeout preserved')
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            soak.main(['run', '--binary', '/bin/true', '--output', str(args.output / 'invalid-timeout'), '--rpc-timeout', 'nan'])
            raise AssertionError('NaN RPC timeout accepted')
        except SystemExit as exc:
            assert exc.code == 2
    assert not (args.output / 'invalid-timeout').exists()
    passed('invalid RPC timeout rejected before runner creation')
    report = copy.deepcopy(json.loads((args.report_source / 'report.json').read_text()))
    report['journalProjection'].update(expectedAlarms=3, historyRows=2)
    tables = dict(soak.human_report.sections(report))
    rows = {row[0]: row for row in tables['源 journal 与历史投影']}
    assert rows['应投影 alarm'][2] == '独立 ledger 期望值' and rows['历史 alarm 行数'][2] == '不通过'
    passed('expected alarm count labelled as ledger expectation; actual history count compared against it')
    result = dict(passed=True, accepted24h=False, remoteConnections=0, cases=cases)
    soak.dump(args.output / 'selftest.json', result)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
