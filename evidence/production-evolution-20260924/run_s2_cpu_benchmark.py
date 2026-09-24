"""Bounded local diagnostic, reusing the pinned native Debug library unchanged."""
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = Path('/var/tmp/ems-production-candidate-20260924-build')
LIBRARY = BUILD / 'libedge_gateway.a'
OUT = HERE / sys.argv[1]
OUT.mkdir(exist_ok=False)
P1 = len(sys.argv) > 2 and sys.argv[2] == 'p1'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


result = {'architecture': platform.machine(), 'productRebuilds': 0, 'remoteConnections': 0,
          'librarySha256': digest(LIBRARY), 'libraryOptimization': 'existing Debug -g (no -O)',
          'armQualification': False, 'steps': {}}
assert result['librarySha256'] == 'd9c02f29e4fa2c830db033ccefc038ca12315c260318e9b9a213f77115226ad2'
result['sources'] = {name: digest(ROOT / name) for name in (
    'ems_cluster_main.cpp', 'src/memory_point_store.cpp', 'src/ems_cluster_points.cpp',
    'include/edge_gateway/ems_cluster_points.hpp',
    'evidence/production-evolution-20260924/s2-cpu-benchmark.cpp',
    'evidence/production-evolution-20260924/run_s2_cpu_benchmark.py')}
result['compiler'] = subprocess.check_output(['c++', '--version'], text=True)


def run(name, args, timeout):
    child = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    path = OUT / (name + '.log')
    with path.open('wb') as stream:
        stream.write(child.stdout)
        stream.flush()
        os.fsync(stream.fileno())
    assert path.read_bytes() == child.stdout
    result['steps'][name] = {'argv': [str(a) for a in args], 'exitCode': child.returncode,
                             'logSha256': digest(path)}
    print(name, child.returncode, flush=True)
    if child.returncode:
        raise RuntimeError(name + ' failed; retained log at ' + str(path))


try:
    with tempfile.TemporaryDirectory(prefix='s2-cpu-probe-', dir=BUILD) as directory:
        binary = Path(directory) / 's2-cpu-probe'
        additions = []
        if P1:
            bridge = Path(directory) / 'bridge.o'
            run('compile-bridge', ['c++', '-std=c++17', '-O0', '-g', '-ffunction-sections', '-fdata-sections',
                '-I' + str(ROOT / 'include'), '-c', str(ROOT / 'src/ems_cluster_points.cpp'), '-o', str(bridge)], 60)
            additions = ['-DBENCHMARK_BATCH_CAPABILITY', str(bridge)]
            result['newBridgeObjectSha256'] = digest(bridge)
        run('compile', ['c++', '-std=c++17', '-O0', '-g', '-ffunction-sections', '-fdata-sections',
            '-I' + str(ROOT / 'include'), str(HERE / 's2-cpu-benchmark.cpp')] + additions + [str(LIBRARY),
            '-Wl,--gc-sections', '-Wl,--wrap=_ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl',
            '-pthread', '-lrt', '-ldl', '-o', str(binary)], 60)
        result['binarySha256'] = digest(binary)
        run('measure', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
            'sh', str(ROOT / 'evidence/ems-integrated-20260924/isolated.sh'), str(binary)], 90)
        rows = [json.loads(line) for line in (OUT / 'measure.log').read_text().splitlines()]
        assert rows[-1]['checksPassed']
        result['callCounts'] = [r for r in rows if 'reads' in r]
        groups = {}
        for row in rows:
            if 'cpuUsPerCall' in row:
                groups.setdefault((row['scenario'], row['operation']), []).append(row)
        result['medians'] = [{'scenario': key[0], 'operation': key[1],
            'cpuUsPerCall': statistics.median(r['cpuUsPerCall'] for r in group),
            'wallUsPerCall': statistics.median(r['wallUsPerCall'] for r in group)}
            for key, group in groups.items()]
        assert digest(LIBRARY) == result['librarySha256']
        result['checksPassed'] = True
finally:
    raw = (json.dumps(result, indent=2) + '\n').encode()
    with (OUT / 'result.json').open('wb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    assert (OUT / 'result.json').read_bytes() == raw
