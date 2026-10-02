"""Private PID1 fixture boundary; outer rescue is never product cleanup."""
import ctypes
import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

COMMANDS = [('diagnostic_process_test', [], 30), ('system_monitor_diagnostic_test', [], 30),
            ('system_monitor_service_test', [], 60), ('ownership-probe', ['fd'], 5),
            ('ownership-probe', ['parent-death'], 5), ('ownership-probe', ['supervisor-death'], 5)]
V11_BYTES = 15667032  # Archived B2 native size; layout/constructor blobs equal cb45.
CASE_BUDGETS = {
    'diagnostic_process_test': dict(fileBytes=1024**2, shmSegments=0, largestFixtureFileBytes=128*1024+3),
    'system_monitor_diagnostic_test': dict(fileBytes=1024**2, shmSegments=0, largestFixtureFileBytes=1024**2),
    'system_monitor_service_test': dict(fileBytes=32*1024**2, shmSegments=3, largestFixtureFileBytes=5*1024**2+1),
    'ownership-probe': dict(fileBytes=1024**2, shmSegments=0, largestFixtureFileBytes=0),
}
for budget in CASE_BUDGETS.values():
    budget.update(addressSpaceBytes=512*1024**2, cpuSeconds=20, logBytes=1024**2)


def check_budgets():
    for budget in CASE_BUDGETS.values():
        assert budget['fileBytes'] <= 32*1024**2
        assert budget['fileBytes'] >= max(budget['largestFixtureFileBytes'], V11_BYTES if budget['shmSegments'] else 0)
        assert budget['shmSegments']*V11_BYTES+16*1024**2 <= 128*1024**2
        # Source-directed headroom, not a measured RSS upper bound.
        assert budget['shmSegments']*V11_BYTES+256*1024**2 <= budget['addressSpaceBytes']


