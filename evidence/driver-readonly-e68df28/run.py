"""Independent pinned driver review, no writes to the driver's worktree or tests."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = Path('/home/wmzdxs/.cache/edge-driver-readonly-e68df28')
SOURCE = ROOT / 'source'
BUILD = ROOT / 'build'
SOURCE.mkdir(parents=True, exist_ok=True)
GIT = ['git', '--git-dir=/mnt/d/workspace/Embedded/Gateway-zk/.git']
commit = subprocess.check_output(GIT + ['rev-parse', 'e68df28'], text=True).strip()
result = {'commit': commit, 'steps': {}}

def run(name, argv, timeout=600):
    start = time.monotonic()
    with (HERE / (name + '.log')).open('x') as log:
        completed = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    entry = {'argv': argv, 'exitCode': completed.returncode, 'seconds': round(time.monotonic()-start, 3)}
    result['steps'][name] = entry
    print(name, json.dumps(entry), flush=True)
    return completed.returncode

try:
    if run('archive', GIT + ['archive', '-o', str(ROOT/'source.tar'), commit]): raise SystemExit(2)
    if run('extract', ['tar', '-xf', str(ROOT/'source.tar'), '-C', str(SOURCE)]): raise SystemExit(2)
    if run('configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD), '-DCMAKE_BUILD_TYPE=Debug']): raise SystemExit(2)
    if run('library', ['cmake', '--build', str(BUILD), '--target', 'edge_gateway', '--parallel', '2']): raise SystemExit(2)
    library = BUILD / 'libedge_gateway.a'
    result['librarySha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
    binary = BUILD / 'probe'
    if run('compile', ['c++', '-std=c++17', '-O0', '-g', '-I'+str(SOURCE/'include'), '-I'+str(SOURCE/'tools'),
                       str(HERE/'probe.cpp'), str(library), '-pthread', '-ldl', '-lrt',
                       '-Wl,--wrap=send,--wrap=select', '-o', str(binary)]): raise SystemExit(2)
    with tempfile.TemporaryDirectory(prefix='driver-review-', dir=ROOT) as work:
        run('probe', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc', 'sh',
                      str(SOURCE/'evidence/ems-cluster-v2-20260924/isolated_test.sh'), str(binary), work], 60)
    result['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
finally:
    with (HERE/'result.json').open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
