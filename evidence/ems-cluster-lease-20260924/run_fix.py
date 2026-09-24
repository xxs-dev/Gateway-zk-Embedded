"""Bounded native test build/run; never builds release programs or contacts devices."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
BUILD = Path('/home/wmzdxs/.cache/edge-cluster-lease-worker-20260924')
BASELINE = Path('/home/wmzdxs/.cache/edge-cluster-fixed-7e392ce-20260924/libedge_gateway.a')
ISOLATE = SOURCE.parent / 'cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh'


def run(label, args, timeout=300):
    start = time.monotonic()
    with (HERE / (label + '.log')).open('w') as log:
        result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False)
    record = {'argv': args, 'exitCode': result.returncode,
              'seconds': round(time.monotonic() - start, 3)}
    print(label, json.dumps(record), flush=True)
    return record


label = sys.argv[1]
BUILD.mkdir(parents=True, exist_ok=True)
results = {'label': label, 'steps': {}, 'sourceSha256': {},
           'compiler': subprocess.check_output(['c++', '--version'], text=True)}
for relative in ('src/ems_cluster.cpp', 'src/ems_cluster_dispatch.cpp',
                 'include/edge_gateway/ems_cluster.hpp', 'tools/ems_cluster_test.cpp'):
    results['sourceSha256'][relative] = hashlib.sha256((SOURCE / relative).read_bytes()).hexdigest()
library = BASELINE
if not label.startswith('red-'):
    for name, args in (
        ('configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD), '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON']),
        ('library', ['cmake', '--build', str(BUILD), '--target', 'edge_gateway', '--parallel', '4'])):
        results['steps'][name] = run(label + '-' + name, args)
        if results['steps'][name]['exitCode']:
            raise SystemExit(2)
    library = BUILD / 'libedge_gateway.a'
results['librarySha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
tests = ('ems_cluster_test',) if label.startswith('red-') else ('ems_cluster_test', 'graph_ems_cluster_dispatch_test')
for name in tests:
    binary = BUILD / (name + '-' + label)
    compile_result = run(label + '-compile-' + name, ['c++', '-std=c++17', '-O0', '-g',
                         '-I' + str(SOURCE / 'include'), str(SOURCE / ('tools/' + name + '.cpp')),
                         str(library), '-pthread', '-ldl', '-o', str(binary)])
    results['steps']['compile-' + name] = compile_result
    if compile_result['exitCode']:
        break
    with tempfile.TemporaryDirectory(prefix='cluster-fix-') as work:
        entry = run(label + '-' + name, ['unshare', '--mount', '--net', '--ipc', '--pid',
                     '--fork', '--mount-proc', 'sh', str(ISOLATE), str(binary), work], timeout=90)
        entry['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        results['steps'][name] = entry
(HERE / (label + '-result.json')).write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(int(any(item['exitCode'] for item in results['steps'].values())))
