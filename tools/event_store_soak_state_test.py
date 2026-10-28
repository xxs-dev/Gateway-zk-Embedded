#!/usr/bin/env python3
"""Real full evidence verification plus labelled synthetic state-corruption checks."""
import argparse
import json
from pathlib import Path
import sqlite3
import sys

sys.dont_write_bytecode = True
import event_store_soak_journal as journal
import event_store_soak_state as state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    real = json.loads((args.source / 'report.json').read_text())
    assert real['stateCasVersion'] == 1 and not real.get('syntheticReportFixture')
    assert real['stateReconciliation']['passed'] and real['journalProjection']['passed']
    baseline = state.reconcile(args.source / 'ledger.sqlite', args.source / 'events.db')
    assert baseline['passed'] and baseline['confirmedUpdates'] > 0
    cases = [dict(name='existing real full baseline', passed=True, synthetic=False, detail=baseline)]
    key = '(SELECT min(state_key) FROM mqtt_event_state)'
    mutations = {
        'owner': "UPDATE event_store_state_version SET producer_id='wrong' WHERE state_key=" + key,
        'version': 'UPDATE event_store_state_version SET version=version+1 WHERE state_key=' + key,
        'value': 'UPDATE mqtt_event_state SET value=value+1 WHERE state_key=' + key,
        'lifecycle': "UPDATE mqtt_event_state SET lifecycle='wrong' WHERE state_key=" + key,
        'sourceTs': 'UPDATE mqtt_event_state SET source_ts=source_ts+1 WHERE state_key=' + key,
        'missingState': 'DELETE FROM mqtt_event_state WHERE state_key=' + key,
        'extraState': "INSERT INTO event_store_state_version VALUES('extra','p0',1)",
    }
    for name, sql in mutations.items():
        out = args.output / name
        out.mkdir()
        journal.backup(args.source / 'ledger.sqlite', out / 'ledger.sqlite')
        journal.backup(args.source / 'events.db', out / 'events.db')
        with sqlite3.connect(str(out / 'events.db')) as db:
            db.execute(sql)
        result = state.reconcile(out / 'ledger.sqlite', out / 'events.db', out / 'state_reconciliation.csv')
        assert not result['passed'], name
        cases.append(dict(name=name + ' rejected', passed=True, synthetic=True, detail=result))
    result = dict(passed=True, accepted24h=False, cases=cases, realRuntimeRerun=False,
                  realSource=str(args.source), note='baseline is earlier real full evidence; mutations only test the reconciler')
    (args.output / 'selftest.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
