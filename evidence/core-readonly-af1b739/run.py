"""Read-only pinned candidate review; all generated files stay in private output paths."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
GIT = ['git', '--git-dir=/mnt/d/workspace/Embedded/Gateway-zk/.git']
ROOT = Path('/home/wmzdxs/.cache/edge-core-readonly-af1b739')
SOURCE = ROOT / 'source'
BUILD = ROOT / 'build'
SOURCE.mkdir(parents=True, exist_ok=True)
commit = subprocess.check_output(GIT + ['rev-parse', 'af1b739'], text=True).strip()
result = {'commit': commit, 'steps': {}, 'sourceMode': 'git archive pinned commit; candidate read-only'}

def run(name, argv, timeout=600):
    started = time.monotonic()
    with (HERE / (name + '.log')).open('x') as log:
        completed = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    entry = {'argv': argv, 'exitCode': completed.returncode, 'seconds': round(time.monotonic()-started, 3)}
    result['steps'][name] = entry
    print(name, json.dumps(entry), flush=True)
    return completed.returncode

try:
    if run('archive', GIT + ['archive', '-o', str(ROOT/'source.tar'), commit]):
        raise SystemExit(2)
    if run('extract', ['tar', '-xf', str(ROOT/'source.tar'), '-C', str(SOURCE)]):
        raise SystemExit(2)
    if run('configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD), '-DCMAKE_BUILD_TYPE=Debug']):
        raise SystemExit(2)
    if run('library', ['cmake', '--build', str(BUILD), '--target', 'edge_gateway', '--parallel', '2']):
        raise SystemExit(2)
    library = BUILD / 'libedge_gateway.a'
    result['librarySha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
    for name in ('cluster_write_authorization_test', 'ems_cluster_output_authority_test',
                 'ems_cluster_strategy_startup_test', 'provisioned_startup_probe'):
        source = HERE / (name+'.cpp') if name == 'provisioned_startup_probe' else SOURCE / 'tools' / (name+'.cpp')
        binary = BUILD / name
        if run('compile-'+name, ['c++', '-std=c++17', '-O0', '-g', '-I'+str(SOURCE/'include'),
                                '-I'+str(SOURCE/'tools'), str(source), str(library), '-pthread', '-ldl', '-o', str(binary)]):
            continue
        with tempfile.TemporaryDirectory(prefix='core-review-', dir=ROOT) as work:
            run(name, ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc', 'sh',
                       str(SOURCE/'evidence/ems-cluster-v2-20260924/isolated_test.sh'), str(binary), work], 90)
        result['steps'][name]['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
finally:
    with (HERE/'result.json').open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
