"""Compile only the test helper against the qualified native candidate library."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = Path('/var/tmp/ems-production-candidate-20260924-build')
LIBRARY = BUILD / 'libedge_gateway.a'
OUT = HERE / sys.argv[1]
OUT.mkdir(exist_ok=False)
result = {'librarySha256': hashlib.sha256(LIBRARY.read_bytes()).hexdigest(), 'steps': {}, 'sources': {}}
for name in ('ems_shadow_live_helper.cpp', 'ems_shadow_live_helper_test.cpp'):
    result['sources'][name] = hashlib.sha256((ROOT / 'tools' / name).read_bytes()).hexdigest()


def run(name, args):
    with (OUT / (name + '.log')).open('wb') as log:
        child = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    result['steps'][name] = {'argv': [str(item) for item in args], 'exitCode': child.returncode}
    print(name, child.returncode, flush=True)
    return child.returncode


try:
    with tempfile.TemporaryDirectory(prefix='helper-duration-', dir=BUILD) as directory:
        sources = ROOT / 'tools'
        if len(sys.argv) > 2:
            sources = Path(directory)
            old = subprocess.check_output(['git', '--git-dir=/mnt/d/workspace/Embedded/Gateway-zk/.git',
                                           'show', sys.argv[2] + ':tools/ems_shadow_live_helper.cpp'])
            (sources / 'ems_shadow_live_helper.cpp').write_bytes(old)
            (sources / 'ems_shadow_live_helper_test.cpp').write_bytes((ROOT / 'tools/ems_shadow_live_helper_test.cpp').read_bytes())
            result['oldHelperCommit'] = sys.argv[2]
            result['sources']['ems_shadow_live_helper.cpp'] = hashlib.sha256(old).hexdigest()
        for target in ('ems_shadow_live_helper', 'ems_shadow_live_helper_test'):
            binary = Path(directory) / target
            if run('compile-' + target, ['c++', '-std=c++17', '-O0', '-g', '-I' + str(ROOT / 'include'),
                                        str(sources / (target + '.cpp')), str(LIBRARY),
                                        '-pthread', '-ldl', '-o', str(binary)]):
                raise SystemExit(1)
            result['steps']['compile-' + target]['binarySha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        run('test', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc', 'sh',
                     str(ROOT / 'evidence/ems-integrated-20260924/isolated.sh'),
                     str(Path(directory) / 'ems_shadow_live_helper_test'), directory])
        run('cxx14', ['c++', '-std=c++14', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                      str(ROOT / 'tools/ems_shadow_live_helper.cpp'),
                      str(ROOT / 'tools/ems_shadow_live_helper_test.cpp')])
finally:
    with (OUT / 'result.json').open('w') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
sys.exit(int(any(step['exitCode'] for step in result['steps'].values())))
