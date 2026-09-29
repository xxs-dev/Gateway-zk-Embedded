#!/usr/bin/env python3
"""Laboratory-only EventStore soak runner; standard library, Linux/WSL.

An independent SQLite ledger belongs to the harness, never to EventStore.
The existing IPC test module supplies only framing and proc sampling helpers.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

sys.dont_write_bytecode = True
import event_store_delivery_ipc_test as ipc
import event_store_soak_journal as journal_check
import event_store_soak_report as human_report
import event_store_soak_state as state_check


def dump(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=True, allow_nan=False) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def content_hash(event):
    return hashlib.sha256(ipc.encode({k: event[k] for k in
        ('eventId', 'eventType', 'topic', 'payload', 'eventTs')})).hexdigest()


class Store(ipc.LabStore):
    def __init__(self, args, output):
        super().__init__(str(args.binary), str(args.sqlite_library or ''), args.profile)
        self.expected_binary_hash = sha(args.binary)
        self.rpc_timeout = args.rpc_timeout
        self.expected_library_hash = sha(args.sqlite_library) if args.sqlite_library else None
        self.directory = output
        self.short = tempfile.TemporaryDirectory(prefix='es-soak-', dir='/tmp')
        self.socket_path = str(Path(self.short.name) / 's')
        self.database = (Path(self.short.name) if args.ephemeral_db else output) / 'events.db'
        self.history = self.database.with_name('history.db')
        self.config_path = output / 'lab.json'
        self.log = (output / 'server.log').open('a+b', buffering=0)
        self.resource_samples = []
        config = dict(laboratoryOnly=True, storeId=ipc.STORE_ID, configGeneration=ipc.GENERATION,
            databasePath=str(self.database), socketPath=self.socket_path,
            sqliteLibraryPath=self.sqlite_library, storageProfile=args.profile,
            producers=['p' + str(i) for i in range(args.producers)],
            senders=[dict(senderId='s' + str(i), targetId='main', eventTypes=['alarm', 'change'])
                     for i in range(args.senders)])
        if args.mode == 'full':
            config['historyPath'] = str(self.history)
        dump(self.config_path, config)

    def sample_resources(self, phase):
        # Continuous samples are streamed by Runner; do not accumulate base-class samples.
        pass

    def start(self):
        if sha(self.binary) != self.expected_binary_hash:
            raise RuntimeError('binary changed before start/restart')
        if self.sqlite_library and sha(self.sqlite_library) != self.expected_library_hash:
            raise RuntimeError('SQLite library changed before start/restart')
        super().start()
        if sha('/proc/' + str(self.process.pid) + '/exe') != self.expected_binary_hash:
            raise RuntimeError('running executable differs from pinned binary')

    def call(self, operation, args, timeout=None):
        return super().call(operation, args, timeout=self.rpc_timeout if timeout is None else timeout)

    def close(self):
        try:
            self.stop()
        finally:
            self.log.close()
            if self.database.parent != self.directory:
                for path in (self.database, self.history):
                    if path.is_file():
                        journal_check.backup(path, self.directory / path.name)
            self.short.cleanup()


class Runner:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.db = sqlite3.connect(str(self.output / 'ledger.sqlite'))
        self.db.executescript('''
            CREATE TABLE events(eventId TEXT PRIMARY KEY, digest TEXT NOT NULL, generated REAL,
                committed REAL, claimed REAL, acked REAL, rowId TEXT, claims INTEGER DEFAULT 0);
            CREATE TABLE anomalies(kind TEXT, eventId TEXT, detail TEXT);
            CREATE TABLE latency(kind TEXT, ms REAL);
            CREATE INDEX latency_order ON latency(kind,ms);
            CREATE TABLE local_expected(eventId TEXT PRIMARY KEY, ordinal INTEGER UNIQUE NOT NULL, expected TEXT NOT NULL);
            CREATE TABLE state_updates(producerId TEXT, sequence INTEGER, expected TEXT NOT NULL,
                confirmed INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(producerId,sequence));
        ''')
        self.journal = (self.output / 'operations.jsonl').open('a', encoding='utf-8', buffering=1)
        self.resources = (self.output / 'resources.jsonl').open('a', encoding='utf-8', buffering=1)
        self.store = None
        self.started = time.monotonic()
        self.pseq = [0] * args.producers
        self.sseq = [0] * args.senders
        self.epochs = {}
        self.sessions = {}
        self.drop_remaining = args.drop_replies
        self.meta = dict(schemaVersion=1, runId=uuid.uuid4().hex, purpose=args.purpose,
            scope='laboratory IPC ' + args.mode + ', synthetic sender; no MQTT broker', status='running',
            startedAt=ipc.utc_now(), platform=platform.platform(), architecture=platform.machine(),
            binary=str(args.binary), binarySha256=sha(args.binary),
            ipcHelperSha256=sha(ipc.__file__), toolSha256=sha(__file__),
            journalHelperSha256=sha(journal_check.__file__),
            stateHelperSha256=sha(state_check.__file__), stateCasVersion=1 if args.mode == 'full' else None,
            reportHelperSha256=sha(human_report.__file__), operationTraceVersion=1,
            coverageMode=args.mode, journalProjection=dict(executed=False, passed=False),
            sqliteLibrary=str(args.sqlite_library or ''),
            sqliteLibrarySha256=sha(args.sqlite_library) if args.sqlite_library else None,
            config={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
            restarts=[], unknownResponses=0, recoveredResponses=0,
            targetEvents=math.floor(args.duration * args.rate) if args.command == 'run' else 0,
            actualLoadSeconds=0, downtimeSeconds=0, pendingAtEnd=None,
            notCovered=['production integration', 'MQTT broker PUBACK', 'power loss',
                'MQTT callback (real PUBACK: event_store_transport_test)',
                'host reboot', 'migration/rollback', 'CAS conflict rejection', 'concurrent IPC clients',
                'internal SQLite commit duration', 'durability beyond process restart'])
        dump(self.output / 'manifest.json', self.meta)

    def record(self, kind, **values):
        self.journal.write(json.dumps(dict(kind=kind, utc=ipc.utc_now(),
            elapsed=time.monotonic() - self.started, **values), ensure_ascii=True) + '\n')
        self.journal.flush()
        os.fsync(self.journal.fileno())

    def anomaly(self, kind, event_id='', detail=''):
        self.db.execute('INSERT INTO anomalies VALUES(?,?,?)', (kind, event_id, detail))

    def rpc(self, op, args):
        actor_key = 'producerId' if 'producerId' in args else 'senderId'
        actor = args.get(actor_key)
        began = time.monotonic()
        self.record('request', op=op, args=args)
        last = None
        for attempt in range(4):
            try:
                self.record('rpc_attempt', op=op, attempt=attempt)
                value = self.store.call(op, args).value
                if self.drop_remaining and attempt == 0 and op in ('AppendEventsAndStates', 'AckBatch'):
                    self.drop_remaining -= 1
                    self.record('injected_reply_loss', op=op)
                    raise OSError('injected: complete reply discarded by harness')
                if value.get('ok') is not True:
                    if value.get('outcome') == 'not_committed':
                        self.record('rejected', op=op, response=value)
                        raise RuntimeError('definite rejection: ' + json.dumps(value))
                    raise OSError('unknown response: ' + json.dumps(value))
                if actor:
                    if value.get(actor_key) != actor or value.get('sequence') != args.get('sequence', '0'):
                        raise OSError('receipt identity/sequence mismatch')
                    if op.startswith('Register'):
                        if value.get('sessionId') != args['sessionId'] or value.get('epoch') != '1':
                            raise OSError('registration identity mismatch')
                    elif value.get('epoch') != args['epoch'] or value.get('sessionId') != self.sessions[actor]:
                        raise OSError('receipt epoch/session mismatch')
                self.record('response', op=op, response=value, attempt=attempt)
                if attempt:
                    self.meta['recoveredResponses'] += 1
                return value, (time.monotonic() - began) * 1000
            except (OSError, AssertionError, ValueError) as error:
                last = error
                self.meta['unknownResponses'] += 1
                self.record('unknown', op=op, error=str(error), attempt=attempt)
                # The actor has only one request in flight. Exact replay preserves sequence and body.
                time.sleep(0.05)
        raise RuntimeError('unresolved request: ' + str(last))

    def register(self):
        for key, count in (('producerId', self.args.producers), ('senderId', self.args.senders)):
            for i in range(count):
                actor = ('p' if key == 'producerId' else 's') + str(i)
                session = self.meta['runId'] + actor
                self.sessions[actor] = session
                self.rpc('RegisterProducer' if key == 'producerId' else 'RegisterSender',
                         {key: actor, 'sessionId': session, 'expectedEpoch': '0'})

    def produce(self, index, count, offset):
        actor = 'p' + str(index)
        sequence = self.pseq[index] + 1
        events = [ipc.event(self.meta['runId'] + ':' + str(offset + j),
            event_type='alarm' if (offset + j) % 5 == 0 else 'change', payload='x' * self.args.payload_bytes)
            for j in range(count)]
        now = time.monotonic()
        self.db.executemany('INSERT INTO events(eventId,digest,generated) VALUES(?,?,?)',
                           [(e['eventId'], content_hash(e), now) for e in events])
        self.db.commit()
        args = dict(producerId=actor, epoch='1', sequence=str(sequence), events=events, states=[])
        if self.args.mode == 'full':
            local_events = [journal_check.local_event(event, offset + j + 1, index + 1,
                           self.meta['hello']['configGeneration']) for j, event in enumerate(events)]
            args['localEvents'] = local_events
            self.db.executemany('INSERT INTO local_expected VALUES(?,?,?)',
                [(event['eventId'], offset + j + 1, json.dumps(event, sort_keys=True))
                 for j, event in enumerate(local_events)])
            baseline = state_check.state(self.meta['runId'], index, sequence, local_events[-1])
            args['states'] = [baseline]
            self.db.execute('INSERT INTO state_updates(producerId,sequence,expected) VALUES(?,?,?)',
                            (actor, sequence, json.dumps(baseline, sort_keys=True)))
            self.db.commit()
        value, latency = self.rpc('AppendEventsAndStates', args)
        receipt = value.get('receipt', {})
        if receipt.get('committed') is not True or receipt.get('sequence') != str(sequence):
            raise RuntimeError('append did not confirm commit')
        self.pseq[index] = sequence
        if self.args.mode == 'full':
            self.db.execute('UPDATE state_updates SET confirmed=1 WHERE producerId=? AND sequence=?', (actor, sequence))
        self.db.executemany('UPDATE events SET committed=? WHERE eventId=?',
                           [(time.monotonic(), e['eventId']) for e in events])
        self.db.execute('INSERT INTO latency VALUES(?,?)', ('commitConfirm', latency))
        self.db.commit()

    def deliver(self, index):
        actor = 's' + str(index)
        seq = self.sseq[index] + 1
        args = dict(senderId=actor, epoch='1', sequence=str(seq), limit=str(self.args.batch_size),
                    maxBytes='32768', leaseMs='30000')
        claim, _ = self.rpc('ClaimBatch', args)
        self.sseq[index] = seq
        messages = claim.get('messages', [])
        if claim.get('status') == 'EMPTY' and not messages:
            return 0
        if claim.get('status') != 'CLAIMED' or not messages:
            raise RuntimeError('invalid claim status')
        valid = True
        for event in messages:
            eid = event['eventId']
            row = self.db.execute('SELECT digest,rowId FROM events WHERE eventId=?', (eid,)).fetchone()
            if row is None:
                self.anomaly('unexpected', eid)
                valid = False
            elif row[0] != content_hash(event) or (row[1] is not None and row[1] != event['id']):
                self.anomaly('content_or_row_mismatch', eid)
                valid = False
            else:
                self.db.execute('UPDATE events SET claimed=?,rowId=?,claims=claims+1 WHERE eventId=?',
                                (time.monotonic(), event['id'], eid))
        self.db.commit()
        if not valid:
            raise RuntimeError('claim reconciliation failed; refusing to acknowledge mismatched data')
        args = dict(senderId=actor, epoch='1', sequence=str(seq + 1), claimToken=claim['claimToken'],
                    items=[dict(id=e['id'], eventId=e['eventId']) for e in messages])
        ack, latency = self.rpc('AckBatch', args)
        expected = {(e['id'], e['eventId']) for e in messages}
        results = ack.get('results', [])
        if (ack.get('status') != 'FINISHED' or len(results) != len(expected) or
                {(e['id'], e['eventId']) for e in results} != expected or
                any(e.get('status') != 'APPLIED' for e in results)):
            raise RuntimeError('AckBatch did not confirm all exact items APPLIED')
        self.sseq[index] = seq + 1
        now = time.monotonic()
        for event in messages:
            generated = self.db.execute('SELECT generated FROM events WHERE eventId=?', (event['eventId'],)).fetchone()[0]
            self.db.execute('UPDATE events SET acked=? WHERE eventId=?', (now, event['eventId']))
            self.db.execute('INSERT INTO latency VALUES(?,?)', ('endToEnd', (now - generated) * 1000))
        self.db.execute('INSERT INTO latency VALUES(?,?)', ('ackConfirm', latency))
        self.db.commit()
        return len(messages)

    def sample(self):
        process = self.store.process
        sample = ipc.resource_sample(process.pid, self.args.profile, 'periodic', 'soak',
                                     running=process.poll() is None)
        sample['elapsedSeconds'] = time.monotonic() - self.started
        sample['filesBytes'] = {p.name: p.stat().st_size for p in self.store.database.parent.glob('events.db*') if p.is_file()}
        sample['filesBytes'].update({p.name: p.stat().st_size for p in self.store.database.parent.glob('history.db*') if p.is_file()})
        if self.args.mode == 'full':
            try:
                sample['projection'] = journal_check.progress(self.store.database, self.store.history)
            except (OSError, sqlite3.Error, ValueError) as error:
                sample['unavailable']['projection'] = str(error)
        sample['harnessPid'] = os.getpid()
        sample['freeDiskBytes'] = min(shutil.disk_usage(self.output).free, shutil.disk_usage(self.store.database.parent).free)
        sample['counts'] = self.db.execute('SELECT count(*),count(committed),count(acked) FROM events').fetchone()
        self.resources.write(json.dumps(sample) + '\n')
        if sample['freeDiskBytes'] < self.args.min_free_mb * 1024 * 1024:
            raise RuntimeError('minimum free disk threshold reached')
        if process.poll() is not None:
            raise RuntimeError('unexpected EventStore exit: ' + str(process.returncode))

    def restart(self):
        began = time.monotonic()
        old = self.store.process.pid
        self.record('restart_begin', pid=old, mode=self.args.restart_mode)
        if self.args.restart_mode == 'kill':
            self.store.process.kill()
            self.store.process.wait(timeout=10)
        else:
            self.store.stop()
        self.store.start()
        if self.args.mode == 'full' and journal_check.retry_read(lambda: journal_check.identity(self.store.database)) != self.meta['journalIdentity']:
            raise RuntimeError('source journal identity changed on restart')
        state_recovery = None
        if self.args.mode == 'full':
            state_recovery = journal_check.retry_read(lambda: state_check.reconcile(self.output / 'ledger.sqlite', self.store.database))
            if not state_recovery['passed']:
                raise RuntimeError('restart state CAS reconciliation failed: ' + json.dumps(state_recovery))
        for key, sequences in (('producerId', self.pseq), ('senderId', self.sseq)):
            for index, sequence in enumerate(sequences):
                actor = ('p' if key == 'producerId' else 's') + str(index)
                value = self.store.call('GetReceipt' if key == 'producerId' else 'GetDeliveryReceipt', {key: actor}).value
                if (value.get('ok') is not True or value.get(key) != actor or value.get('sequence') != str(sequence)
                        or value.get('epoch') != '1' or value.get('sessionId') != self.sessions[actor]):
                    raise RuntimeError('restart lost actor receipt')
        duration = time.monotonic() - began
        result = dict(oldPid=old, newPid=self.store.process.pid, mode=self.args.restart_mode,
                      seconds=duration, boundary='between completed RPC batches', receiptRecovery=True,
                      stateRecovery=state_recovery)
        self.meta['restarts'].append(result)
        self.meta['downtimeSeconds'] += duration
        self.record('restart_end', **result)

    def run(self):
        load_start = None
        try:
            self.store = Store(self.args, self.output)
            self.store.start()
            hello = self.store.call('Hello', {}).value
            if hello.get('synchronous') != '2' or hello.get('version') != '1':
                raise RuntimeError('unsupported protocol/storage settings')
            self.meta['hello'] = hello
            if hello.get('configGeneration') != ipc.GENERATION or hello.get('storeId') != ipc.STORE_ID:
                raise RuntimeError('Hello store identity/configGeneration mismatch')
            if self.args.mode == 'full':
                if hello.get('localJournalVersion') != '1':
                    raise RuntimeError('full mode requires Hello localJournalVersion=1 and historyPath support')
                if hello.get('historyProjectionVersion') != '1' or hello.get('historyEnabled') is not True:
                    raise RuntimeError('full mode requires Hello historyProjectionVersion=1 and historyEnabled=true')
                self.meta['journalIdentity'] = journal_check.retry_read(lambda: journal_check.identity(self.store.database))
                if any(self.meta['journalIdentity'][key] != hello[key] for key in ('storeId', 'configGeneration')):
                    raise RuntimeError('source identity differs from Hello identity')
                if not self.store.history.is_file():
                    raise RuntimeError('full mode historyPath was not initialized by runtime')
            else:
                self.meta['notCovered'].append('local journal and history projection')
                self.meta['notCovered'].append('state baseline updates')
            self.meta['databaseFilesystem'] = subprocess.run(
                ['stat', '-f', '-c', '%T', str(self.store.database.parent)], capture_output=True,
                text=True, check=True).stdout.strip()
            self.register()
            # Empty claim is a capability probe, not a production acceptance signal.
            self.deliver(0)
            self.sample()
            if self.args.command == 'preflight':
                self.produce(0, 1, 0)
                self.deliver(0)
                self.wait_projection()
                self.meta['status'] = 'preflight_complete'
                return
            load_start = time.monotonic()
            deadline = load_start + self.args.duration
            next_sample = load_start
            next_restart = load_start + self.args.restart_every if self.args.restart_every else math.inf
            generated, batch = 0, 0
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now >= next_sample:
                    self.sample()
                    self.meta['actualLoadSeconds'] = now - load_start
                    dump(self.output / 'manifest.json', self.meta)
                    next_sample = now + self.args.sample_every
                if now >= next_restart:
                    self.restart()
                    next_restart = time.monotonic() + self.args.restart_every
                due = min(1 + math.floor((time.monotonic() - load_start) * self.args.rate),
                          math.floor(self.args.duration * self.args.rate))
                count = min(self.args.batch_size, due - generated)
                if count > 0:
                    self.produce(batch % self.args.producers, count, generated)
                    generated += count
                    self.deliver(batch % self.args.senders)
                    batch += 1
                else:
                    time.sleep(min(0.01, max(0, deadline - time.monotonic())))
            self.meta['actualLoadSeconds'] = time.monotonic() - load_start
            self.meta['targetEvents'] = math.floor(self.args.duration * self.args.rate)
            drain_start = time.monotonic()
            while True:
                pending = int(self.store.call('GetStats', {'targetId': 'main'}).value['pendingCount'])
                if not pending:
                    break
                if time.monotonic() - drain_start > self.args.drain_timeout:
                    raise RuntimeError('drain timeout')
                self.deliver(batch % self.args.senders)
                batch += 1
            self.meta['drainSeconds'] = time.monotonic() - drain_start
            self.meta['pendingAtEnd'] = pending
            self.wait_projection()
            self.sample()
            self.meta['status'] = 'completed'
        except BaseException as error:
            self.meta['status'] = 'interrupted' if isinstance(error, KeyboardInterrupt) else 'failed'
            self.meta['error'] = type(error).__name__ + ': ' + str(error)
            if load_start is not None and not self.meta.get('drainSeconds'):
                self.meta['actualLoadSeconds'] = time.monotonic() - load_start
            self.record('failure', error=self.meta['error'])
        finally:
            if self.store:
                try:
                    self.store.close()
                except Exception as error:
                    self.meta.update(status='failed', cleanupError=str(error))
            if self.args.mode == 'full':
                self.meta['journalProjection'] = journal_check.reconcile(self.output, self.meta.get('journalIdentity'))
                self.meta['stateReconciliation'] = state_check.reconcile(self.output / 'ledger.sqlite', self.output / 'events.db',
                                                                       self.output / 'state_reconciliation.csv')
                if not self.meta['stateReconciliation']['passed']:
                    self.meta.update(status='failed', stateReconciliationFailed=True)
                if not self.meta['journalProjection']['passed']:
                    self.meta.update(status='failed', journalReconciliationFailed=True)
            self.meta['finishedAt'] = ipc.utc_now()
            self.meta['totalSeconds'] = time.monotonic() - self.started
            self.db.commit()
            self.db.close()
            self.journal.close()
            self.resources.close()
            dump(self.output / 'manifest.json', self.meta)

    def wait_projection(self):
        if self.args.mode != 'full':
            return
        started = time.monotonic()
        expected = self.db.execute('SELECT count(*) FROM local_expected').fetchone()[0]
        while True:
            status = journal_check.retry_read(lambda: journal_check.progress(self.store.database, self.store.history))
            self.meta['projectionProgress'] = status
            if (status['sourceRows'] == expected and status['sourceHighId'] == expected and
                    status['historyWatermark'] == expected and status['projectedThrough'] == expected and status['cleanedThrough'] == 0):
                break
            if time.monotonic() - started >= self.args.drain_timeout:
                raise RuntimeError('local journal/history projection drain timeout')
            self.sample()
            time.sleep(.1)
        self.meta['projectionDrainSeconds'] = time.monotonic() - started


def quantiles(db, kind):
    count = db.execute('SELECT count(*) FROM latency WHERE kind=?', (kind,)).fetchone()[0]
    result = dict(samples=count, unit='ms', method='exact nearest-rank; batch RPC samples, endToEnd per event')
    for name, fraction in (('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)):
        result[name] = db.execute('SELECT ms FROM latency WHERE kind=? ORDER BY ms LIMIT 1 OFFSET ?',
            (kind, max(0, math.ceil(count * fraction) - 1))).fetchone()[0] if count else None
    return result


def report(output):
    output = output.resolve()
    meta = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    db = sqlite3.connect((output / 'ledger.sqlite').as_uri() + '?mode=ro', uri=True)
    counts = dict(zip(('generated', 'committed', 'claimed', 'acked'), db.execute(
        'SELECT count(*),count(committed),count(claimed),count(acked) FROM events').fetchone()))
    counts['missingCommit'] = counts['generated'] - counts['committed']
    counts['missingAck'] = counts['generated'] - counts['acked']
    counts['duplicateClaims'] = db.execute('SELECT coalesce(sum(max(0,claims-1)),0) FROM events').fetchone()[0]
    counts['anomalies'] = db.execute('SELECT count(*) FROM anomalies').fetchone()[0]
    counts['rateGapEvents'] = max(0, meta['targetEvents'] - counts['generated'])
    duration = meta['actualLoadSeconds']
    passed = (meta['status'] == 'completed' and counts['generated'] > 0 and
              not any(counts[k] for k in ('missingCommit', 'missingAck', 'anomalies')) and meta['pendingAtEnd'] == 0)
    projection = (journal_check.reconcile(output, meta.get('journalIdentity'))
                  if meta.get('coverageMode') == 'full' else dict(executed=False, passed=False))
    states = (state_check.reconcile(output / 'ledger.sqlite', output / 'events.db', output / 'state_reconciliation.csv')
              if meta.get('coverageMode') == 'full' else dict(executed=False, passed=False))
    if meta.get('coverageMode') == 'full':
        passed = passed and projection['passed'] and states['passed']
    rate_ok = counts['generated'] >= meta['targetEvents'] * meta['config']['min_rate_ratio']
    full = (passed and rate_ok and duration >= 86400 and meta['config']['duration'] >= 86400 and
            meta['purpose'] == 'soak' and not meta.get('syntheticReportFixture', False) and
            meta.get('coverageMode') == 'full' and projection['passed'])
    result = dict(meta, counts=counts, reconciliationPassed=passed, rateThresholdPassed=rate_ok,
        journalProjection=projection, stateReconciliation=states, accepted24h=full,
        verdict='PREFLIGHT_PASSED' if meta['status'] == 'preflight_complete' and
        (meta.get('coverageMode') != 'full' or (projection['passed'] and states['passed'])) else
        'PASS_24H_LAB' if full else
        'FAILED_RATE_TARGET' if passed and meta['purpose'] == 'soak' and not rate_ok else
        'COMPLETE_EVENTS_ONLY' if passed and meta['purpose'] == 'soak' and meta.get('coverageMode') != 'full' else
        ('PASS_TOOL_SELFTEST' if passed and meta['purpose'] == 'tool-selftest' else
         'COMPLETE_SHORT_LAB_ONLY' if passed else 'INCOMPLETE_OR_FAILED'),
        latency={kind: quantiles(db, kind) for kind in ('commitConfirm', 'ackConfirm', 'endToEnd')},
        throughput={k + 'PerSecond': counts[k] / (duration + meta.get('drainSeconds', 0) +
                    meta.get('projectionDrainSeconds', 0)) if duration else None
                    for k in ('generated', 'committed', 'acked')},
        measurementNotes=['RPC latency includes IPC and recovery; not internal SQLite transaction time.',
            'Synthetic AckBatch confirmation is not broker PUBACK.',
            'Throughput denominator is load plus delivery/projection drain wall time, including planned downtime.',
            'Resource maxima are sampled maxima; no hard resource limits are enforced.',
            'Multiple actor identities share one sequential scheduler; no parallel writer pressure.',
            '24h is one run/profile; restart downtime is included and separately reported.',
            'No latency/resource acceptance limits specified; these metrics are descriptive.'])
    with (output / 'events.csv').open('w', newline='', encoding='utf-8') as target:
        writer = csv.writer(target)
        cursor = db.execute('SELECT *, CASE WHEN acked IS NULL THEN 1 ELSE 0 END AS missingAck FROM events ORDER BY eventId')
        writer.writerow([c[0] for c in cursor.description])
        writer.writerows(cursor)
    with (output / 'anomalies.csv').open('w', newline='', encoding='utf-8') as target:
        writer = csv.writer(target)
        writer.writerow(['kind', 'eventId', 'detail'])
        writer.writerows(db.execute('SELECT * FROM anomalies'))
    db.close()
    resource_summary = dict(samples=0, rssMaxKiB=None, pssMaxKiB=None, cpuMaxOneCorePercent=None,
                            collectionErrors=0, sampledMaxFileBytes={})
    previous = None
    with (output / 'resources.csv').open('w', newline='', encoding='utf-8') as target:
        writer = csv.writer(target)
        writer.writerow(['elapsedSeconds', 'pid', 'rssKiB', 'pssKiB', 'cpuOneCorePercent', 'freeDiskBytes',
                         'generated', 'committed', 'acked', 'dbBytes', 'walBytes', 'historyDbBytes',
                         'historyWalBytes', 'projection', 'unavailable'])
        with (output / 'resources.jsonl').open(encoding='utf-8') as source:
            for line in source:
                try:
                    sample = json.loads(line)
                except ValueError:
                    resource_summary['collectionErrors'] += 1
                    continue
                resource_summary['samples'] += 1
                for name, size in sample['filesBytes'].items():
                    resource_summary['sampledMaxFileBytes'][name] = max(
                        resource_summary['sampledMaxFileBytes'].get(name, 0), size)
                cpu = None
                counters = sample.get('cpu', {})
                if (previous and sample['pid'] == previous['pid'] and
                    counters.get('startTimeTicks') == previous.get('cpu', {}).get('startTimeTicks') and
                    counters.get('totalTicks') is not None and previous.get('cpu', {}).get('totalTicks') is not None and
                    counters.get('clockTicksPerSecond') and sample['elapsedSeconds'] > previous['elapsedSeconds']):
                    cpu = 100 * (counters['totalTicks'] - previous['cpu']['totalTicks']) / counters['clockTicksPerSecond'] / (
                        sample['elapsedSeconds'] - previous['elapsedSeconds'])
                for key, value in (('rssMaxKiB', sample.get('rssKiB')), ('pssMaxKiB', sample.get('pssKiB')),
                                   ('cpuMaxOneCorePercent', cpu)):
                    if value is not None:
                        resource_summary[key] = max(resource_summary[key] or 0, value)
                writer.writerow([sample['elapsedSeconds'], sample['pid'], sample.get('rssKiB'), sample.get('pssKiB'),
                    cpu, sample['freeDiskBytes'], *sample['counts'], sample['filesBytes'].get('events.db'),
                    sample['filesBytes'].get('events.db-wal'), sample['filesBytes'].get('history.db'),
                    sample['filesBytes'].get('history.db-wal'), json.dumps(sample.get('projection')),
                    json.dumps(sample.get('unavailable', {}))])
                previous = sample
    result['resources'] = resource_summary
    result['reportRendererSha256'] = sha(human_report.__file__)
    result['operations'] = human_report.operation_counts(output / 'operations.jsonl', meta.get('operationTraceVersion') == 1)
    result['counts']['submitted'] = result['operations']['submittedEvents']
    result['countUnits'] = {'generated': 'unique events', 'submitted': 'unique events in initiated logical Append requests',
        'committed': 'unique events with validated commit receipt', 'acked': 'unique events with validated Ack receipt',
        'submittedBatches': 'logical Append requests, excluding retries',
        'committedBatches': 'validated commit batches; latency.commitConfirm.samples',
        'unknownResponses': 'individual unknown-response occurrences', 'unknownRequests': 'logical requests with any unknown result',
        'retryAttempts': 'additional RPC call attempts, not duplicate events',
        'duplicateClaims': 'extra observed event deliveries, not duplicate persistence'}
    dump(output / 'report.json', result)
    human_report.render(output, result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['preflight', 'run', 'report'])
    parser.add_argument('--output', type=Path, required=True, help='new directory for runs; existing directory for report')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--expect-sha256')
    parser.add_argument('--sqlite-library', type=Path)
    parser.add_argument('--profile', choices=['delete-full', 'wal-full'], default='wal-full')
    parser.add_argument('--mode', choices=['events', 'full'], default='full',
                        help='full requires localJournalVersion=1 and historyPath; events is legacy IPC only')
    parser.add_argument('--purpose', choices=['tool-selftest', 'soak'], default='tool-selftest')
    parser.add_argument('--duration', type=float, default=60)
    parser.add_argument('--rate', type=float, default=20)
    parser.add_argument('--producers', type=int, default=2)
    parser.add_argument('--senders', type=int, default=2)
    parser.add_argument('--batch-size', type=int, default=16)
    parser.add_argument('--payload-bytes', type=int, default=128)
    parser.add_argument('--sample-every', type=float, default=5)
    parser.add_argument('--drain-timeout', type=float, default=60)
    parser.add_argument('--rpc-timeout', type=float, default=30, help='seconds per soak RPC attempt; independent of 5s IPC helper default')
    parser.add_argument('--restart-every', type=float, default=0)
    parser.add_argument('--restart-mode', choices=['term', 'kill'], default='term')
    parser.add_argument('--drop-replies', type=int, default=0, help='test injection: discard complete append/ack replies')
    parser.add_argument('--min-free-mb', type=int, default=256)
    parser.add_argument('--min-rate-ratio', type=float, default=.99)
    parser.add_argument('--ephemeral-db', action='store_true', help='selftest only: native /tmp DB, retained after stop')
    args = parser.parse_args(argv)
    if args.command == 'report':
        result = report(args.output)
        print(json.dumps({k: result[k] for k in ('verdict', 'counts', 'accepted24h')}))
        return 0
    if os.name != 'posix' or not args.binary:
        parser.error('run under Linux/WSL with --binary')
    args.binary = args.binary.resolve()
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error('binary must be an existing executable')
    if args.sqlite_library:
        args.sqlite_library = args.sqlite_library.resolve(strict=True)
    if args.expect_sha256 and sha(args.binary) != args.expect_sha256.lower():
        parser.error('binary SHA256 mismatch')
    if args.purpose == 'soak' and not args.expect_sha256:
        parser.error('soak requires --expect-sha256; binary provenance must be explicit')
    if args.purpose == 'soak' and args.ephemeral_db:
        parser.error('ephemeral database is restricted to tool selftests')
    for name in ('duration', 'rate', 'sample_every', 'drain_timeout', 'rpc_timeout'):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            parser.error(name + ' must be finite and positive')
    if not math.isfinite(args.restart_every) or args.restart_every < 0:
        parser.error('invalid restart interval')
    if not (1 <= args.producers <= 64 and 1 <= args.senders <= 64 and 1 <= args.batch_size <= 16 and
            0 <= args.payload_bytes <= 1900 and args.min_free_mb >= 0 and args.drop_replies >= 0 and
            0 < args.min_rate_ratio <= 1):
        parser.error('invalid actor/batch/payload/disk/rate limits')
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    runner = Runner(args)
    runner.run()
    result = report(args.output)
    print(json.dumps({k: result[k] for k in ('verdict', 'counts', 'accepted24h')}))
    if args.command == 'preflight':
        return 0 if result['status'] == 'preflight_complete' else 1
    return 0 if result['reconciliationPassed'] and (args.purpose != 'soak' or result['accepted24h']) else 1


if __name__ == '__main__':
    sys.exit(main())
