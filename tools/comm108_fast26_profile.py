"""Build a stopped/read-only PCS fast electrical profile; never connects or deploys."""
import argparse
import copy
import json
from pathlib import Path

FAST = set(range(10021, 10047))
SAFETY = set(range(10010, 10015)) | {10266, 10267, 10268}


def build_profile(original):
    result = copy.deepcopy(original)
    if original.get('startupWrites'):
        raise ValueError('Startup writes are not allowed')
    points = [p for m in result['meters'] for p in m['points']]
    if any(p.get('write', {}).get('enable') for p in points):
        raise ValueError('Electrical writes must already be disabled')
    enabled = [p for p in points if p.get('enabled') and p.get('read', {}).get('enable')]
    if len(enabled) != 440 or len({p['index'] for p in enabled}) != 440:
        raise ValueError('Expected the verified 440-point PCS table')
    by_index = {p['index']: p for p in enabled}
    if not (FAST | SAFETY) <= set(by_index):
        raise ValueError('Required electrical/safety point missing')
    for index in FAST:
        p = by_index[index]
        if (p['read']['function'], p['address'], p['read']['length']) != (4, index-9820, 1):
            raise ValueError('Unexpected fast-group register mapping')
    if min(result['collect']['maxBatchRegisters'], result['collect']['maxRequestRegisters']) < 26:
        raise ValueError('Existing read limits cannot fit fast group')
    result['protocol']['transport']['frameIntervalMs'] = 20
    result['collect'].update(maxTasksPerMeterPerCycle=1, realtimeMaxTasksPerMeterPerCycle=1,
        maxBackgroundTasksPerMeterPerCycle=1, realtimeMaxBackgroundTasksPerMeterPerCycle=1,
        backgroundTaskIntervalMs=0)
    for p in enabled:
        if p['read']['dataType'] == 'device_online':
            continue
        interval = p['read'].get('intervalMs', 0) or result['collect']['defaultIntervalMs']
        if p['index'] in FAST:
            p['collectPriority'] = 0
            p['read']['intervalMs'] = 20
        elif p['index'] in SAFETY:
            p['collectPriority'] = 1
            p['read']['intervalMs'] = min(interval, 1000)
        else:
            p['collectPriority'] = 2 if p.get('collectPriority', 0) == 0 else 3
            p['read']['intervalMs'] = max(interval, 5000)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original')
    parser.add_argument('output')
    args = parser.parse_args()
    profile = build_profile(json.loads(Path(args.original).read_text(encoding='utf-8')))
    with Path(args.output).open('x', encoding='utf-8') as stream:
        json.dump(profile, stream, indent=2, ensure_ascii=True)
        stream.write('\n')


if __name__ == '__main__':
    main()
