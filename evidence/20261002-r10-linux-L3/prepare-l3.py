"""Freeze the eb6cc53 Git-byte AArch64 build inputs without connecting."""
import argparse
import ast
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
L2 = HERE.parent / '20261002-r10-linux'
REPO = HERE.parents[1]
COMMIT = 'eb6cc533ce730d6ff929d05a362070c18f455f6e'
BUILD_ROOT = '/srv/build/gw-r10-eb6cc53-build-L3'
TARGETS = ['SystemMonitor', 'diagnostic_process_test', 'system_monitor_diagnostic_test',
           'system_monitor_service_test']
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
PINS = {'run_bounded_probe.py': 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf',
        'd2_device_readonly_probe.py': '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'}
BASE_COMMIT = '438507cdb191b5b91c77eca2e672b4e74e06bd1f'
BASE_PATH = 'evidence/20261002-nativeboot-B2/execute-native-creator.py'
BASE_SHA = 'fee48c39ca46a3fc40cefed0782232183f582c397061ebac33979a117e52770a'
HELPERS = {'build-child.py': L2 / 'build-child.py',
           'build-group.py': L2 / 'build-group.py',
           'build-environment-base.py': L2 / 'build-environment.py',
           'build-environment.py': HERE / 'build-environment.py',
           'ownership-probe.cpp': None,
           'prepare-l3.py': HERE / 'prepare-l3.py',
           'execute-l3.py': HERE / 'execute-l3.py'}
L2_HELPER_PINS = {
    'build-child.py': 'eb8b06d93921e7d80bbc9e3f13e262d6b295a36e6fe8b9094b2f54aaf9527e19',
    'build-group.py': 'd8394316f09f28e473b9973b68d73a62d605f9c39bc97b8ad8e35e4a68f09cfa',
    'build-environment-base.py': '3d469e73c6f7ba507ecbc93f1e198d288713f4f3e702113b33ac35ce0b4091b4',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def blob(commit, path):
    return subprocess.check_output(['git', 'cat-file', 'blob', commit + ':' + path], cwd=str(REPO))


def helper_bytes(name):
    if name == 'ownership-probe.cpp':
        return blob(COMMIT, 'evidence/20261002-r10-linux/ownership-probe.cpp')
    return HELPERS[name].read_bytes()


def prepare():
    assert subprocess.check_output(['git', 'rev-parse', COMMIT], cwd=str(REPO)).decode().strip() == COMMIT
    top = subprocess.check_output(['git', 'ls-tree', '--name-only', COMMIT], cwd=str(REPO)).decode().splitlines()
    paths = ['CMakeLists.txt', 'include', 'src', 'tools', 'toolchains', 'test-lab'] + [
        path for path in top if Path(path).suffix in ('.cpp', '.hpp', '.h', '.c')]
    archive = subprocess.check_output(['git', '-c', 'core.autocrlf=false', '-c', 'core.eol=lf',
                                       'archive', '--format=tar', COMMIT, '--'] + paths, cwd=str(REPO))
    assert len(archive) <= 32 * 1024**2
    assert subprocess.check_output(['git', 'get-tar-commit-id'], input=archive, cwd=str(REPO)).decode().strip() == COMMIT
    rows = []
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream.getmembers():
            assert not member.name.startswith('/') and '..' not in Path(member.name).parts
            if member.isdir():
                continue
            assert member.isreg() and member.size <= 2 * 1024**2
            data = stream.extractfile(member).read()
            assert data == blob(COMMIT, member.name), 'archive/Git byte mismatch: ' + member.name
            rows.append(dict(path=member.name, bytes=len(data), sha256=sha(data), mode=member.mode & 0o777))
    assert rows and all(not row['path'].startswith('evidence/') for row in rows)
    supplements = {}
    for name in HELPERS:
        data = helper_bytes(name)
        if name in L2_HELPER_PINS:
            assert sha(data) == L2_HELPER_PINS[name], 'accepted L2 helper drift: ' + name
        if name.endswith('.py'):
            ast.parse(data.decode(), feature_version=(3, 6))
        supplements[name] = dict(bytes=len(data), sha256=sha(data), mode=0o500)
    for name, digest in PINS.items():
        assert sha((COORD / name).read_bytes()) == digest
    assert sha(blob(BASE_COMMIT, BASE_PATH)) == BASE_SHA
    manifest = dict(sourceCommit=COMMIT, sourceArchiveBytes=len(archive), sourceArchiveSha256=sha(archive),
                    sourceFiles=rows, supplementalFiles=supplements, buildRoot=BUILD_ROOT,
                    buildHost='192.168.22.11', targets=TARGETS, artifactNames=TARGETS + ['ownership-probe'],
                    executionAuthorized=False, boardExecutionReady=False, connectionPins=PINS,
                    baselineGitSha256=BASE_SHA, gitByteIdentity=True)
    return archive, manifest


def primitives():
    module = {'__name__': 'r10_pinned_baseline', '__file__': str(REPO / BASE_PATH)}
    exec(compile(blob(BASE_COMMIT, BASE_PATH), BASE_PATH, 'exec'), module)
    return module


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--freeze', action='store_true')
    args = parser.parse_args()
    archive, manifest = prepare()
    encoded = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode()
    if args.freeze:
        with (HERE / 'input-manifest-L3.json').open('xb') as output:
            output.write(encoded)
    print(json.dumps(dict(sourceCommit=COMMIT, sourceFiles=len(manifest['sourceFiles']),
                          sourceArchiveBytes=len(archive), sourceArchiveSha256=sha(archive),
                          manifestSha256=sha(encoded), executionAuthorized=False), indent=2))
