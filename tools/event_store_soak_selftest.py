#!/usr/bin/env python3
"""Short real-IPC selftests and deliberate report corruption tests. Never a 24h run."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode = True
import event_store_soak as soak


def backup_database(source, destination):
    """Read the committed SQLite view, including WAL, without checkpointing source."""
    soak.journal_check.backup(source, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=['events', 'full'], default='full')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    tool = str(Path(soak.__file__).resolve())
    records = []
    binary_hash = soak.sha(args.binary)

    def check(name, action):
        start = time.monotonic()
        try:
            detail = action()
            records.append(dict(name=name, passed=True, detail=detail, seconds=time.monotonic() - start))
        except Exception as error:
            records.append(dict(name=name, passed=False, error=repr(error), traceback=traceback.format_exc(),
                                seconds=time.monotonic() - start))

    def run_case(profile, mode):
        output = args.output / (profile + '-' + mode)
        cmd = [sys.executable, tool, 'run', '--binary', str(args.binary.resolve()),
               '--expect-sha256', binary_hash, '--output', str(output), '--duration', '8',
               '--rate', '20', '--sample-every', '1', '--restart-every', '3',
               '--restart-mode', mode, '--drop-replies', '2', '--profile', profile, '--ephemeral-db',
               '--mode', args.mode]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        (args.output / (profile + '-' + mode + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        report = json.loads((output / 'report.json').read_text())
        assert result.returncode == 0, report.get('error', result.stderr)
        assert report['verdict'] == 'PASS_TOOL_SELFTEST' and not report['accepted24h']
        assert report['recoveredResponses'] == 2 and len(report['restarts']) >= 1
        assert report['counts']['generated'] == report['counts']['acked'] >= 100
        assert report['resources']['samples'] >= 3
        if args.mode == 'full':
            assert report['journalProjection']['passed']
            assert report['journalProjection']['expectedLocal'] == report['counts']['generated']
            assert report['stateReconciliation']['passed'] and report['stateReconciliation']['expectedStates'] == 2
            assert report['stateReconciliation']['confirmedUpdates'] == report['latency']['commitConfirm']['samples']
            assert all(r['stateRecovery']['passed'] and r['stateRecovery']['expectedStates'] == 2 for r in report['restarts'])
        assert report['latency']['commitConfirm']['samples'] > 0
        with (output / 'events.csv').open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == report['counts']['generated']
        assert len({row['eventId'] for row in rows}) == len(rows)
        assert all(row['acked'] and row['missingAck'] == '0' for row in rows)
        for suffix in ('md', 'html'):
            assert soak.human_report.verdict_label(report) in (output / ('report.' + suffix)).read_text()
        assert report['counts']['submitted'] == report['counts']['generated']
        assert report['operations']['unknownResponses'] == 2
        assert report['operations']['unknownRequests'] == report['operations']['recoveredUnknownRequests'] == 2
        assert report['operations']['retryAttempts'] == 2
        assert report['operations']['unresolvedUnknownRequests'] == 0
        assert report['operations']['submittedBatches'] == report['latency']['commitConfirm']['samples']
        rebuilt = soak.report(output)
        assert rebuilt == report, 'offline report is not reproducible'
        return dict(events=report['counts']['generated'], actualLoadSeconds=report['actualLoadSeconds'],
                    restarts=len(report['restarts']), recovered=report['recoveredResponses'],
                    states=report.get('stateReconciliation'))

    for profile in ('delete-full', 'wal-full'):
        for mode in ('term', 'kill'):
            check(profile + '-' + mode, lambda p=profile, m=mode: run_case(p, m))

    baseline = args.output / 'wal-full-kill'

    def corrupt(name, sql=None, change=None):
        output = args.output / name
        output.mkdir()
        files = ['ledger.sqlite', 'manifest.json', 'resources.jsonl']
        if args.mode == 'full':
            files.extend(['events.db', 'history.db'])
        for file in files:
            if file.endswith(('.sqlite', '.db')):
                backup_database(baseline / file, output / file)
            else:
                shutil.copy2(baseline / file, output / file)
        if sql:
            with sqlite3.connect(str(output / 'ledger.sqlite')) as db:
                db.executescript(sql)
        manifest = json.loads((output / 'manifest.json').read_text())
        if change:
            change(manifest)
        manifest.update(syntheticReportFixture=True, scope='SYNTHETIC REPORT FIXTURE; elapsed fields may be invented')
        soak.dump(output / 'manifest.json', manifest)
        return soak.report(output)

    def missing():
        result = corrupt('missing-event', 'UPDATE events SET acked=NULL WHERE eventId=(SELECT min(eventId) FROM events);')
        assert not result['reconciliationPassed'] and result['counts']['missingAck'] == 1
        return result['verdict']

    def extra():
        result = corrupt('extra-event', "INSERT INTO anomalies VALUES('unexpected','synthetic','selftest');")
        assert not result['reconciliationPassed'] and result['counts']['anomalies'] == 1
        return result['verdict']

    def duration():
        def update(meta):
            meta['purpose'] = 'soak'
            meta['config']['duration'] = 86400
        result = corrupt('short-is-not-24h', change=update)
        assert not result['accepted24h']
        return result['verdict']

    def interrupted():
        result = corrupt('unfinished', change=lambda m: m.update(status='running', actualLoadSeconds=90000))
        assert not result['accepted24h'] and not result['reconciliationPassed']
        return result['verdict']

    def hash_guard():
        output = args.output / 'bad-hash'
        p = subprocess.run([sys.executable, tool, 'run', '--binary', str(args.binary.resolve()),
            '--expect-sha256', '0' * 64, '--output', str(output)], capture_output=True, text=True)
        assert p.returncode == 2 and not output.exists()
        return 'blocked before process creation'

    def preflight():
        output = args.output / 'preflight'
        p = subprocess.run([sys.executable, tool, 'preflight', '--binary', str(args.binary.resolve()),
            '--ephemeral-db', '--output', str(output), '--mode', args.mode], capture_output=True, text=True, timeout=20)
        result = json.loads((output / 'report.json').read_text())
        assert p.returncode == 0 and result['verdict'] == 'PREFLIGHT_PASSED'
        assert result['counts']['acked'] == 1 and not result['accepted24h']
        return 'real append/claim/ack capability verified'

    def rate_gate():
        def update(meta):
            meta.update(purpose='soak', actualLoadSeconds=86400, targetEvents=100000)
            meta['config']['duration'] = 86400
        result = corrupt('rate-gap', change=update)
        assert not result['accepted24h'] and result['verdict'] == 'FAILED_RATE_TARGET'
        return 'synthetic report fixture; no elapsed-time claim'

    def wal_backup():
        source = args.output / 'wal-backup-source.sqlite'
        target = args.output / 'wal-backup-copy.sqlite'
        with sqlite3.connect(str(source)) as writer:
            writer.execute('PRAGMA journal_mode=WAL')
            writer.execute('PRAGMA wal_autocheckpoint=0')
            writer.execute('CREATE TABLE probe(id INTEGER PRIMARY KEY)')
            writer.executemany('INSERT INTO probe VALUES(?)', [(n,) for n in range(200)])
            writer.commit()
            wal = Path(str(source) + '-wal')
            assert wal.stat().st_size > 0
            before = {str(p): soak.sha(p) for p in (source, wal)}
            backup_database(source, target)
            assert before == {str(p): soak.sha(p) for p in (source, wal)}, 'backup changed source DB/WAL'
            with sqlite3.connect(str(target)) as copy:
                assert copy.execute('SELECT id FROM probe ORDER BY id').fetchall() == [(n,) for n in range(200)]
        return '200 committed WAL rows backed up from read-only source; source DB/WAL hashes unchanged'

    def percentiles():
        with sqlite3.connect(':memory:') as db:
            db.execute('CREATE TABLE latency(kind TEXT,ms REAL)')
            db.executemany('INSERT INTO latency VALUES(?,?)', [('known', i) for i in range(1, 101)])
            result = soak.quantiles(db, 'known')
            assert [result[k] for k in ('p50', 'p95', 'p99', 'max')] == [50, 95, 99, 100]
            assert soak.quantiles(db, 'absent')['p99'] is None
        return 'exact nearest-rank and empty sample behavior'

    def html_escape():
        result = corrupt('html-escaping', change=lambda m: m.update(error='</pre><script>alert(1)</script>'))
        text = (args.output / 'html-escaping' / 'report.html').read_text()
        assert '<script>' not in text and '&lt;script&gt;' in text
        return 'untrusted diagnostics are escaped'

    def launcher_guard():
        launcher = str(Path(tool).with_name('event_store_soak_22_16.sh'))
        syntax = subprocess.run(['bash', '-n', launcher], capture_output=True, text=True)
        assert syntax.returncode == 0, syntax.stderr
        rejected = subprocess.run(['bash', launcher, '--check-only', '/tmp', '0' * 64],
                                  capture_output=True, text=True)
        assert rejected.returncode == 2 and 'Refusing non-isolated' in rejected.stderr
        return 'shell syntax and non-lab path rejection; namespaces not executed'

    def term_harness():
        output = args.output / 'interrupted-live'
        p = subprocess.Popen([sys.executable, tool, 'run', '--binary', str(args.binary.resolve()),
            '--duration', '120', '--ephemeral-db', '--output', str(output), '--mode', args.mode],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                stream = output / 'operations.jsonl'
                if stream.exists() and 'AckBatch' in stream.read_text():
                    break
                if p.poll() is not None:
                    raise AssertionError('runner exited before interruption')
                time.sleep(.1)
            else:
                raise AssertionError('runner did not become active')
            p.terminate()
            p.communicate(timeout=15)
            result = json.loads((output / 'report.json').read_text())
            assert p.returncode == 1 and result['status'] in ('interrupted', 'failed') and not result['accepted24h']
            samples = [json.loads(line) for line in (output / 'resources.jsonl').read_text().splitlines()]
            assert samples and not Path('/proc/' + str(samples[-1]['pid'])).exists(), 'child process leaked'
            return 'partial report saved; child reaped'
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate(timeout=10)

    for name, action in (('missing event detection', missing), ('unexpected event detection', extra),
                         ('short cannot pass 24h', duration), ('unfinished cannot pass', interrupted),
                         ('binary hash guard', hash_guard), ('SIGTERM partial report and cleanup', term_harness),
                         ('real IPC preflight', preflight), ('rate gate', rate_gate),
                         ('read-only WAL backup fixture', wal_backup),
                         ('exact quantiles', percentiles), ('HTML escaping', html_escape),
                         ('22.16 launcher static and rejection', launcher_guard)):
        check(name, action)
    summary = dict(schemaVersion=1, suite='event-store-soak-tool-selftest', binarySha256=binary_hash,
                   coverageMode=args.mode, passed=all(row['passed'] for row in records), accepted24h=False, cases=records)
    soak.dump(args.output / 'selftest.json', summary)
    print(json.dumps(summary, indent=2))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
