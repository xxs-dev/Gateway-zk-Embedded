"""Compare old/new bridge recovery locally; never access devices or existing SHM."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
BUILD = Path('/home/wmzdxs/.cache/edge-shm11-egress-20260924')
LIBRARY = BUILD / 'libedge_gateway.a'
ISOLATE = SOURCE.parent / 'cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh'
BEFORE = 'b35a306878ce2a40c7a6eb92d48b20a0793afec7'
LABEL = sys.argv[1] if len(sys.argv) > 1 else 'reelection-comparison-20260924'
TESTS = {
    'ems_cluster_output_authority_test':
        'new term sequence 1 must recover in the same bridge after old term sequence 157',
    'ems_cluster_strategy_startup_test':
        'same bridge must authorize the first low-sequence dispatch after real re-election',
}
result = {'before': BEFORE, 'steps': {}, 'sourcesSha256': {},
          'librarySha256': hashlib.sha256(LIBRARY.read_bytes()).hexdigest()}
for relative in ['src/ems_cluster_points.cpp'] + ['tools/' + test + '.cpp' for test in TESTS]:
    result['sourcesSha256'][relative] = hashlib.sha256((SOURCE / relative).read_bytes()).hexdigest()


def run(name, argv, expected=0, stdin=None, message=None):
    log_path = HERE / (LABEL + '-' + name + '.log')
    with log_path.open('xb') as log:
        process = subprocess.run(argv, input=stdin, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    ok = process.returncode == expected
    if message:
        ok = ok and message in log_path.read_text()
    result['steps'][name] = {'argv': [str(item) for item in argv], 'exitCode': process.returncode,
                             'expectedExitCode': expected, 'expectedMessage': message, 'pass': ok}
    print(name, 'PASS' if ok else 'FAIL', flush=True)
    if not ok:
        raise RuntimeError(name + ' failed; inspect ' + str(log_path))


try:
    run('cxx14-syntax', ['c++', '-std=c++14', '-fsyntax-only', '-I' + str(SOURCE / 'include'),
                         str(SOURCE / 'src/ems_cluster_points.cpp'),
                         *[str(SOURCE / 'tools' / (test + '.cpp')) for test in TESTS]])
    # The Windows worktree .git file contains a drive-letter path, not a WSL path.
    old_source = subprocess.check_output(['git', '--git-dir=/mnt/d/workspace/Embedded/Gateway-zk/.git',
                                          'show', BEFORE + ':src/ems_cluster_points.cpp'])
    result['oldBridgeSha256'] = hashlib.sha256(old_source).hexdigest()
    with tempfile.TemporaryDirectory(prefix='reelection-', dir=BUILD) as directory:
        work = Path(directory)
        old_object = work / 'old-bridge.o'
        run('compile-old-bridge', ['c++', '-std=c++17', '-O0', '-g', '-I' + str(SOURCE / 'include'),
                                  '-x', 'c++', '-', '-c', '-o', str(old_object)], stdin=old_source)
        for test, message in TESTS.items():
            old_binary = work / (test + '-old')
            run('link-old-' + test, ['c++', '-std=c++17', '-O0', '-g', '-I' + str(SOURCE / 'include'),
                                    str(SOURCE / 'tools' / (test + '.cpp')), str(old_object),
                                    str(LIBRARY), '-pthread', '-ldl', '-o', str(old_binary)])
            run('red-' + test, ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
                                'sh', str(ISOLATE), str(old_binary), str(work)], expected=1, message=message)
            new_binary = work / (test + '-new')
            run('link-new-' + test, ['c++', '-std=c++17', '-O0', '-g', '-I' + str(SOURCE / 'include'),
                                    str(SOURCE / 'tools' / (test + '.cpp')), str(LIBRARY),
                                    '-pthread', '-ldl', '-o', str(new_binary)])
            result['steps']['red-' + test]['binarySha256'] = hashlib.sha256(old_binary.read_bytes()).hexdigest()
            for repeat in range(3):
                step = 'green-' + test + '-' + str(repeat + 1)
                run(step, ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
                           'sh', str(ISOLATE), str(new_binary), str(work)])
                result['steps'][step]['binarySha256'] = hashlib.sha256(new_binary.read_bytes()).hexdigest()
        core_binary = work / 'ems_cluster_test-core-only'
        run('compile-core-only', ['c++', '-std=c++17', '-O0', '-g', '-DEMS_CLUSTER_CORE_ONLY',
                                 '-I' + str(SOURCE / 'include'), str(SOURCE / 'tools/ems_cluster_test.cpp'),
                                 str(LIBRARY), '-pthread', '-ldl', '-o', str(core_binary)])
        run('core-only', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork', '--mount-proc',
                          'sh', str(ISOLATE), str(core_binary), str(work)])
        result['steps']['core-only']['binarySha256'] = hashlib.sha256(core_binary.read_bytes()).hexdigest()
finally:
    with (HERE / (LABEL + '-result.json')).open('x') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
