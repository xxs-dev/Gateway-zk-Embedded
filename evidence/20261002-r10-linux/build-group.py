"""Private compiler launcher for old util-linux. No CLI --kill-child dependency."""
import ctypes
import json
import os
from pathlib import Path
import signal
import sys
import time


def host_parent():
    # /proc is still mounted for the builder's host PID namespace.
    matches = [line.split()[1] for line in Path('/proc/self/status').read_text().splitlines() if line.startswith('PPid:')]
    assert len(matches) == 1
    return int(matches[0])


def parent_death(libc, expected_parent):
    assert libc.prctl(1, signal.SIGKILL, 0, 0, 0) == 0
    configured = ctypes.c_int()
    assert libc.prctl(2, ctypes.byref(configured), 0, 0, 0) == 0 and configured.value == signal.SIGKILL
    assert host_parent() == expected_parent, 'parent lost in PDEATHSIG setup window'


def main():
    argv = json.loads(sys.argv[1]); deadline_seconds = int(sys.argv[2]); expected_parent = int(sys.argv[3])
    assert argv and all(isinstance(value, str) for value in argv) and 1 <= deadline_seconds <= 1200
    libc = ctypes.CDLL(None, use_errno=True)
    parent_death(libc, expected_parent)
    launcher_host_pid = os.getpid()
    assert libc.unshare(0x20000000) == 0, 'kernel CLONE_NEWPID denied'
    child = os.fork()
    if child == 0:
        # fork clears PDEATHSIG. Restore it before proof/GO/exec; checking the
        # host-mounted proc PPid also closes the already-dead-parent window.
        parent_death(libc, launcher_host_pid)
        assert os.getpid() == 1
        signal.signal(signal.SIGALRM, lambda signum, frame: os._exit(124))
        signal.alarm(15)
        print(json.dumps(dict(pidNamespace=os.readlink('/proc/self/ns/pid'),
                              pdeathSignal=signal.SIGKILL, launcherHostPid=launcher_host_pid)), flush=True)
        assert sys.stdin.readline() == 'GO\n'
        signal.alarm(0)
        os.execvpe(argv[0], argv, os.environ)
    stopped = False
    def stop(signum, frame):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    until = time.monotonic() + deadline_seconds + 10
    try:
        while not stopped and time.monotonic() < until:
            info = os.waitid(os.P_PID, child, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if info is not None:
                # Leave the exact child unreaped for the outer subreaper.
                # Its PID cannot be reused before the final signal below.
                return info.si_status if info.si_code == os.CLD_EXITED else 128 + info.si_status
            time.sleep(0.01)
    finally:
        try: os.kill(child, signal.SIGKILL)
        except ProcessLookupError: pass
    until = time.monotonic() + 5
    while time.monotonic() < until:
        if os.waitid(os.P_PID, child, os.WEXITED | os.WNOHANG | os.WNOWAIT) is not None:
            return 124 if not stopped else 143
        time.sleep(0.01)
    print('private compiler PID1 cleanup unresolved', file=sys.stderr, flush=True)
    return 125


if __name__ == '__main__':
    raise SystemExit(main())
