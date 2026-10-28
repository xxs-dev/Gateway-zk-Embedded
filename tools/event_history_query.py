#!/usr/bin/env python3
"""Read-only merged alarm history: committed journal first, legacy projection second."""
import argparse
from contextlib import closing
import csv
import json
from pathlib import Path
import sqlite3
import sys
import time

COLUMNS = ('event_id', 'point_index', 'ts', 'alarm_type', 'active', 'threshold',
           'value', 'quality', 'stale', 'persist_value', 'gateway_code', 'device_code', 'point_code')

def query(source, history, begin, end, limit=1000, machine=None, meter=None):
    source = Path(source).resolve(strict=True)
    history = Path(history).resolve(strict=True)
    if source.samefile(history):
        raise ValueError('journal and history must be distinct files')
    if begin < 0 or end < begin or not 1 <= limit <= 10000:
        raise ValueError('invalid range/limit')
    with closing(sqlite3.connect(source.as_uri() + '?mode=ro', uri=True, timeout=.05)) as db:
        db.row_factory = sqlite3.Row
        deadline = time.monotonic() + 3.0
        db.set_progress_handler(lambda: int(time.monotonic() > deadline), 1000)
        db.execute('PRAGMA query_only=ON')
        db.execute('ATTACH DATABASE ? AS history', (history.as_uri() + '?mode=ro',))
        db.execute('BEGIN')
        identity = db.execute('SELECT store_id FROM event_store_identity WHERE id=1').fetchone()
        generation = db.execute('SELECT journal_generation FROM event_journal_meta WHERE id=1').fetchone()
        projection = db.execute('SELECT store_id,journal_generation FROM history.alarm_projection_meta WHERE id=1').fetchone()
        if not identity or not generation or not projection or tuple(projection) != (identity[0], generation[0]):
            raise ValueError('source/projection identity mismatch')
        conditions = ['ts>=?', 'ts<=?']
        parameters = [begin, end]
        if machine is not None:
            conditions.append('gateway_code=?'); parameters.append(machine)
        if meter is not None:
            conditions.append('device_code=?'); parameters.append(meter)
        columns = ','.join(COLUMNS)
        where = ' AND '.join(conditions)
        journal_where = ' AND '.join('j.' + condition for condition in conditions)
        different = ' OR '.join('h.' + c + ' IS NOT j.' + c for c in COLUMNS)
        invalid = db.execute("SELECT 1 FROM event_local_journal j WHERE j.kind='alarm' AND " + journal_where +
            " AND (j.event_id IS NULL OR j.event_id='') LIMIT 1", parameters).fetchone()
        if invalid:
            raise ValueError('committed journal has an invalid event identity')
        # Validate the entire requested journal range, independently of the page size.
        conflict = db.execute("SELECT j.event_id FROM event_local_journal j WHERE j.kind='alarm' AND " +
            journal_where + ' AND (EXISTS (SELECT 1 FROM history.alarm_events h WHERE h.event_id=j.event_id '
            'AND (' + different + ')) OR (SELECT count(*) FROM history.alarm_events h WHERE h.event_id=j.event_id)>1) LIMIT 1',
            parameters).fetchone()
        if conflict:
            raise ValueError('history content conflicts with committed journal: ' + conflict[0])
        sql = ('WITH merged AS (SELECT ' + columns + ", 'journal' AS origin FROM event_local_journal WHERE kind='alarm' AND " + where +
               ' UNION ALL SELECT ' + ','.join('h.' + c for c in COLUMNS) + ", 'legacy-history' AS origin "
               'FROM history.alarm_events h WHERE ' + ' AND '.join('h.' + condition for condition in conditions) +
               ' AND (h.event_id IS NULL OR h.event_id=\'\' OR NOT EXISTS '
               '(SELECT 1 FROM event_local_journal j WHERE j.event_id=h.event_id))) '
               'SELECT * FROM merged ORDER BY ts,event_id LIMIT ?')
        rows = [dict(row) for row in db.execute(sql, parameters + parameters + [limit + 1])]
        truncated = len(rows) > limit
        rows = rows[:limit]
        return {'storeId': identity[0], 'journalGeneration': generation[0],
                'beginMs': begin, 'endMs': end, 'count': len(rows), 'truncated': truncated, 'alarms': rows}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--history', required=True)
    parser.add_argument('--begin-ms', type=int, required=True)
    parser.add_argument('--end-ms', type=int, required=True)
    parser.add_argument('--limit', type=int, default=1000)
    parser.add_argument('--machine')
    parser.add_argument('--meter')
    parser.add_argument('--format', choices=['json', 'csv'], default='json')
    args = parser.parse_args()
    result = query(args.source, args.history, args.begin_ms, args.end_ms, args.limit, args.machine, args.meter)
    if args.format == 'json':
        print(json.dumps(result, ensure_ascii=False, allow_nan=False, indent=2))
    else:
        writer = csv.DictWriter(sys.stdout, fieldnames=COLUMNS + ('origin',))
        writer.writeheader(); writer.writerows(result['alarms'])
        if result['truncated']:
            print('Result truncated; narrow the time range or raise --limit (maximum 10000).', file=sys.stderr)
    return 0

if __name__ == '__main__':
    sys.exit(main())
