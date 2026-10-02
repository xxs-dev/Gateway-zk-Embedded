"""Fixed Git-byte inputs; default local-only. Generated archive is not product code."""
import ast
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PRODUCT_COMMIT = '68de70fbc37cf3267116624a259a81510b2d74d3'
COMMIT = '438507cdb191b5b91c77eca2e672b4e74e06bd1f'
BUILD_ROOT = '/srv/build/gw-r10-68de70f-build-L2'
BOARD_ROOT = '/tmp/gw-r10-68de70f-23-L1'
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
PINS = {'run_bounded_probe.py': 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf',
        'd2_device_readonly_probe.py': '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'}
BASE_PATH = 'evidence/20261002-nativeboot-B2/execute-native-creator.py'
BASE_SHA = 'fee48c39ca46a3fc40cefed0782232183f582c397061ebac33979a117e52770a'
TARGETS = ['diagnostic_process_test', 'system_monitor_diagnostic_test', 'system_monitor_service_test']
SCRIPTS = ['build-child.py', 'build-group.py', 'build-environment.py', 'board-child.py', 'namespace-child.py', 'ownership-probe.cpp', 'execute-r10.py', 'prepare-r10.py']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def blob(path):
    return subprocess.check_output(['git', 'cat-file', 'blob', COMMIT + ':' + path], cwd=str(REPO))


def prepare():
    # CMake configures every named source even when only three targets are built.
    top = subprocess.check_output(['git', 'ls-tree', '--name-only', COMMIT], cwd=str(REPO)).decode().splitlines()
    paths = ['CMakeLists.txt', 'include', 'src', 'tools', 'toolchains', 'test-lab'] + [
        path for path in top if Path(path).suffix in ('.cpp', '.hpp', '.h', '.c')]
    archive = subprocess.check_output(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf',
                                       'archive', '--format=tar', COMMIT, '--'] + paths, cwd=str(REPO))
    assert len(archive) <= 32 * 1024**2, 'source archive bound'
    commit = subprocess.check_output(['git', 'get-tar-commit-id'], input=archive, cwd=str(REPO)).decode().strip()
    assert commit == COMMIT
    rows = []
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream.getmembers():
            assert not member.name.startswith('/') and '..' not in Path(member.name).parts
            if member.isdir():
                continue
            assert member.isreg() and member.size <= 2 * 1024**2, 'nonregular/large source'
            data = stream.extractfile(member).read()
            assert data == blob(member.name), 'archive/Git byte mismatch: ' + member.name
            rows.append(dict(path=member.name, bytes=len(data), sha256=sha(data), mode=member.mode & 0o777))
    audit = json.loads((COORD / 'r10-source-checkpoint-independent.json').read_bytes())
    assert audit['commit'] == PRODUCT_COMMIT
    by_path = {row['path']: row for row in rows}
    for row in audit['inputs']:
        if row['path'] != 'tools/system_monitor_diagnostic_test.cpp':
            assert by_path[row['path']]['sha256'] == row['gitSha256']
    assert subprocess.check_output(['git', 'diff', '--name-only', PRODUCT_COMMIT, COMMIT, '--', 'src', 'include', 'CMakeLists.txt'], cwd=str(REPO)) == b''
    supplements = {}
    for name in SCRIPTS:
        data = (HERE / name).read_bytes()
        if name.endswith('.py'):
            ast.parse(data.decode(), feature_version=(3, 6))
        supplements[name] = dict(bytes=len(data), sha256=sha(data), mode=0o500)
    for name, digest in PINS.items():
        assert sha((COORD / name).read_bytes()) == digest
    base = blob(BASE_PATH)
    assert sha(base) == BASE_SHA
    manifest = dict(sourceCommit=COMMIT, productCommit=PRODUCT_COMMIT, sourceArchiveBytes=len(archive), sourceArchiveSha256=sha(archive),
                    sourceFiles=rows, supplementalFiles=supplements, buildRoot=BUILD_ROOT, boardRoot=BOARD_ROOT,
                    buildHost='192.168.22.11', boardHost='192.168.22.23', targets=TARGETS,
                    executionAuthorized=False, boardExecutionReady=False, connectionPins=PINS, baselineGitSha256=BASE_SHA,
                    gitByteIdentity=True)
    return archive, manifest


def primitives():
    # Reuse the frozen baseline sampler, not the old execution/scope.
    module = {'__name__': 'r10_pinned_baseline', '__file__': str(REPO / BASE_PATH)}
    exec(compile(blob(BASE_PATH), BASE_PATH, 'exec'), module)
    return module


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--freeze', action='store_true')
    args = parser.parse_args()
    archive, manifest = prepare()
    encoded = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode()
    if args.freeze:
        if (HERE / 'source.tar').exists(): assert (HERE / 'source.tar').read_bytes() == archive
        else:
            with (HERE / 'source.tar').open('xb') as output: output.write(archive)
        with (HERE / 'input-manifest-L2.json').open('xb') as output: output.write(encoded)
    print(json.dumps(dict(sourceCommit=COMMIT, sourceFiles=len(manifest['sourceFiles']),
                          sourceArchiveBytes=len(archive), sourceArchiveSha256=sha(archive),
                          manifestSha256=sha(encoded), executionAuthorized=False), indent=2))
