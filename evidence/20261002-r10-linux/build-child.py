"""One isolated build; retains logs/output, no shared-tree edits or dependency install."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import resource
import selectors
import signal
import stat
import struct
import subprocess
import tarfile
import time


def build(root, manifest):
    root = Path(root)
    assert str(root) == manifest['buildRoot'] and root.resolve() == root and root.stat().st_uid == 0
    assert stat.S_IMODE(root.stat().st_mode) == 0o700
    shared = '/srv/build/Gateway-zk'
    def shared_status():
        data = subprocess.check_output(['git', '-C', shared, 'status', '--porcelain=v1', '--untracked-files=all'], timeout=15)
        assert len(data) <= 65536
        head = subprocess.check_output(['git', '-C', shared, 'rev-parse', 'HEAD'], timeout=15).decode().strip()
        return dict(head=head, pathStatusSha256=hashlib.sha256(data).hexdigest(),
                    proofScope='HEAD and tracked/untracked path status, NOT complete file-content hashing')
    result = dict(sourceCommit=manifest['sourceCommit'], root=str(root), phase='STARTED', artifacts=[])
    libc = ctypes.CDLL(None)
    assert libc.prctl(36, 1, 0, 0, 0) == 0
    def interrupted(signum, frame):
        raise TimeoutError('outer build interrupted by signal')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    def save():
        with (root / 'build-result.json').open('w') as out:
            json.dump(result, out, sort_keys=True, indent=2); out.flush(); os.fsync(out.fileno())
    def bounded(argv, label, seconds):
        assert not (root / (label + '.log')).exists()
        parent = os.getpid()
        def limits():
            for key, value in ((resource.RLIMIT_AS, 2 * 1024**3), (resource.RLIMIT_FSIZE, 128 * 1024**2),
                               (resource.RLIMIT_NOFILE, 256), (resource.RLIMIT_CORE, 0)):
                resource.setrlimit(key, (value, value))
            assert libc.prctl(1, signal.SIGKILL, 0, 0, 0) == 0
            assert os.getppid() == parent
        command = ['/usr/bin/unshare', '--pid', '--fork', '--kill-child=KILL',
                   '/usr/bin/python3', '-I', '-B', str(root / 'build-group.py'), json.dumps(argv)]
        with (root / (label + '.log')).open('xb') as log:
            child = subprocess.Popen(command, cwd=str(root / 'source'), stdout=subprocess.PIPE, stdin=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=limits, env=env)
            row = dict(argv=argv, boundaryArgv=command, leader=child.pid, timeoutSeconds=seconds)
            result.setdefault('commands', []).append(row); save()
            namespace = None; raw = bytearray(); selector = selectors.DefaultSelector()
            selector.register(child.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + seconds; startup = time.monotonic() + 10; admitted = False
            try:
                while True:
                    assert time.monotonic() < (deadline if admitted else startup), 'build command deadline'
                    for _, _ in selector.select(0.1):
                        chunk = os.read(child.stdout.fileno(), 4096)
                        if not chunk: break
                        raw.extend(chunk); log.write(chunk); log.flush()
                        assert len(raw) <= 16 * 1024**2, 'build log bound'
                        if not admitted and b'\n' in raw:
                            namespace = json.loads(raw.split(b'\n', 1)[0])['pidNamespace']
                            children = Path('/proc/%d/task/%d/children' % (child.pid, child.pid)).read_text().split()
                            assert len(children) == 1
                            assert os.readlink('/proc/' + children[0] + '/ns/pid') == namespace != os.readlink('/proc/self/ns/pid')
                            row['ownedNamespace'] = dict(initHostPid=int(children[0]), namespace=namespace); save()
                            child.stdin.write(b'GO\n'); child.stdin.flush(); child.stdin.close(); admitted = True
                    if child.poll() is not None:
                        tail = child.stdout.read(16 * 1024**2 + 1 - len(raw)); raw.extend(tail); log.write(tail)
                        assert len(raw) <= 16 * 1024**2
                        row['exitCode'] = child.returncode; break
            except BaseException as error:
                row.update(interrupted=True, errorType=type(error).__name__, errorMessage=str(error))
                raise
            finally:
                # Exact unreaped Popen owner only. --kill-child and PDEATHSIG
                # kill namespace PID1; kernel kills even setsid descendants.
                if child.poll() is None: child.kill()
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
                row['logBytes'] = (root / (label + '.log')).stat().st_size
                assert row['logBytes'] <= 16 * 1024**2, 'build log bound'
                row['logSha256'] = hashlib.sha256((root / (label + '.log')).read_bytes()).hexdigest(); save()
        assert row['exitCode'] == 0 and not row.get('interrupted'), label + ' failed; retain logs'
    compiler_bin = '/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin'
    env = dict(PATH=compiler_bin + ':/usr/local/bin:/usr/bin:/bin', HOME='/root', LC_ALL='C', TMPDIR=str(root))
    try:
        group = root / 'build-group.py'
        assert hashlib.sha256(group.read_bytes()).hexdigest() == manifest['supplementalFiles']['build-group.py']['sha256']
        probe = root / 'ownership-probe.cpp'
        assert hashlib.sha256(probe.read_bytes()).hexdigest() == manifest['supplementalFiles']['ownership-probe.cpp']['sha256']
        helptext = subprocess.check_output(['/usr/bin/unshare', '--help'], timeout=5)
        assert len(helptext) <= 65536 and b'--kill-child' in helptext
        result['sharedBefore'] = shared_status(); save()
        archive = root / 'source.tar'
        assert archive.stat().st_size == manifest['sourceArchiveBytes']
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == manifest['sourceArchiveSha256']
        with archive.open('rb') as data:
            actual = subprocess.check_output(['git', 'get-tar-commit-id'], stdin=data, timeout=10).decode().strip()
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
        bounded(['bash', 'tools/build_edge_aarch64.sh', '--build-dir', str(root / 'build'), '--jobs', '2'] + manifest['targets'], 'build', 1200)
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
        result.update(phase='FAILED', errorType=type(error).__name__, errorMessage=str(error))
    finally:
        result['sharedAfter'] = shared_status()
        result['sharedUnchanged'] = result.get('sharedBefore') == result['sharedAfter']; save()
    return result
