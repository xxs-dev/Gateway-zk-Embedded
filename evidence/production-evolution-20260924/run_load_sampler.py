"""Compare unchanged health semantics and actual sampler cost, local only."""
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = Path('/var/tmp/ems-production-candidate-20260924-build')
MODE = sys.argv[1]
assert MODE in ('old', 'new')
OUT = HERE / ('load-sampler-' + MODE)
OUT.mkdir(exist_ok=False)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


result = {'mode': MODE, 'steps': {}, 'armQualification': False,
          'mainSourceSha256': digest(ROOT / 'ems_cluster_main.cpp'),
          'testSourceSha256': digest(ROOT / 'tools/ems_cluster_load_sampler_test.cpp')}


def run(name, args):
    child = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    path = OUT / (name + '.log')
    with path.open('wb') as stream:
        stream.write(child.stdout)
        stream.flush()
        os.fsync(stream.fileno())
    assert path.read_bytes() == child.stdout
    result['steps'][name] = {'argv': [str(a) for a in args], 'exitCode': child.returncode,
                             'logSha256': digest(path)}
    print(name, child.returncode, flush=True)
    assert child.returncode == 0, str(path)


try:
    with tempfile.TemporaryDirectory(prefix='load-sampler-' + MODE + '-', dir=BUILD) as directory:
        binary = Path(directory) / 'load-sampler-test'
        run('compile', ['c++', '-std=c++17', '-O0', '-g', '-ffunction-sections', '-fdata-sections',
            '-I' + str(ROOT / 'include'), str(ROOT / 'tools/ems_cluster_load_sampler_test.cpp'),
            '-Wl,--gc-sections', '-Wl,--wrap=_ZNSt6chrono3_V212system_clock3nowEv',
            '-pthread', '-o', str(binary)])
        run('test', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
            'sh', str(ROOT / 'evidence/ems-integrated-20260924/isolated.sh'), str(binary)])
        lines = (OUT / 'test.log').read_text().splitlines()
        rows = [json.loads(line[6:]) for line in lines if line.startswith('BENCH ')]
        result['medians'] = {str(fresh): statistics.median(
            row['cpuUsPerCall'] for row in rows if row['fresh'] == fresh) for fresh in (0, 1)}
        if MODE == 'new':
            old_lines = (HERE / 'load-sampler-old/test.log').read_text().splitlines()
            cases = [line for line in lines if line.startswith('CASE ')]
            assert cases == [line for line in old_lines if line.startswith('CASE ')]
            result['oldNewIdenticalCases'] = len(cases)
            run('cxx14', ['c++', '-std=c++14', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                str(ROOT / 'ems_cluster_main.cpp'), str(ROOT / 'tools/ems_cluster_load_sampler_test.cpp')])
        result['checksPassed'] = True
finally:
    raw = (json.dumps(result, indent=2) + '\n').encode()
    with (OUT / 'result.json').open('wb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    assert (OUT / 'result.json').read_bytes() == raw
