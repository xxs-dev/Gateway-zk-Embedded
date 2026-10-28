"""Scoped direct-API review; credentials never enter prompts or evidence."""
import hashlib
import difflib
import json
from pathlib import Path
import sqlite3
import sys
import time
import tomllib
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
FILES = (
    'include/edge_gateway/event_store_sender.hpp', 'src/event_store_sender.cpp',
    'include/edge_gateway/event_store_replay_factory.hpp', 'src/event_store_replay_factory.cpp',
    'include/edge_gateway/event_store_config.hpp',
    'include/edge_gateway/builtin_mqtt_driver_publisher.hpp',
    'src/builtin_mqtt_driver_publisher.cpp',
    'tools/event_store_sender_test.cpp', 'tools/builtin_mqtt_driver_publisher_test.cpp',
)
DESIGN = '''Review EventStore MQTT throughput repair independently. No deployment or live-device
operations. Scope sender/publisher/replay factory/config header and focused tests only;
runtime/projection CPU work belongs to another agent. Current sender sends one message
then waits synchronously for PUBACK and ACKs a successful prefix. Proposed design:
optional synchronous sendBatch callback (legacy send fallback), batch context has one
absolute BOOTTIME network deadline and completion reserve. Callback receives canSend,
onAttempt(index), onConfirmed(index); sender retains exact per-index confirmation even
if callback throws. No references escape callback. Publisher sends QoS1 with sliding
window <=8 (MQTT5 conservatively 1 until Receive Maximum supported), <=16 messages and
<=32768 topic+payload bytes. Packet IDs map to unique input indexes. Fill window, read
validated PUBACK, record matching index, refill only after fresh authorization and
deadline check. Unknown/duplicate/malformed/rejected ACK fails and closes connection;
already confirmed indexes survive. Partial sends are unconfirmed. No hidden retries.
Authority denial is sticky; no new PUBLISH after denial/deadline, drain existing ACKs
within deadline then close if unresolved. Lock acquisition, connection and I/O share
bounded budget. Sender exits Network before callback, builds ACK exact confirmed set
and Release complement. IPC retry replays original request only; never calls network.
Statistics use ACK index mapping, not prefix; confirmed items with failed storage ACK
are not counted delivered. Existing claim identity/lease/scope checks remain.
Review correctness, bounds, deadlines, identity, uncertainty and compatibility.
Finish with VERDICT: ACCEPT or REVISE and concrete blocking findings only.
'''


