"""Read-only full-mode reconciliation. Never creates or repairs server databases."""
import contextlib
import csv
import json
from pathlib import Path
import sqlite3
import time


# The runtime localEvents decoder and EventHistoryProjection::bindAlarm contract.
FIELDS = {'eventId': 'event_id', 'index': 'point_index', 'ts': 'ts', 'alarmType': 'alarm_type',
          'active': 'active', 'threshold': 'threshold', 'value': 'value', 'quality': 'quality',
          'stale': 'stale', 'persistValue': 'persist_value', 'machineCode': 'gateway_code',
          'meterCode': 'device_code', 'pointCode': 'point_code'}
JOURNAL_FIELDS = dict(FIELDS, kind='kind', stateVersion='state_version', configGeneration='config_generation')


def local_event(event, ordinal, point, generation):
    alarm = event['eventType'] == 'alarm'
    return dict(kind=event['eventType'], eventId=event['eventId'], index=str(point),
        machineCode='soak-gateway', meterCode='soak-device', pointCode='point-' + str(point),
        alarmType='upper-limit' if alarm else '', active=ordinal % 2 == 1,
        threshold=50.0, value=float(ordinal % 1000), quality='0', ts=event['eventTs'],
        stale=False, persistValue=str(ordinal % 1000), stateVersion=str(ordinal), configGeneration=generation)


def readonly(path):
    path = Path(path).resolve(strict=True)
    db = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=1)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA query_only=ON')
    return db


def busy_error(error):
    code = getattr(error, 'sqlite_errorcode', None)
    return isinstance(error, sqlite3.OperationalError) and (
        (code is not None and code & 255 in (5, 6)) or
        str(error).lower() in ('database is locked', 'database table is locked', 'database schema is locked'))


def retry_read(operation, attempts=4):
    """Retry only transient BUSY/LOCKED observations; each connection waits at most 1s."""
    for attempt in range(attempts):
        try:
            result = operation()
            if not isinstance(result, dict) or not result.get('retryableReadError') or attempt + 1 == attempts:
                return result
        except sqlite3.OperationalError as error:
            if not busy_error(error) or attempt + 1 == attempts:
                raise
        time.sleep(.1)


def backup(source, destination):
    """Capture committed DB plus WAL through SQLite, never checkpoint the source."""
    destination = Path(destination)
    if destination.exists():
        raise FileExistsError(destination)
    deadline = time.monotonic() + 60
    def progress(status, remaining, total):
        if time.monotonic() >= deadline:
            raise TimeoutError('SQLite evidence backup timeout')
    with contextlib.closing(readonly(source)) as src:
        with contextlib.closing(sqlite3.connect(str(destination))) as dst:
            src.backup(dst, pages=256, progress=progress, sleep=.05)
            if dst.execute('PRAGMA integrity_check').fetchone() != ('ok',):
                raise ValueError('SQLite evidence backup integrity failure')


def identity(source):
    with contextlib.closing(readonly(source)) as db:
        row = db.execute('SELECT store_id,config_generation,schema_version FROM event_store_identity WHERE id=1').fetchone()
        generation = db.execute('SELECT schema_version,journal_generation FROM event_journal_meta WHERE id=1').fetchone()
        if row is None or generation is None or row['schema_version'] != 1 or generation['schema_version'] != 1:
            raise ValueError('missing or unsupported source journal identity')
        return dict(storeId=row['store_id'], configGeneration=row['config_generation'],
                    journalGeneration=generation['journal_generation'])


def progress(source, history):
    with contextlib.closing(readonly(source)) as db:
        count, high = db.execute('SELECT count(*),coalesce(max(id),0) FROM event_local_journal').fetchone()
        cursor = db.execute('SELECT projected_through,cleaned_through FROM event_history_projection_cursor WHERE id=1').fetchone()
    with contextlib.closing(readonly(history)) as db:
        watermark = db.execute('SELECT last_contiguous_journal_id FROM alarm_projection_meta WHERE id=1').fetchone()
    if cursor is None or watermark is None:
        raise ValueError('projection metadata unavailable')
    return dict(sourceRows=count, sourceHighId=high, projectedThrough=cursor[0], cleanedThrough=cursor[1],
                historyWatermark=watermark[0])


def differences(expected, row, fields):
    result = []
    for wire, column in fields.items():
        value = expected[wire]
        if wire in ('index', 'ts', 'quality', 'stateVersion'):
            value = int(value)
        if row[column] != value:
            result.append(wire)
    return result


