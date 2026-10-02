"""B2 PID1 fixture: distinguish natural adoption from harness rescue."""
import hashlib
import os
from pathlib import Path
import signal
import time

BASE = Path('/tmp/gw-r10-eb6cc53-23-B2/source/namespace-child-base.py')
BASE_SHA256 = '1c3ef7f98dd66226c9d617c945e90b20964ecf7c6010c33c7c9d90f81365b158'


def wait_status(pid, status):
    row = dict(pid=pid, rawStatus=status)
    if os.WIFEXITED(status):
        row.update(kind='exited', exitCode=os.WEXITSTATUS(status))
    elif os.WIFSIGNALED(status):
        row.update(kind='signaled', signal=os.WTERMSIG(status))
    else:
        row['kind'] = 'other'
    return row


def children_snapshot():
    namespace = os.readlink('/proc/self/ns/pid')
    rows = []
    for path in Path('/proc').iterdir():
        if not path.name.isdigit() or int(path.name) <= 1:
            continue
        assert len(rows) < 128, 'private process count bound'
        pid = int(path.name)
        raw = (path / 'status').read_bytes()
        assert len(raw) <= 8192, 'private process status bound'
        fields = dict(line.split(b':', 1) for line in raw.splitlines() if b':' in line)
        assert fields[b'Pid'].split() == [str(pid).encode()]
        assert int(fields[b'NSpid'].split()[-1]) == pid
        assert os.readlink(str(path / 'ns/pid')) == namespace
        rows.append(dict(pid=pid, ppid=int(fields[b'PPid'].split()[0]),
                         state=fields[b'State'].split()[0].decode('ascii'), namespace=namespace))
    return rows


def sweep():
    assert os.getpid() == 1, 'private PID1 required'
    assert os.readlink('/proc/1/ns/pid') == os.readlink('/proc/self/ns/pid')
    natural = []
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            break
        if pid == 0:
            time.sleep(0.01)
            continue
        assert len(natural) < 128, 'natural adoption bound'
        natural.append(wait_status(pid, status))
    live_or_unreaped = children_snapshot()
    rescued = []
    for row in live_or_unreaped:
        try:
            os.kill(row['pid'], signal.SIGKILL)
            rescued.append(dict(row, signalSent=True))
        except ProcessLookupError:
            rescued.append(dict(row, signalSent=False, disappearedBeforeSignal=True))
    rescue_reaped = []
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            break
        if pid == 0:
            time.sleep(0.01)
            continue
        assert len(rescue_reaped) < 128, 'rescue reaping bound'
        rescue_reaped.append(wait_status(pid, status))
    remaining = children_snapshot()
    return dict(naturalReaped=natural, outerRescuedPids=[row['pid'] for row in rescued],
                rescueActions=rescued, rescueReaped=rescue_reaped, remaining=remaining,
                productCleanupProved=not rescued and not remaining)


def run_base():
    base = BASE.read_bytes()
    assert hashlib.sha256(base).hexdigest() == BASE_SHA256
    namespace = {'__name__': 'r10_b2_namespace_base'}
    exec(compile(base, 'namespace-child-base.py', 'exec'), namespace)
    namespace['sweep'] = sweep
    return namespace['main']()


if __name__ == '__main__':
    raise SystemExit(run_base())
