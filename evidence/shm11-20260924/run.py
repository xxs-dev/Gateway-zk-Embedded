"""Local native regressions in private IPC/net/mount namespaces, no device access."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
BUILD = Path('/home/wmzdxs/.cache/edge-shm11-egress-20260924')
BASELINE = Path('/home/wmzdxs/.cache/edge-cluster-lease-worker-20260924/libedge_gateway.a')
ISOLATE = SOURCE.parent / 'cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh'
label = sys.argv[1]
tests = sys.argv[2:] or ['memory_point_store_shm11_test']
BUILD.mkdir(parents=True, exist_ok=True)
result = {'label': label, 'steps': {}, 'sources': {}}
for folder in ('src', 'include', 'tools'):
    for path in (SOURCE / folder).rglob('*'):
        if path.suffix in ('.cpp', '.hpp'):
            result['sources'][str(path.relative_to(SOURCE))] = hashlib.sha256(path.read_bytes()).hexdigest()

def run(name, argv, timeout=300):
    started = time.monotonic()
    with (HERE / (label + '-' + name + '.log')).open('x') as log:
        process = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    entry = {'argv': argv, 'exitCode': process.returncode, 'seconds': round(time.monotonic()-started, 3)}
    result['steps'][name] = entry
    print(name, json.dumps(entry), flush=True)
    return process.returncode

try:
    library = BASELINE
    if not label.startswith('baseline-'):
        if run('configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD), '-DCMAKE_BUILD_TYPE=Debug']):
            raise SystemExit(2)
        if run('build', ['cmake', '--build', str(BUILD), '--target', 'edge_gateway', '--parallel', '4']):
            raise SystemExit(2)
        library = BUILD / 'libedge_gateway.a'
        if 'memory_point_store_migration_test' in tests:
            if run('migration-cli', ['cmake', '--build', str(BUILD), '--target', 'memory_point_store_migrate']):
                raise SystemExit(2)
    result['librarySha256'] = hashlib.sha256(library.read_bytes()).hexdigest()
    for test in tests:
        binary = BUILD / (test + '-' + label)
        extra = [str(SOURCE/'src/memory_point_store_migration.cpp')] if 'migration' in test else []
        if run('compile-' + test, ['c++', '-std=c++17', '-O0', '-g', '-I'+str(SOURCE/'include'),
                str(SOURCE/'tools'/(test+'.cpp')), *extra, str(library), '-pthread', '-ldl', '-o', str(binary)]):
            break
        with tempfile.TemporaryDirectory(prefix='shm11-', dir=BUILD) as work:
            (Path(work) / 'config').symlink_to(SOURCE / 'config', target_is_directory=True)
            os.environ['GATEWAY_MIGRATION_TEST_BACKUP_DIR'] = work
            run(test, ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
                      'sh', str(ISOLATE), str(binary), work], 120)
        result['steps'][test]['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
finally:
    with (HERE / (label+'-result.json')).open('x') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
raise SystemExit(int(any(x['exitCode'] for x in result['steps'].values())))
