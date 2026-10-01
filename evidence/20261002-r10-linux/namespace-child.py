"""PID1 private fixture boundary, NOT product recovery. Never called by dry-run."""
import ctypes
import errno
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time

LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p]


def mount(source, target, kind=None, flags=0, options=None):
    values = [v.encode() if v is not None else None for v in (source, str(target), kind)]
    assert LIBC.mount(*values, flags, options.encode() if options else None) == 0, 'mount boundary failed'


def sweep():
    # Only PID1 of this NEW namespace may enumerate/terminate its entire inventory.
    assert os.getpid() == 1
    rows = [int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and int(p.name) > 1]
    assert len(rows) <= 128, 'owned PID namespace inventory bound'
    for pid in rows:
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
    end = time.monotonic() + 3
    while time.monotonic() < end:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
            if pid == 0: time.sleep(0.01)
        except ChildProcessError: break
    remaining = [p.name for p in Path('/proc').iterdir() if p.name.isdigit() and int(p.name) > 1]
    return dict(outerRescuedPids=rows, remaining=remaining, productCleanupProved=not rows and not remaining)


def main():
    root = Path(sys.argv[1]); expected = json.loads(sys.argv[2])
    assert root.resolve() == root and os.getpid() == 1 and os.geteuid() == 0
    proof = {name: os.readlink('/proc/self/ns/' + name) for name in ('mnt', 'pid', 'net')}
    assert all(proof[name] != expected[name] for name in proof), 'namespace not isolated'
    print(json.dumps(dict(namespaceProof=proof)), flush=True)
    assert sys.stdin.readline() == 'GO\n', 'no parent ownership admission'
    mount(None, '/', flags=(1 << 18) | (1 << 14))  # MS_PRIVATE|MS_REC.
    fs = root / 'work/rootfs'; fs.mkdir()
    # Chroot only exposes read-only host runtime dependencies and fixed input.
    for name in ('usr', 'lib', 'lib64', 'bin', 'sbin', 'etc', 'sys'):
        original = Path('/') / name
        if original.exists():
            target = fs / name; target.mkdir()
            mount(str(original), target, flags=4096)
            mount(None, target, flags=4096 | 32 | 1 | 2 | 4)  # bind remount ro,nosuid,nodev.
    target = fs / 'input'; target.mkdir()
    mount(str(root / 'bin'), target, flags=4096)
    mount(None, target, flags=4096 | 32 | 1 | 2 | 4)
    for name, options in (('tmp', 'size=128m,mode=1777'), ('run', 'size=16m,mode=0755'),
                          ('dev', 'size=1m,mode=0755')):
        target = fs / name; target.mkdir(); mount('tmpfs', target, 'tmpfs', 2 | 4, options)
    (fs / 'dev/shm').mkdir(); mount('tmpfs', fs / 'dev/shm', 'tmpfs', 2 | 4, 'size=128m,mode=1777')
    for name, major, minor in (('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9)):
        os.mknod(str(fs / 'dev' / name), 0o20666, os.makedev(major, minor))
    # The /dev tmpfs must allow only these explicit character devices.
    mount(None, fs / 'dev', flags=32 | 2, options='size=1m,mode=0755')
    (fs / 'proc').mkdir(); mount('proc', fs / 'proc', 'proc', 2 | 4 | 8)
    os.chroot(str(fs)); os.chdir('/tmp')
    assert os.getpid() == 1 and Path('/dev/shm').stat().st_dev != expected['shmDev']
    assert not Path('/opt').exists() and not Path('/srv').exists()
    # No fixture can mount, gain capabilities on exec, or escape the private root.
    assert LIBC.prctl(38, 1, 0, 0, 0) == 0
    for cap in range(64):
        status = LIBC.prctl(24, cap, 0, 0, 0)
        assert status == 0 or ctypes.get_errno() == errno.EINVAL
    class Header(ctypes.Structure):
        _fields_ = [('version', ctypes.c_uint32), ('pid', ctypes.c_int)]
    class Data(ctypes.Structure):
        _fields_ = [('effective', ctypes.c_uint32), ('permitted', ctypes.c_uint32), ('inheritable', ctypes.c_uint32)]
    header = Header(0x20080522, 0); caps = (Data * 2)()
    assert LIBC.capset(ctypes.byref(header), ctypes.byref(caps)) == 0
    commands = [('diagnostic_process_test', [], 30), ('system_monitor_diagnostic_test', [], 30),
                ('system_monitor_service_test', [], 60), ('ownership-probe', ['fd'], 5),
                ('ownership-probe', ['parent-death'], 5), ('ownership-probe', ['supervisor-death'], 5)]
    result = dict(privateFixtureOnly=True, commands=[], productAcceptance=False)
    def limits():
        for key, value in ((resource.RLIMIT_AS, 512 * 1024**2), (resource.RLIMIT_CPU, 20),
                           (resource.RLIMIT_FSIZE, 1024**2), (resource.RLIMIT_NOFILE, 256),
                           (resource.RLIMIT_CORE, 0)):
            resource.setrlimit(key, (value, value))
    try:
        for index, (name, args, seconds) in enumerate(commands):
            argv = ['/input/' + name] + args
            path = Path('/tmp/case-%d.log' % index)
            started = time.monotonic()
            with path.open('xb') as log:
                child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=dict(PATH='/usr/bin:/bin', LC_ALL='C', TMPDIR='/tmp'), preexec_fn=limits)
                try: code = child.wait(timeout=seconds)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait(timeout=3); code = 124
            assert path.stat().st_size <= 1024**2
            cleanup = sweep()
            result['commands'].append(dict(argv=argv, exitCode=code, elapsedSeconds=time.monotonic() - started,
                                            stdout=path.read_text(errors='replace'), cleanup=cleanup))
            if code != 0 or not cleanup['productCleanupProved']:
                result['stoppedOnFailure'] = True; break
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error), cleanup=sweep())
    print(json.dumps(result), flush=True)
    return 0 if len(result['commands']) == 6 and all(row['exitCode'] == 0 and row['cleanup']['productCleanupProved'] for row in result['commands']) and 'errorType' not in result else 1


if __name__ == '__main__':
    raise SystemExit(main())
