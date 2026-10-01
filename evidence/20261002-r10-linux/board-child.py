"""23-only namespace launcher; fixed B2 business baseline provided by caller."""
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
import time


def run(root, manifest, baseline, expected_baseline):
    root = Path(root); assert str(root) == manifest['boardRoot'] and root.resolve() == root
    assert os.geteuid() == 0 and root.stat().st_uid == 0 and stat.S_IMODE(root.stat().st_mode) == 0o700
    result = dict(privateFixtureOnly=True, realLifecycleCalled=False, sourceCommit=manifest['sourceCommit'])
    child = None; proof = None; work = root / 'work'; identity = None
    keys = ('machineCode', 'bootId', 'stateDir', 'stateSha256', 'approvalSha256', 'activeSha256',
            'guardSha256', 'configSha256', 'markerSha256', 'readers', 'runtimeProcesses', 'segments',
            'offlineLock', 'segmentIdentities', 'readerStartTicks', 'readerShmMaps')
    def save():
        with (root / 'result.json').open('w') as output:
            json.dump(result, output, sort_keys=True, indent=2); output.flush(); os.fsync(output.fileno())
    def namespace_members():
        if proof is None: return []
        rows = []; count = 0
        for item in Path('/proc').iterdir():
            if not item.name.isdigit(): continue
            count += 1; assert count <= 32768
            try:
                if os.readlink(str(item / 'ns/pid')) == proof['pid']: rows.append(int(item.name))
            except (FileNotFoundError, ProcessLookupError): pass
        return rows
    def cleanup():
        if child is not None and child.poll() is None:
            child.kill(); child.wait(timeout=5)  # unshare --kill-child=KILL closes its namespace PID1.
        end = time.monotonic() + 5
        while time.monotonic() < end:
            try:
                got, _ = os.waitpid(-1, os.WNOHANG)
                if got == 0: time.sleep(0.01)
            except ChildProcessError: break
        remaining = namespace_members(); assert not remaining, 'owned namespace survivors; preserve work'
        return remaining
    try:
        for name, row in manifest['boardFiles'].items():
            path = root / row['directory'] / name; info = path.lstat()
            assert stat.S_ISREG(info.st_mode) and not path.is_symlink() and info.st_uid == 0
            assert stat.S_IMODE(info.st_mode) == row['mode'] and info.st_size == row['bytes']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256']
        result['before'] = baseline(); save()
        assert all(result['before']['snapshot'][key] == expected_baseline['snapshot'][key] for key in keys)
        assert result['before']['parentNamespace'] == expected_baseline['parentNamespace'], 'upload/preflight drift'
        assert os.statvfs('/tmp').f_bavail * os.statvfs('/tmp').f_frsize >= 1024**3
        assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0
        helptext = subprocess.check_output(['/usr/bin/unshare', '--help'], timeout=5)
        assert len(helptext) <= 65536 and b'--kill-child' in helptext, 'required unshare semantics absent'
        work.mkdir(mode=0o700); info = work.stat(); identity = (info.st_dev, info.st_ino)
        expected = {name: os.readlink('/proc/self/ns/' + name) for name in ('mnt', 'pid', 'net')}
        expected['shmDev'] = os.stat('/dev/shm').st_dev
        argv = ['/usr/bin/unshare', '--mount', '--pid', '--net', '--fork', '--kill-child=KILL',
                '/usr/bin/python3', '-I', '-B', str(root / 'source/namespace-child.py'), str(root), json.dumps(expected)]
        parent = os.getpid()
        def parent_death():
            assert ctypes.CDLL(None).prctl(1, signal.SIGKILL, 0, 0, 0) == 0
            assert os.getppid() == parent
        child = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 env=dict(PATH='/usr/bin:/bin', LC_ALL='C'), start_new_session=True, preexec_fn=parent_death)
        result['outerPid'] = child.pid; result['argv'] = argv; save()
        selector = selectors.DefaultSelector(); selector.register(child.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + 240; startup = time.monotonic() + 15; output = bytearray(); admitted = False
        with (root / 'fixture.log').open('xb') as log:
            while True:
                assert time.monotonic() < (deadline if admitted else startup), 'fixture outer deadline'
                for _, _ in selector.select(0.1):
                    chunk = os.read(child.stdout.fileno(), 4096)
                    if not chunk: break
                    output.extend(chunk); log.write(chunk); log.flush()
                    assert len(output) <= 1024**2, 'fixture output bound'
                    if not admitted and b'\n' in output:
                        proof = json.loads(output.split(b'\n', 1)[0])['namespaceProof']
                        children = Path('/proc/%d/task/%d/children' % (child.pid, child.pid)).read_text().split()
                        assert len(children) == 1
                        init = children[0]
                        assert all(os.readlink('/proc/' + init + '/ns/' + name) == proof[name] != expected[name] for name in ('mnt', 'pid', 'net'))
                        result['ownedNamespace'] = dict(initHostPid=int(init), proof=proof); save()
                        child.stdin.write(b'GO\n'); child.stdin.flush(); child.stdin.close(); admitted = True
                if child.poll() is not None:
                    tail = child.stdout.read(1024**2 + 1 - len(output)); output.extend(tail); log.write(tail)
                    assert len(output) <= 1024**2; break
        result['fixtureExitCode'] = child.returncode
        result['fixtureOutput'] = output.decode('utf-8', 'replace')
        result['fixtureLogSha256'] = hashlib.sha256(output).hexdigest()
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error))
    finally:
        try: result['remainingNamespaceProcesses'] = cleanup()
        except BaseException as error: result['cleanupError'] = str(error)
        if identity and result.get('remainingNamespaceProcesses') == []:
            current = work.lstat()
            assert not work.is_symlink() and (current.st_dev, current.st_ino) == identity
            assert shutil.rmtree.avoids_symlink_attacks
            shutil.rmtree(str(work)); result['workRemoved'] = not work.exists()
        # Business and parent namespace proof is sampled AFTER owned-work cleanup.
        try:
            result['afterCleanup'] = baseline()
            before = result.get('before')
            assert before is not None
            result['baselineChanges'] = [key for key in keys if before['snapshot'][key] != result['afterCleanup']['snapshot'][key]]
            if before['parentNamespace'] != result['afterCleanup']['parentNamespace']: result['baselineChanges'].append('parentNamespace')
            result['postHashes'] = {}
            for name, row in manifest['boardFiles'].items():
                path = root / row['directory'] / name
                result['postHashes'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
                assert result['postHashes'][name] == row['sha256']
        except BaseException as error: result['postcheckError'] = str(error)
        save()
    return result
