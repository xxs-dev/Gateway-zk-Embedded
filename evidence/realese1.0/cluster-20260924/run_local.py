"""Run only the two reviewed EMS suites in private Linux namespaces."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2]
BUILD = Path('/tmp/gateway-fixed-cluster-7e392ce-20260924')
TESTS = ('ems_cluster_test', 'graph_ems_cluster_dispatch_test')


def command(label, args, cwd=None, timeout=600):
    start = time.monotonic()
    with (HERE / (label + '.log')).open('w') as log:
        try:
            run = subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                 timeout=timeout, check=False)
            code = run.returncode
        except subprocess.TimeoutExpired:
            code = 124
            log.write('\nHARNESS TIMEOUT\n')
    item = {'label': label, 'argv': args, 'exitCode': code,
            'seconds': round(time.monotonic() - start, 3)}
    print(json.dumps(item), flush=True)
    return item


def suites(directory, label):
    results = []
    for name in TESTS:
        binary = directory / name
        with tempfile.TemporaryDirectory(prefix='cluster-suite-') as work:
            args = ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork',
                    '--mount-proc', 'sh', str(HERE / 'isolated_test.sh'),
                    str(binary), work]
            item = command(label + '-' + name, args, timeout=90)
            item['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
            results.append(item)
    return results


def main():
    mode = sys.argv[1]
    result = {'savedAtUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'mode': mode, 'source': str(SOURCE), 'runs': []}
    if mode == 'legacy':
        result['provenance'] = '2026-09-06 build; NOT proof of fixed 7e392cec source'
        result['runs'] = suites(Path('/home/wmzdxs/gateway-zk-review-build'), mode)
    elif mode == 'fixed':
        result['provenance'] = 'Native Linux x86 test-only build from pinned worktree'
        config = command('fixed-configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD),
                           '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON'])
        result['runs'].append(config)
        if config['exitCode'] == 0:
            build = command('fixed-build', ['cmake', '--build', str(BUILD), '--target',
                            *TESTS, '--parallel', '4'], timeout=900)
            result['runs'].append(build)
            if build['exitCode'] == 0:
                result['runs'].extend(suites(BUILD, mode))
    else:
        raise ValueError(mode)
    (HERE / (mode + '-result.json')).write_text(json.dumps(result, indent=2) + '\n')
    return int(any(item['exitCode'] for item in result['runs']))


if __name__ == '__main__':
    sys.exit(main())