def main():
    mode, destination = sys.argv[1:3]
    out = Path(destination)
    out.mkdir(parents=True, exist_ok=False)
    hashes = {}
    prompt = DESIGN + '\nReview phase: ' + mode + '\n'
    for name in FILES:
        raw = (ROOT / name).read_bytes()
        hashes[name] = hashlib.sha256(raw).hexdigest()
        if mode in ('baseline', 'freeze'):
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
        else:
            source = raw.decode('utf-8')
            # Publisher has unrelated code; provide complete leased transport and ACK helpers.
            if name == 'src/builtin_mqtt_driver_publisher.cpp':
                lines = source.splitlines()
                starts = ('std::uint16_t validatedPublishAckPacketId(',
                          'void BuiltinMqttDriverPublisher::publishLeasedEvent(',
                          'void BuiltinMqttDriverPublisher::publishLeasedEvents(',
                          'void BuiltinMqttDriverPublisher::sendQos1BatchOnTxConnection(',
                          'void BuiltinMqttDriverPublisher::ensureTxConnected(',
                          'std::uint16_t BuiltinMqttDriverPublisher::nextPacketIdentifier(',
                          'void BuiltinMqttDriverPublisher::closeTx(',
                          'void validateConnAck(')
                chunks = []
                for i, line in enumerate(lines):
                    if line.startswith(starts):
                        end = next(j for j in range(i + 1, len(lines)) if lines[j] == '}')
                        chunks.append('\n'.join(lines[i:end + 1]))
                source = '\n\n'.join(chunks)
            if name == 'tools/builtin_mqtt_driver_publisher_test.cpp':
                lines = source.splitlines()
                starts = ('class TestMqttBroker {', 'class LeasedScriptBroker {',
                          'void testLeasedBatchWindowAndExactTopics(', 'void testLeasedBatchPartial(',
                          'void testLeasedBatchWrapReconnect(', 'void testLeasedBatchInputGates(',
                          'void testLeasedBatchRejectsPersistentSession(')
                chunks = []
                for i, line in enumerate(lines):
                    if line.startswith(starts):
                        ending = '};' if line.startswith('class ') else '}'
                        end = next(j for j in range(i + 1, len(lines)) if lines[j] == ending)
                        chunks.append('\n'.join(lines[i:end + 1]))
                source = '\n\n'.join(chunks)
            if mode == 'session-delta':
                if name not in ('src/builtin_mqtt_driver_publisher.cpp',
                                'include/edge_gateway/builtin_mqtt_driver_publisher.hpp',
                                'tools/builtin_mqtt_driver_publisher_test.cpp'):
                    continue
                if name != 'include/edge_gateway/builtin_mqtt_driver_publisher.hpp':
                    lines = source.splitlines()
                    prefix = ('void BuiltinMqttDriverPublisher::publishLeasedEvents(' if name.startswith('src/')
                              else 'void testLeasedBatchRejectsPersistentSession(')
                    begin = next(i for i, line in enumerate(lines) if line.startswith(prefix))
                    end = next(i for i in range(begin + 1, len(lines)) if lines[i] == '}')
                    source = '\n'.join(lines[begin:end + 1])
            prompt += '\nFILE ' + name + '\n' + source + '\n'
    (out / 'sha256.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    if mode in ('baseline', 'freeze'):
        if mode == 'freeze':
            baseline = out.parent / 'baseline'
            patch = ''
            for name in FILES:
                patch += ''.join(difflib.unified_diff((baseline / name).read_text(encoding='utf-8').splitlines(True),
                    (out / name).read_text(encoding='utf-8').splitlines(True), fromfile='before/' + name, tofile='after/' + name))
            (out / 'scoped.patch').write_text(patch, encoding='utf-8')
            digest = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
            (out / 'freeze-id.txt').write_text(digest + '\n', encoding='utf-8')
            print('Frozen source manifest SHA256: ' + digest)
        print('Snapshot captured: ' + str(out))
        return
    if len(sys.argv) > 3:
        prompt += '\nCODEX EVIDENCE / RECONCILIATION\n' + Path(sys.argv[3]).read_text(encoding='utf-8')
    (out / 'prompt.txt').write_text(prompt, encoding='utf-8')
    with sqlite3.connect('file:C:/Users/12193/.cc-switch/cc-switch.db?mode=ro', uri=True) as db:
        setting = db.execute('SELECT settings_config FROM providers WHERE app_type=? AND is_current=1',
                             ('grokbuild',)).fetchone()[0]
    model = tomllib.loads(json.loads(setting)['config'])['model']['grok-4.6']
    request = urllib.request.Request(model['base_url'].rstrip('/') + '/responses',
        data=json.dumps(dict(model=model['model'], input=prompt, stream=True, max_output_tokens=5000,
                             reasoning=dict(effort='medium'))).encode(),
        headers={'Authorization': 'Bearer ' + model['api_key'], 'Content-Type': 'application/json'})
    response = None
    started = time.monotonic()
    with urllib.request.urlopen(request, timeout=60) as stream:
        for line in stream:
            if time.monotonic() - started > 600:
                raise TimeoutError('review total deadline')
            if line.startswith(b'data: ') and line.strip() != b'data: [DONE]':
                event = json.loads(line[6:])
                if event.get('type') == 'response.completed':
                    response = event['response']
    if not response or response.get('status') != 'completed':
        raise RuntimeError('review incomplete')
    result = '\n'.join(c.get('text', '') for item in response.get('output', []) if item.get('type') == 'message'
                       for c in item.get('content', []) if c.get('type') == 'output_text')
    if not result or not any(v in result for v in ('VERDICT: ACCEPT', 'VERDICT: REVISE')):
        raise RuntimeError('review lacks verdict')
    (out / 'review.txt').write_text(result, encoding='utf-8')
    (out / 'status.json').write_text(json.dumps(dict(status='completed', responseId=response['id'])), encoding='utf-8')
    print(result)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    try:
        main()
    except Exception as error:
        # Do not print HTTP headers, request objects, endpoint, or configuration.
        if len(sys.argv) > 2 and Path(sys.argv[2]).is_dir():
            status_path = Path(sys.argv[2]) / 'status.json'
            completed = status_path.exists() and json.loads(status_path.read_text(encoding='utf-8')).get('status') == 'completed'
            error_path = Path(sys.argv[2]) / 'output-error.json' if completed else status_path
            error_path.write_text(json.dumps(dict(status='output-error' if completed else 'incomplete',
                completedReview=completed, errorType=type(error).__name__)), encoding='utf-8')
        print('Review failed: ' + type(error).__name__)
        sys.exit(1)
