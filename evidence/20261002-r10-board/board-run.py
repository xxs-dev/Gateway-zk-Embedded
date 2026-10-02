"""One bounded private fixture; retain first error and after-cleanup baseline."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import traceback

KEYS = ('machineCode', 'bootId', 'stateDir', 'stateSha256', 'approvalSha256', 'activeSha256',
        'guardSha256', 'configSha256', 'markerSha256', 'readers', 'runtimeProcesses', 'segments',
        'offlineLock', 'segmentIdentities', 'readerStartTicks', 'readerShmMaps')


def persist(root, result):
    fd, temporary = tempfile.mkstemp(prefix='.receipt-', dir=str(root))
    with os.fdopen(fd, 'wb') as stream:
        stream.write((json.dumps(result, sort_keys=True, indent=2)+'\n').encode()); stream.flush(); os.fsync(stream.fileno())
    os.replace(temporary, str(root/'result.json'))
    fd = os.open(str(root), os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def primary(result, error):
    if 'primaryError' not in result:
        result['primaryError'] = dict(type=type(error).__name__, message=str(error), traceback=traceback.format_exc(limit=10))


def accepted(result):
    return (result.get('fixtureExitCode') == 0 and result.get('remainingNamespaceProcesses') == []
            and result.get('workRemoved') is True and result.get('baselineChanges') == []
            and result.get('allInputHashesUnchanged') is True and not any(result.get(k) for k in
                 ('primaryError', 'cleanupError', 'postcheckError', 'receiptError')))


def checked_init(row, launcher_pid, host):
    pid = row['initHostPid']
    proof = row['namespaceProof']
    assert type(pid) is int and pid > 1 and row['launcherHostPid'] == launcher_pid
    assert row['pdeathSignal'] == 9 and set(proof) == {'pid', 'mnt', 'net'}
    with Path('/proc/%d/status' % pid).open('rb') as stream:
        status = stream.read(4097)
    assert len(status) <= 4096, 'private PID1 status bound'
    fields = dict(line.split(b':', 1) for line in status.splitlines() if b':' in line)
    assert fields[b'PPid'].split() == [str(launcher_pid).encode()]
    assert fields[b'NSpid'].split() == [str(pid).encode(), b'1']
    assert all(os.readlink('/proc/%d/ns/%s' % (pid, name)) == proof[name] != host[name]
               for name in ('pid', 'mnt', 'net'))
    return dict(initHostPid=pid, proof=proof)


def run(root, manifest, baseline, expected, environment):
    root = Path(root); work = root/'work'; child = None; proof = None; identity = None
    result = dict(privateFixtureOnly=True, realLifecycleCalled=False, productCommit=manifest['productCommit'])
    def save(): persist(root, result)
    def validate_inputs():
        for name, row in manifest['remoteFiles'].items():
            path = root/row['directory']/name; info = path.lstat()
            assert path.resolve() == path and stat.S_ISREG(info.st_mode) and info.st_uid == 0 and info.st_nlink == 1
            assert info.st_size == row['bytes'] and stat.S_IMODE(info.st_mode) == row['mode']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256']
    def stop(signum, frame): raise TimeoutError('outer board deadline/signal')
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    try:
        info = root.lstat()
        assert str(root) == manifest['root'] and root.resolve() == root and stat.S_ISDIR(info.st_mode)
        assert info.st_uid == 0 and stat.S_IMODE(info.st_mode) == 0o700
        save(); validate_inputs()
        result['before'] = baseline()
        assert all(result['before']['snapshot'][k] == expected['snapshot'][k] for k in KEYS)
        assert result['before']['parentNamespace'] == expected['parentNamespace']
        module = {}; exec(compile((root/'source/environment.py').read_bytes(), 'environment.py', 'exec'), module)
        current = module['inspect'](absent=False)
        assert all(current[k] == environment[k] for k in ('bootId', 'parent', 'tools', 'capEff', 'python'))
        result['dependencies'] = module['runtime_gate'](root, manifest['elfRequirements']); save()
        assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0
        work.mkdir(mode=0o700); info = work.stat(); identity = (info.st_dev, info.st_ino)
        host = {n: os.readlink('/proc/self/ns/'+n) for n in ('pid', 'mnt', 'net')}
        host['shmDev'] = os.stat('/dev/shm').st_dev
        host['runtimeMetadata'] = [r['metadata'] for r in result['dependencies']['libraries']] + current['tools']
        command = ['/usr/bin/python3', '-I', '-B', str(root/'source/namespace-child.py'), str(root), json.dumps(host)]
        parent = os.getpid()
        def limits():
            import resource
            for key, value in ((resource.RLIMIT_AS, 768*1024**2), (resource.RLIMIT_FSIZE, 32*1024**2),
                               (resource.RLIMIT_NOFILE, 256), (resource.RLIMIT_CORE, 0)):
                resource.setrlimit(key, (value, value))
            assert ctypes.CDLL(None).prctl(1, signal.SIGKILL, 0, 0, 0) == 0 and os.getppid() == parent
        argv = ['/usr/bin/python3', '-I', '-B', str(root/'source/board-group.py'), json.dumps(command), str(parent)]
        with (root/'fixture.log').open('xb') as log:
            child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     env=dict(PATH='/usr/bin:/bin', LC_ALL='C', HOME='/nonexistent'),
                                     start_new_session=True, preexec_fn=limits)
            result.update(launcherPid=child.pid, argv=argv); save()
            selector = selectors.DefaultSelector(); selector.register(child.stdout, selectors.EVENT_READ)
            deadline = time.monotonic()+240; startup = time.monotonic()+10; admitted = False; output = bytearray()
            while True:
                assert time.monotonic() < (deadline if admitted else startup), 'board fixture deadline'
                for _, _ in selector.select(0.1):
                    chunk = os.read(child.stdout.fileno(), 4096)
                    if not chunk: break
                    output.extend(chunk); log.write(chunk); log.flush()
                    assert len(output) <= 262144, 'fixture output cap'
                    if not admitted and b'\n' in output:
                        row = json.loads(output.split(b'\n', 1)[0]); proof = row['namespaceProof']
                        result['ownedNamespace'] = checked_init(row, child.pid, host); save()
                        child.stdin.write(b'GO\n'); child.stdin.flush(); child.stdin.close(); admitted = True
                if child.poll() is not None:
                    tail = child.stdout.read(262145-len(output)); output.extend(tail); log.write(tail)
                    assert len(output) <= 262144
                    result.update(fixtureExitCode=child.returncode, fixtureOutput=output.decode('utf-8', 'replace'))
                    break
            selector.close()
    except BaseException as error:
        primary(result, error)
        try: save()  # Persist first error before cleanup/postcheck.
        except BaseException as secondary: result['receiptError'] = str(secondary)
    finally:
        try:
            if child is not None:
                if child.poll() is None: child.kill()  # Exact Popen owner, not global/PGID kill.
                child.wait(timeout=5)
                end = time.monotonic()+5
                adopted = []
                cleared = False
                while time.monotonic() < end:
                    try:
                        pid, _ = os.waitpid(-1, os.WNOHANG)
                        if pid > 0: adopted.append(pid)
                        else: time.sleep(0.01)
                    except ChildProcessError:
                        cleared = True; break
                result['reapedAdoptedChildren'] = adopted
                result['remainingAdoptedChildren'] = [] if cleared else 'unconfirmed'
                assert cleared, 'pre-GO/adopted child cleanup unconfirmed'
            remaining = []
            if proof:
                scanned = 0
                for p in Path('/proc').iterdir():
                    if not p.name.isdigit(): continue
                    scanned += 1
                    assert scanned <= 32768, 'private namespace inventory bound'
                    try:
                        if os.readlink(str(p/'ns/pid')) == proof['pid']: remaining.append(int(p.name))
                    except (FileNotFoundError, ProcessLookupError): pass
            assert not remaining, 'owned namespace cleanup unconfirmed'
            result['remainingNamespaceProcesses'] = remaining
            if identity:
                info = work.lstat()
                assert work.resolve() == work and not work.is_symlink() and (info.st_dev, info.st_ino) == identity
                mounts = Path('/proc/self/mountinfo').read_text()
                assert len(mounts) <= 262144 and mounts == expected['parentNamespace']['mountinfoRaw']
                assert shutil.rmtree.avoids_symlink_attacks
                shutil.rmtree(str(work)); result['workRemoved'] = not os.path.lexists(str(work))
        except BaseException as error: result['cleanupError'] = str(error)
        try:
            result['afterCleanup'] = baseline()
            result['baselineChanges'] = [k for k in KEYS if expected['snapshot'][k] != result['afterCleanup']['snapshot'][k]]
            if expected['parentNamespace'] != result['afterCleanup']['parentNamespace']: result['baselineChanges'].append('parentNamespace')
            validate_inputs(); result['allInputHashesUnchanged'] = True
            if (root/'fixture.log').exists():
                data = (root/'fixture.log').read_bytes(); assert len(data) <= 262144
                result.update(fixtureLogSha256=hashlib.sha256(data).hexdigest(), fixtureLogBytes=len(data))
        except BaseException as error: result['postcheckError'] = str(error)
        try: save()
        except BaseException as error: result['receiptError'] = str(error)
    return result
