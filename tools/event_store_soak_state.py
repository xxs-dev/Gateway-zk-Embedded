"""Expected baseline CAS state and read-only persisted-state reconciliation."""
import contextlib
import csv
import json
from pathlib import Path
import sqlite3

from event_store_soak_journal import readonly, busy_error


FIELDS = dict(stateKey='state_key', eventType='event_type', index='point_index', alarmType='alarm_type',
              active='active', value='value', quality='quality', sourceTs='source_ts', lifecycle='lifecycle')


def state(run_id, producer, sequence, local):
    return dict(stateKey=run_id + ':baseline:p' + str(producer), eventType=local['kind'], index=local['index'],
                alarmType=local['alarmType'], active=local['active'], value=local['value'], quality=local['quality'],
                sourceTs=local['ts'], lifecycle='batch-' + str(sequence) + ':' + local['eventId'],
                expectedVersion=str(sequence - 1))


def reconcile(ledger_path, source_path, csv_path=None):
    result = dict(executed=True, passed=False, expectedStates=0, confirmedUpdates=0, pendingUpdates=0,
                  missingStates=0, extraStates=0, mismatchStates=0, sequenceErrors=0, errors=[])
    try:
        with contextlib.ExitStack() as stack:
            ledger = stack.enter_context(contextlib.closing(readonly(ledger_path)))
            source = stack.enter_context(contextlib.closing(readonly(source_path)))
            ledger.execute('BEGIN')
            source.execute('BEGIN')
            writer = None
            if csv_path:
                writer = csv.writer(stack.enter_context(Path(csv_path).open('w', newline='', encoding='utf-8')))
                writer.writerow(['stateKey', 'producerId', 'expectedVersion', 'actualOwner', 'actualVersion', 'status', 'mismatchedFields'])
            expected_keys = set()
            for actor_row in ledger.execute('SELECT DISTINCT producerId FROM state_updates ORDER BY producerId'):
                actor = actor_row['producerId']
                last, sequence = None, 0
                for row in ledger.execute('SELECT * FROM state_updates WHERE producerId=? ORDER BY sequence', (actor,)):
                    wanted = json.loads(row['expected'])
                    if row['sequence'] != sequence + 1 or int(wanted['expectedVersion']) != sequence:
                        result['sequenceErrors'] += 1
                    if row['confirmed']:
                        result['confirmedUpdates'] += 1
                        sequence = row['sequence']
                        last = wanted
                    else:
                        result['pendingUpdates'] += 1
                if last is None:
                    continue
                key = last['stateKey']
                expected_keys.add(key)
                result['expectedStates'] += 1
                actual = source.execute('SELECT s.*,v.producer_id,v.version FROM mqtt_event_state s LEFT JOIN '
                                        'event_store_state_version v ON v.state_key=s.state_key WHERE s.state_key=?', (key,)).fetchone()
                mismatches = []
                if actual is None:
                    result['missingStates'] += 1
                else:
                    if actual['producer_id'] != actor:
                        mismatches.append('owner')
                    if actual['version'] != sequence:
                        mismatches.append('version')
                    for field, column in FIELDS.items():
                        value = int(last[field]) if field in ('index', 'quality', 'sourceTs') else last[field]
                        if value != actual[column]:
                            mismatches.append(field)
                    if mismatches:
                        result['mismatchStates'] += 1
                if writer:
                    writer.writerow([key, actor, sequence, actual['producer_id'] if actual else '',
                                     actual['version'] if actual else '', 'MISSING' if actual is None else 'MISMATCH' if mismatches else 'MATCH', ','.join(mismatches)])
            actual_keys = {r[0] for r in source.execute('SELECT state_key FROM mqtt_event_state UNION SELECT state_key FROM event_store_state_version')}
            result['extraStates'] = len(actual_keys - expected_keys)
            if writer:
                for key in sorted(actual_keys - expected_keys):
                    writer.writerow([key, '', '', '', '', 'EXTRA', ''])
            batches = ledger.execute("SELECT count(*) FROM latency WHERE kind='commitConfirm'").fetchone()[0]
            if batches != result['confirmedUpdates']:
                result['errors'].append('confirmed state updates differ from confirmed Append batches')
            result['passed'] = not result['errors'] and not any(result[k] for k in ('pendingUpdates', 'missingStates', 'extraStates', 'mismatchStates', 'sequenceErrors'))
    except (OSError, sqlite3.Error, ValueError, KeyError, TypeError) as exc:
        result['errors'].append(type(exc).__name__ + ': ' + str(exc))
        if busy_error(exc):
            result['retryableReadError'] = True
    return result
