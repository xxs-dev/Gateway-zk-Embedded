"""Submit only the soak tool source to the repository's configured independent reviewer."""
import json
from pathlib import Path
import sqlite3
import sys
import time
import tomllib
import urllib.request


def main():
    root = Path(__file__).resolve().parent.parent
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=False)
    prompt = ('Independently review this laboratory-only EventStore soak harness. '
              'Review correctness of event reconciliation, exact request replay, 24h gates, interruption, '
              'measurement honesty, bounded memory, and isolated launcher safety. '
              'Existing IPC protocol uses producer/sender sessionId epoch sequence receipts; append commits '
              'return receipt.committed=true. Sender AckBatch returns FINISHED with per-item APPLIED. '
              'Unknown replies must not advance sequences. This harness does not test production MQTT. '
              'No deployment is authorized. Respond concisely with real blocking issues and fixes, '
              'finish with VERDICT: ACCEPT or REVISE for harness only. Source follows.\n')
    for name in ('event_store_soak.py', 'event_store_soak_selftest.py', 'event_store_soak_22_16.sh'):
        prompt += '\nFILE ' + name + '\n' + (root / 'tools' / name).read_text(encoding='utf-8')
    (output / 'prompt.txt').write_text(prompt, encoding='utf-8')
    with sqlite3.connect('file:C:/Users/12193/.cc-switch/cc-switch.db?mode=ro', uri=True) as db:
        setting = db.execute('SELECT settings_config FROM providers WHERE app_type=? AND is_current=1',
                             ('grokbuild',)).fetchone()[0]
    model = tomllib.loads(json.loads(setting)['config'])['model']['grok-4.6']
    request = urllib.request.Request(model['base_url'].rstrip('/') + '/responses',
        data=json.dumps(dict(model=model['model'], input=prompt, stream=True, max_output_tokens=4000,
                             reasoning=dict(effort='medium'))).encode(),
        headers={'Authorization': 'Bearer ' + model['api_key'], 'Content-Type': 'application/json'})
    response = None
    started = time.monotonic()
    with urllib.request.urlopen(request, timeout=60) as stream:
        for line in stream:
            if time.monotonic() - started > 180:
                raise TimeoutError('independent review exceeded 180 second total deadline')
            if line.startswith(b'data: ') and line.strip() != b'data: [DONE]':
                event = json.loads(line[6:])
                if event.get('type') == 'response.completed':
                    response = event['response']
    if not response or response.get('status') != 'completed':
        raise RuntimeError('review not completed')
    result = '\n'.join(c.get('text', '') for item in response.get('output', []) if item.get('type') == 'message'
                       for c in item.get('content', []) if c.get('type') == 'output_text')
    if not result:
        raise RuntimeError('empty review')
    (output / 'review.txt').write_text(result, encoding='utf-8')
    (output / 'status.json').write_text(json.dumps(dict(status='completed', responseId=response['id'])), encoding='utf-8')
    print(result)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        if len(sys.argv) > 1:
            directory = Path(sys.argv[1])
            if directory.is_dir() and not (directory / 'status.json').exists():
                (directory / 'status.json').write_text(json.dumps(dict(status='incomplete',
                    completedReview=False, errorType=type(error).__name__)), encoding='utf-8')
        raise