def reconcile(output, expected_identity):
    """Stream expected and actual IDs; export per-ID evidence, including extras."""
    output = Path(output)
    result = dict(executed=True, passed=False, observerSqliteVersion=sqlite3.sqlite_version,
                  expectedIdentity=expected_identity, errors=[], expectedLocal=0, expectedAlarms=0,
                  missingJournal=0, journalMismatch=0, extraJournal=0, missingHistory=0,
                  historyMismatch=0, extraHistory=0, journalIdGaps=0)
    try:
        actual_identity = identity(output / 'events.db')
        result['actualIdentity'] = actual_identity
        if actual_identity != expected_identity:
            result['errors'].append('source identity/generation differs from startup identity')
        with contextlib.ExitStack() as stack:
            ledger = stack.enter_context(contextlib.closing(readonly(output / 'ledger.sqlite')))
            source = stack.enter_context(contextlib.closing(readonly(output / 'events.db')))
            history = stack.enter_context(contextlib.closing(readonly(output / 'history.db')))
            history_meta = history.execute('SELECT * FROM alarm_projection_meta WHERE id=1').fetchone()
            cursor = source.execute('SELECT * FROM event_history_projection_cursor WHERE id=1').fetchone()
            if history_meta is None or cursor is None:
                raise ValueError('projection identity/cursor missing')
            result['historyIdentity'] = dict(history_meta)
            result['sourceCursor'] = dict(cursor)
            if (history_meta['projection_id'] != 'alarm-history-v1' or
                    history_meta['store_id'] != actual_identity['storeId'] or
                    history_meta['journal_generation'] != actual_identity['journalGeneration'] or
                    cursor['projection_id'] != 'alarm-history-v1'):
                result['errors'].append('history/source generation or projection identity mismatch')
            target = stack.enter_context((output / 'journal_reconciliation.csv').open('w', newline='', encoding='utf-8'))
            writer = csv.writer(target)
            writer.writerow(['eventId', 'kind', 'stateVersion', 'journalId', 'journalStatus', 'historyStatus', 'mismatchedFields'])
            for item in ledger.execute('SELECT eventId,expected FROM local_expected ORDER BY ordinal'):
                expected = json.loads(item['expected'])
                result['expectedLocal'] += 1
                alarm = expected['kind'] == 'alarm'
                result['expectedAlarms'] += int(alarm)
                rows = source.execute('SELECT * FROM event_local_journal WHERE event_id=?', (item['eventId'],)).fetchmany(2)
                mismatch = []
                if not rows:
                    result['missingJournal'] += 1
                    journal_status = 'MISSING'
                else:
                    mismatch = differences(expected, rows[0], JOURNAL_FIELDS)
                    if len(rows) != 1:
                        mismatch.append('duplicateEventId')
                    if mismatch:
                        result['journalMismatch'] += 1
                    journal_status = 'MISMATCH' if mismatch else 'MATCH'
                saved = history.execute('SELECT * FROM alarm_events WHERE event_id=?', (item['eventId'],)).fetchmany(2)
                history_status = 'NOT_PROJECTED_BY_DESIGN'
                if alarm:
                    if not saved:
                        result['missingHistory'] += 1
                        history_status = 'MISSING'
                    else:
                        changes = differences(expected, saved[0], FIELDS)
                        if len(saved) != 1:
                            changes.append('duplicateEventId')
                        if changes:
                            result['historyMismatch'] += 1
                        mismatch.extend('history.' + field for field in changes)
                        history_status = 'MISMATCH' if changes else 'MATCH'
                elif saved:
                    history_status = 'UNEXPECTED_CHANGE_PROJECTION'
                writer.writerow([item['eventId'], expected['kind'], expected['stateVersion'],
                    rows[0]['id'] if rows else None, journal_status, history_status, ','.join(mismatch)])
            last = 0
            for row in source.execute('SELECT id,event_id FROM event_local_journal ORDER BY id'):
                if row['id'] != last + 1:
                    result['journalIdGaps'] += 1
                last = row['id']
                if ledger.execute('SELECT 1 FROM local_expected WHERE eventId=?', (row['event_id'],)).fetchone() is None:
                    result['extraJournal'] += 1
                    writer.writerow([row['event_id'], '', '', row['id'], 'EXTRA', '', ''])
            for row in history.execute('SELECT event_id FROM alarm_events'):
                expected = ledger.execute('SELECT expected FROM local_expected WHERE eventId=?', (row['event_id'],)).fetchone()
                if expected is None or json.loads(expected[0])['kind'] != 'alarm':
                    result['extraHistory'] += 1
                    writer.writerow([row['event_id'], '', '', '', '', 'EXTRA', ''])
            result['sourceHighId'] = last
            result['sourceRows'] = source.execute('SELECT count(*) FROM event_local_journal').fetchone()[0]
            result['historyRows'] = history.execute('SELECT count(*) FROM alarm_events').fetchone()[0]
            result['duplicateJournalRows'] = source.execute(
                'SELECT count(event_id)-count(DISTINCT event_id) FROM event_local_journal').fetchone()[0]
            result['duplicateHistoryRows'] = history.execute(
                'SELECT count(event_id)-count(DISTINCT event_id) FROM alarm_events').fetchone()[0]
            result['historyWatermark'] = history_meta['last_contiguous_journal_id']
            result['generatedEvents'] = ledger.execute('SELECT count(*) FROM events').fetchone()[0]
            if result['generatedEvents'] != result['expectedLocal']:
                result['errors'].append('generated events and generated localEvents differ')
            if (result['sourceRows'] != result['expectedLocal'] or last != result['expectedLocal'] or
                    result['historyRows'] != result['expectedAlarms']):
                result['errors'].append('source/history row counts or contiguous high ID differ from generated events')
            if (history_meta['last_contiguous_journal_id'] != last or cursor['projected_through'] != last or
                    cursor['cleaned_through'] != 0):
                result['errors'].append('watermark/cursor incomplete, or source rows cleaned before full reconciliation')
            result['passed'] = not result['errors'] and not any(result[k] for k in
                ('missingJournal', 'journalMismatch', 'extraJournal', 'missingHistory', 'historyMismatch',
                 'extraHistory', 'journalIdGaps'))
    except (OSError, sqlite3.Error, ValueError, KeyError, IndexError, TypeError) as error:
        result['errors'].append(type(error).__name__ + ': ' + str(error))
    return result
