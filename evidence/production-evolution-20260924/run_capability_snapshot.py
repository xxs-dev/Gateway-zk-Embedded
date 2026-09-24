"""Red/green bridge regression and affected native suites, no full rebuild."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = Path('/var/tmp/ems-production-candidate-20260924-build')
LIBRARY = BUILD / 'libedge_gateway.a'
MODE = sys.argv[1]
assert MODE in ('red', 'green')
OUT = HERE / ('capability-snapshot-' + MODE)
OUT.mkdir(exist_ok=False)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


result = {'mode': MODE, 'librarySha256': digest(LIBRARY), 'steps': {}, 'armQualification': False,
          'bridgeSourceSha256': digest(ROOT / 'src/ems_cluster_points.cpp'),
          'testSourceSha256': digest(ROOT / 'tools/ems_cluster_capability_snapshot_test.cpp')}
assert result['librarySha256'] == 'd9c02f29e4fa2c830db033ccefc038ca12315c260318e9b9a213f77115226ad2'


def run(name, args, expected=0, timeout=60):
    child = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    path = OUT / (name + '.log')
    with path.open('wb') as stream:
        stream.write(child.stdout)
        stream.flush()
        os.fsync(stream.fileno())
    assert path.read_bytes() == child.stdout
    result['steps'][name] = {'argv': [str(a) for a in args], 'exitCode': child.returncode,
                             'expectedExitCode': expected, 'logSha256': digest(path)}
    print(name, child.returncode, flush=True)
    assert child.returncode == expected, str(path)


def isolated(binary):
    return ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
            'sh', str(ROOT / 'evidence/ems-integrated-20260924/isolated.sh'), str(binary)]


try:
    with tempfile.TemporaryDirectory(prefix='capability-' + MODE + '-', dir=BUILD) as directory:
        directory = Path(directory)
        prefix = ['c++', '-std=c++17', '-O0', '-g', '-ffunction-sections', '-fdata-sections',
                  '-I' + str(ROOT / 'include')]
        objects = []
        if MODE == 'green':
            bridge = directory / 'bridge.o'
            run('compile-bridge', prefix + ['-c', str(ROOT / 'src/ems_cluster_points.cpp'), '-o', str(bridge)])
            objects.append(str(bridge))
        tail = objects + [str(LIBRARY), '-Wl,--gc-sections', '-pthread', '-lrt', '-ldl']
        binary = directory / 'capability-test'
        run('compile-regression', prefix + ['-DGATEWAY_CAPABILITY_READ_PROBE',
            str(ROOT / 'tools/ems_cluster_capability_snapshot_test.cpp')] + tail + [
            '-Wl,--wrap=_ZNK12edge_gateway16MemoryPointStore16getLatestByIndexEjl',
            '-Wl,--wrap=_ZNK12edge_gateway16MemoryPointStore18getLatestByIndexesERKSt6vectorIjSaIjEEl',
            '-o', str(binary)])
        run('regression', isolated(binary), expected=1 if MODE == 'red' else 0)
        text = (OUT / 'regression.log').read_text()
        assert ('READ_COUNTS single=16 batch=0' if MODE == 'red' else 'READ_COUNTS single=0 batch=1') in text
        if MODE == 'green':
            old = (HERE / 'capability-snapshot-red/regression.log').read_text()
            cases = [line for line in text.splitlines() if line.startswith('CASE ')]
            assert cases == [line for line in old.splitlines() if line.startswith('CASE ')]
            result['oldNewIdenticalCases'] = len(cases)
            for target in ('ems_cluster_test', 'ems_cluster_output_authority_test', 'ems_cluster_strategy_startup_test'):
                binary = directory / target
                run('compile-' + target, prefix + [str(ROOT / 'tools' / (target + '.cpp'))] + tail + ['-o', str(binary)])
                run(target, isolated(binary))
            run('cxx14', ['c++', '-std=c++14', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                         str(ROOT / 'src/ems_cluster_points.cpp'),
                         str(ROOT / 'tools/ems_cluster_capability_snapshot_test.cpp')])
        result['expectedResultsVerified'] = True
finally:
    raw = (json.dumps(result, indent=2) + '\n').encode()
    with (OUT / 'result.json').open('wb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    assert (OUT / 'result.json').read_bytes() == raw