def sweep():
    assert os.getpid() == 1
    rows = [int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and int(p.name) > 1]
    assert len(rows) <= 128, 'private process count bound'
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
    import resource
    check_budgets()
    root = Path(sys.argv[1]); expected = json.loads(sys.argv[2])
    assert root.resolve() == root and os.getpid() == 1 and os.geteuid() == 0
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_char_p]
    def mount(source, target, kind=None, flags=0, options=None):
        values = [v.encode() if v is not None else None for v in (source, str(target), kind)]
        assert libc.mount(*values, flags, options.encode() if options else None) == 0, 'private mount failed'
    proof = {n: os.readlink('/proc/self/ns/' + n) for n in ('pid', 'mnt', 'net')}
    assert all(proof[n] != expected[n] for n in proof)
    mount(None, '/', flags=(1 << 18) | (1 << 14))  # Private propagation before any new mount.
    fs = root / 'work/rootfs'; fs.mkdir()
    for name in ('usr', 'lib', 'lib64', 'bin'):
        original = Path('/') / name
        if original.exists():
            target = fs / name; target.mkdir()
            mount(str(original), target, flags=4096)
            mount(None, target, flags=4096 | 32 | 1 | 2 | 4)
    (fs / 'etc').mkdir()
    cache = Path('/etc/ld.so.cache')
    if cache.exists():
        assert cache.is_file() and not cache.is_symlink()
        target = fs / 'etc/ld.so.cache'; target.touch()
        mount(str(cache), target, flags=4096); mount(None, target, flags=4096 | 32 | 1 | 2 | 4)
    target = fs / 'input'; target.mkdir()
    mount(str(root / 'bin'), target, flags=4096); mount(None, target, flags=4096 | 32 | 1 | 2 | 4)
    for name, options in (('tmp', 'size=128m,mode=1777'), ('run', 'size=16m,mode=0755'), ('dev', 'size=1m,mode=0755')):
        target = fs / name; target.mkdir(); mount('tmpfs', target, 'tmpfs', 2 | 4, options)
    (fs / 'dev/shm').mkdir(); mount('tmpfs', fs / 'dev/shm', 'tmpfs', 2 | 4, 'size=128m,mode=1777')
    for name, major, minor in (('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9)):
        os.mknod(str(fs / 'dev' / name), 0o20666, os.makedev(major, minor))
    mount(None, fs / 'dev', flags=32 | 2, options='size=1m,mode=0755')
    (fs / 'proc').mkdir(); mount('proc', fs / 'proc', 'proc', 1 | 2 | 4 | 8)
    os.chroot(str(fs)); os.chdir('/tmp')
    for row in expected['runtimeMetadata']:
        info = Path(row['path']).stat()
        assert info.st_uid == 0 and not info.st_mode & 0o6022
        assert (info.st_dev, info.st_ino, info.st_size, info.st_mode & 0o777) == (row['dev'], row['ino'], row['bytes'], row['mode']), 'bound runtime identity drift'
    assert Path('/dev/shm').stat().st_dev != expected['shmDev']
    assert not any(Path('/' + n).exists() for n in ('opt', 'srv', 'sys'))
    assert libc.prctl(38, 1, 0, 0, 0) == 0
    for cap in range(64):
        ctypes.set_errno(0); status = libc.prctl(24, cap, 0, 0, 0)
        assert status == 0 or ctypes.get_errno() == errno.EINVAL
    class Header(ctypes.Structure):
        _fields_ = [('version', ctypes.c_uint32), ('pid', ctypes.c_int)]
    class Data(ctypes.Structure):
        _fields_ = [('effective', ctypes.c_uint32), ('permitted', ctypes.c_uint32), ('inheritable', ctypes.c_uint32)]
    header = Header(0x20080522, 0); caps = (Data * 2)()
    assert libc.capset(ctypes.byref(header), ctypes.byref(caps)) == 0
    status = dict(x.split(':', 1) for x in Path('/proc/self/status').read_text().splitlines() if ':' in x)
    assert all(int(status[n].strip(), 16) == 0 for n in ('CapEff', 'CapPrm', 'CapInh', 'CapBnd', 'CapAmb'))
    assert status['NoNewPrivs'].strip() == '1'
    print(json.dumps(dict(privateRootProof=proof, capabilitiesDropped=True, noNewPrivs=True,
                          shmDevice=Path('/dev/shm').stat().st_dev, hostBusinessPathsVisible=False)), flush=True)
    result = dict(privateFixtureOnly=True, commands=[], configuredCommands=COMMANDS)
    try:
        for index, (name, args, seconds) in enumerate(COMMANDS):
            argv = ['/input/' + name] + args; path = Path('/tmp/case-%d.log' % index)
            budget = CASE_BUDGETS[name]
            def limits():
                for key, value in ((resource.RLIMIT_AS, budget['addressSpaceBytes']), (resource.RLIMIT_CPU, budget['cpuSeconds']),
                                   (resource.RLIMIT_FSIZE, budget['fileBytes']), (resource.RLIMIT_NOFILE, 256), (resource.RLIMIT_CORE, 0)):
                    resource.setrlimit(key, (value, value))
            started = time.monotonic()
            with path.open('xb') as log:
                child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=dict(PATH='/usr/bin:/bin', LC_ALL='C', TMPDIR='/tmp'), preexec_fn=limits)
                try: code = child.wait(timeout=seconds)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait(timeout=3); code = 124
            assert path.stat().st_size <= budget['logBytes']
            cleanup = sweep()
            result['commands'].append(dict(argv=argv, exitCode=code, elapsedSeconds=time.monotonic()-started,
                                           stdout=path.read_text(errors='replace'), cleanup=cleanup))
            if code != 0 or not cleanup['productCleanupProved']:
                result['stoppedOnFailure'] = True; break
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error), cleanup=sweep())
    print(json.dumps(dict(fixtureResult=result)), flush=True)
    return 0 if len(result['commands']) == 6 and all(x['exitCode'] == 0 and x['cleanup']['productCleanupProved'] for x in result['commands']) and 'errorType' not in result else 1


if __name__ == '__main__': raise SystemExit(main())
