"""One isolated build; retains logs/output, no shared-tree edits or dependency install."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import selectors
import signal
import stat
import struct
import subprocess
import tarfile
import tempfile
import time
import traceback


def error_record(error):
    return dict(type=type(error).__name__, message=str(error), traceback=traceback.format_exc(limit=12))


def primary_error(result, error):
    if 'primaryError' not in result:
        result['primaryError'] = error_record(error)
    result['phase'] = 'FAILED'


def sync_directory(root):
    fd = os.open(str(root), os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def persist(root, result):
    fd, temporary = tempfile.mkstemp(prefix='.receipt-', dir=str(root))
    with os.fdopen(fd, 'wb') as output:
        output.write((json.dumps(result, sort_keys=True, indent=2) + '\n').encode())
        output.flush(); os.fsync(output.fileno())
    os.replace(temporary, str(root / 'build-result.json'))
    sync_directory(root)


def finish(result, check, save):
    # Each postcheck/save error is independent of the first error already saved.
    try:
        result['sharedAfter'] = check()
        result['sharedUnchanged'] = result.get('sharedBefore') == result['sharedAfter']
        if not result['sharedUnchanged']: result['phase'] = 'FAILED'
    except BaseException as error:
        result.setdefault('postcheckErrors', []).append(error_record(error)); result['phase'] = 'FAILED'
    try: save()
    except BaseException as error:
        result.setdefault('receiptErrors', []).append(error_record(error)); result['phase'] = 'FAILED'


def build(root, manifest, environment):
    import resource
    root = Path(root)
    assert str(root) == manifest['buildRoot'] and root.resolve() == root and root.stat().st_uid == 0
    assert stat.S_IMODE(root.stat().st_mode) == 0o700
    from_namespace = {}
    code = (root / 'build-environment.py').read_bytes()
    assert hashlib.sha256(code).hexdigest() == manifest['supplementalFiles']['build-environment.py']['sha256']
    exec(compile(code, 'build-environment.py', 'exec'), from_namespace)
    owner = environment['owner']
    def shared_status():
        check = from_namespace['git_read'](['config', '--name-only', '--get-regexp', r'^(filter\..*\.(clean|smudge|process)|diff\..*\.textconv)$'], owner)
        assert check.returncode == 1 and not check.stdout and not check.stderr, 'external content driver configured/unknown'
        status = from_namespace['git_read'](['status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=all'], owner)
        head_result = from_namespace['git_read'](['rev-parse', 'HEAD'], owner)
        assert status.returncode == head_result.returncode == 0 and not status.stderr and not head_result.stderr
        data = status.stdout; head = head_result.stdout.decode().strip()
        return dict(head=head, pathStatusSha256=hashlib.sha256(data).hexdigest(),
                    gitExecutable='/usr/bin/git', uid=1000, optionalLocks=False, ownerIdentity=owner,
                    proofScope='HEAD and tracked/untracked path status, NOT complete file-content hashing')
    result = dict(sourceCommit=manifest['sourceCommit'], root=str(root), phase='STARTED', artifacts=[])
    libc = ctypes.CDLL(None)
    def interrupted(signum, frame):
        raise TimeoutError('outer build interrupted by signal')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    def save():
        try: persist(root, result)
        except BaseException as error:
            result.setdefault('receiptErrors', []).append(error_record(error)); result['phase'] = 'FAILED'
    def bounded(argv, label, seconds):
        assert not (root / (label + '.log')).exists()
        parent = os.getpid()
        def limits():
            for key, value in ((resource.RLIMIT_AS, 2 * 1024**3), (resource.RLIMIT_FSIZE, 128 * 1024**2),
                               (resource.RLIMIT_NOFILE, 256), (resource.RLIMIT_CORE, 0)):
                resource.setrlimit(key, (value, value))
            assert libc.prctl(1, signal.SIGKILL, 0, 0, 0) == 0
            assert os.getppid() == parent
        command = ['/usr/bin/python3', '-I', '-B', str(root / 'build-group.py'), json.dumps(argv), str(seconds), str(parent)]
        assert not result.get('receiptErrors'), 'receipt failed before launcher'
        with (root / (label + '.log')).open('xb') as log:
            child = subprocess.Popen(command, cwd=str(root / 'source'), stdout=subprocess.PIPE, stdin=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=limits, env=env)
            row = dict(argv=argv, boundaryArgv=command, leader=child.pid, timeoutSeconds=seconds)
            result.setdefault('commands', []).append(row); save()
            namespace = None; raw = bytearray(); selector = selectors.DefaultSelector()
            selector.register(child.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + seconds; startup = time.monotonic() + 10; admitted = False
            try:
                assert not result.get('receiptErrors'), 'launcher receipt failed before GO'
                while True:
                    assert time.monotonic() < (deadline if admitted else startup), 'build command deadline'
                    for _, _ in selector.select(0.1):
                        chunk = os.read(child.stdout.fileno(), 4096)
                        if not chunk: break
                        raw.extend(chunk); log.write(chunk); log.flush()
                        assert len(raw) <= 16 * 1024**2, 'build log bound'
                        if not admitted and b'\n' in raw:
                            namespace = json.loads(raw.split(b'\n', 1)[0])['pidNamespace']
                            proof = json.loads(raw.split(b'\n', 1)[0])
                            assert proof['pdeathSignal'] == signal.SIGKILL and proof['launcherHostPid'] == child.pid
                            children = Path('/proc/%d/task/%d/children' % (child.pid, child.pid)).read_text().split()
                            assert len(children) == 1
                            assert os.readlink('/proc/' + children[0] + '/ns/pid') == namespace != os.readlink('/proc/self/ns/pid')
                            row['ownedNamespace'] = dict(initHostPid=int(children[0]), namespace=namespace); save()
                            assert not result.get('receiptErrors'), 'namespace receipt failed before GO'
                            child.stdin.write(b'GO\n'); child.stdin.flush(); child.stdin.close(); admitted = True
                    if child.poll() is not None:
                        tail = child.stdout.read(16 * 1024**2 + 1 - len(raw)); raw.extend(tail); log.write(tail)
                        assert len(raw) <= 16 * 1024**2
                        row['exitCode'] = child.returncode; break
            except BaseException as error:
                row.update(interrupted=True, errorType=type(error).__name__, errorMessage=str(error))
                primary_error(result, error); save()  # Before any cleanup/postcheck can fail.
                raise
            finally:
                try:
                    if child.poll() is None: child.kill()  # Exact Popen launcher, never global/PGID kill.
                    child.wait(timeout=5)
                    end = time.monotonic() + 5
                    while time.monotonic() < end:
                        try:
                            got, _ = os.waitpid(-1, os.WNOHANG)
                            if got == 0: time.sleep(0.01)
                        except ChildProcessError: break
                    remaining = []
                    if namespace:
                        scanned = 0
                        for proc in Path('/proc').iterdir():
                            if not proc.name.isdigit(): continue
                            scanned += 1; assert scanned <= 32768
                            try:
                                if os.readlink(str(proc / 'ns/pid')) == namespace: remaining.append(int(proc.name))
                            except (FileNotFoundError, ProcessLookupError): pass
                    row['remainingNamespaceProcesses'] = remaining
                    assert not remaining, 'compiler namespace cleanup unconfirmed'
                except BaseException as error:
                    row['cleanupError'] = error_record(error)
                    primary_error(result, error); save()
                try:
                    row['logBytes'] = (root / (label + '.log')).stat().st_size
                    assert row['logBytes'] <= 16 * 1024**2, 'build log bound'
                    row['logSha256'] = hashlib.sha256((root / (label + '.log')).read_bytes()).hexdigest()
                except BaseException as error:
                    row['logError'] = error_record(error); primary_error(result, error)
                save()
        assert not row.get('cleanupError') and not row.get('logError') and not result.get('receiptErrors'), 'command cleanup/receipt unconfirmed'
        assert row['exitCode'] == 0 and not row.get('interrupted'), label + ' failed; retain logs'
    compiler_bin = '/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin'
    env = dict(from_namespace['BUILD_ENV'], TMPDIR=str(root))
    try:
        save()
        assert not result.get('receiptErrors'), 'initial receipt failed'
        assert libc.prctl(36, 1, 0, 0, 0) == 0
        assert from_namespace['owner_identity']() == owner
        current_environment = from_namespace['inspect_environment'](require_absent=False)
        assert all(current_environment[key] == environment[key] for key in ('bootId', 'parent', 'owner', 'paths', 'cmakeExecutable', 'bashExecutable', 'python'))
        result['environment'] = current_environment; save()
        group = root / 'build-group.py'
        assert hashlib.sha256(group.read_bytes()).hexdigest() == manifest['supplementalFiles']['build-group.py']['sha256']
        probe = root / 'ownership-probe.cpp'
        assert hashlib.sha256(probe.read_bytes()).hexdigest() == manifest['supplementalFiles']['ownership-probe.cpp']['sha256']
        result['sharedBefore'] = shared_status(); save()
        archive = root / 'source.tar'
        assert archive.stat().st_size == manifest['sourceArchiveBytes']
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == manifest['sourceArchiveSha256']
        with archive.open('rb') as data:
            actual = subprocess.check_output(['/usr/bin/git', 'get-tar-commit-id'], stdin=data, env=from_namespace['GIT_ENV'], timeout=10).decode().strip()
        assert actual == manifest['sourceCommit']
        source = root / 'source'; source.mkdir(mode=0o700)
        expected = {row['path']: row for row in manifest['sourceFiles']}
        with tarfile.open(str(archive)) as tar:
            files = [member for member in tar.getmembers() if not member.isdir()]
            assert {m.name for m in files} == set(expected) and len(files) == len(expected)
            for member in files:
                assert member.isreg() and not member.name.startswith('/') and '..' not in Path(member.name).parts
                row = expected[member.name]; data = tar.extractfile(member).read(row['bytes'] + 1)
                assert len(data) == row['bytes'] and hashlib.sha256(data).hexdigest() == row['sha256']
                destination = source / member.name; destination.parent.mkdir(parents=True, exist_ok=True)
                with destination.open('xb') as out: out.write(data)
                destination.chmod(row['mode'])
        assert os.statvfs(str(root)).f_bavail * os.statvfs(str(root)).f_frsize >= 2 * 1024**3
        result['compilerVersion'] = subprocess.check_output([compiler_bin + '/aarch64-linux-gnu-g++', '--version'], env=env, timeout=5).decode()
        # Exact existing entry, no --clean/--publish/--package; only three targets.
        bounded([environment['bashExecutable'], 'tools/build_edge_aarch64.sh', '--build-dir', str(root / 'build'), '--jobs', '2'] + manifest['targets'], 'build', 1200)
        bounded([compiler_bin + '/aarch64-linux-gnu-g++', '-std=c++1z', '-pthread',
                 '-DEDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS', '-I', str(source / 'include'),
                 str(source / 'src/diagnostic_process.cpp'), str(root / 'ownership-probe.cpp'),
                 '-o', str(root / 'build/ownership-probe')], 'ownership-build', 60)
        for name in manifest['targets'] + ['ownership-probe']:
            path = root / 'build' / name; data = path.read_bytes()
            assert len(data) <= 32 * 1024**2 and data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183
            meta = subprocess.check_output([compiler_bin + '/aarch64-linux-gnu-readelf', '-W', '-d', '-l', str(path)], timeout=10)
            assert len(meta) <= 65536
            with (root / (name + '-elf.log')).open('xb') as out: out.write(meta)
            result['artifacts'].append(dict(name=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), elfMetadataSha256=hashlib.sha256(meta).hexdigest()))
        for row in manifest['sourceFiles']:
            assert hashlib.sha256((source / row['path']).read_bytes()).hexdigest() == row['sha256'], 'post-build source drift'
        result['postSourceHashesVerified'] = len(manifest['sourceFiles'])
        result['phase'] = 'BUILT'
    except BaseException as error:
        primary_error(result, error); save()
    finally:
        finish(result, shared_status, save)
    return result
