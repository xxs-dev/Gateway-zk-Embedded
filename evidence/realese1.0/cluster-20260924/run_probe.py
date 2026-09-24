"""Execute the authorized bounded lease-window probe, not a release build."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from run_local import HERE, SOURCE, command

build = Path('/home/wmzdxs/.cache/edge-cluster-fixed-7e392ce-20260924')
steps = []
steps.append(command('probe-configure', ['cmake', '-S', str(SOURCE), '-B', str(build),
                     '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON']))
if steps[-1]['exitCode'] == 0:
    steps.append(command('probe-library-build', ['cmake', '--build', str(build),
                         '--target', 'edge_gateway', '--parallel', '4']))
binary = build / 'lease_probe'
if steps[-1]['exitCode'] == 0:
    steps.append(command('probe-compile', ['c++', '-std=c++17', '-O0', '-g',
                         '-I' + str(SOURCE / 'include'), str(HERE / 'lease_probe.cpp'),
                         str(build / 'libedge_gateway.a'), '-pthread', '-ldl',
                         '-o', str(binary)]))
if steps[-1]['exitCode'] == 0:
    with tempfile.TemporaryDirectory(prefix='lease-probe-') as work:
        steps.append(command('probe-trace', ['unshare', '--mount', '--net', '--ipc',
                             '--pid', '--fork', '--mount-proc', 'sh',
                             str(HERE / 'isolated_test.sh'), str(binary), work], timeout=30))

result = {'sourceCommit': '7e392cec6218244b0a230a5f0d58c96888fd51de', 'steps': steps,
          'stepMs': 25, 'partitionLeaderLeaseMs': 400,
          'electionTimeoutMinMs': 500, 'electionTimeoutMaxMs': 800,
          'librarySha256': hashlib.sha256((build / 'libedge_gateway.a').read_bytes()).hexdigest(),
          'compiler': subprocess.check_output(['c++', '--version'], text=True),
          'buildCacheSource': [x for x in (build / 'CMakeCache.txt').read_text().splitlines()
                               if x.startswith('CMAKE_HOME_DIRECTORY:')]}
if binary.exists():
    result['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
if steps[-1]['label'] == 'probe-trace' and steps[-1]['exitCode'] == 0:
    rows = [json.loads(x) for x in (HERE / 'probe-trace.log').read_text().splitlines()]
    summaries = []
    for scenario in dict.fromkeys(x['scenario'] for x in rows):
        selected = [x for x in rows if x['scenario'] == scenario]
        summary = {'scenario': scenario}
        for key, predicate in (
            ('roleOverlap', lambda n: n['role'] == 'leader'),
            ('quorumLeaderOverlap', lambda n: n['role'] == 'leader' and n['quorumValid']),
            ('controlLeaderOverlap', lambda n: n['role'] == 'leader' and n['controlActive']),
            ('dispatchLeaderOverlap', lambda n: n['role'] == 'leader' and n['dispatchValid']),
            ('combinedLeaderOverlap', lambda n: n['role'] == 'leader' and n['quorumValid']
             and n['controlActive'] and n['dispatchValid'])):
            hits = [x['elapsedMs'] for x in selected if sum(predicate(n) for n in x['nodes']) > 1]
            spans = []
            for value in hits:
                if not spans or value != spans[-1][-1] + 25:
                    spans.append([value])
                else:
                    spans[-1].append(value)
            summary[key] = {'firstMs': hits[0] if hits else None,
                            'lastMs': hits[-1] if hits else None,
                            'samples': len(hits),
                            'longestSampleIntervalMs': max((s[-1] - s[0] for s in spans), default=0),
                            'longestUntilNextTickMs': max((s[-1] - s[0] + 25 for s in spans), default=0)}
        summaries.append(summary)
    result['scenarios'] = summaries
(HERE / 'probe-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result.get('scenarios', result), indent=2), flush=True)
raise SystemExit(int(any(s['exitCode'] for s in steps)))
