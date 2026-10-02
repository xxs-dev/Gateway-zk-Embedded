"""L2 launcher adapted only for private mount/network/PID namespaces."""
import ctypes
import json
import os
from pathlib import Path
import signal
import sys
import time


def parent_death(libc, expected):
    assert libc.prctl(1, signal.SIGKILL, 0, 0, 0) == 0
    configured = ctypes.c_int()
    assert libc.prctl(2, ctypes.byref(configured), 0, 0, 0) == 0 and configured.value == signal.SIGKILL
    fields = [x.split()[1] for x in Path('/proc/self/status').read_text().splitlines() if x.startswith('PPid:')]
    assert fields == [str(expected)], 'parent lost before admission'


def main():
    command = json.loads(sys.argv[1]); parent = int(sys.argv[2])
    assert isinstance(command, list) and command and all(isinstance(x, str) for x in command)
    libc = ctypes.CDLL(None, use_errno=True); parent_death(libc, parent)
    leader = os.getpid()
    assert libc.unshare(0x20000000 | 0x20000 | 0x40000000) == 0, 'namespace capability/kernel gate denied'
    child = os.fork()
    if child == 0:
        parent_death(libc, leader); assert os.getpid() == 1
        signal.signal(signal.SIGALRM, lambda signum, frame: os._exit(124)); signal.alarm(15)
        proof = {n: os.readlink('/proc/self/ns/' + n) for n in ('pid', 'mnt', 'net')}
        print(json.dumps(dict(namespaceProof=proof, launcherHostPid=leader, pdeathSignal=9)), flush=True)
        assert sys.stdin.readline() == 'GO\n'
        signal.alarm(0); os.execvpe(command[0], command, os.environ)
    stopped = False
    def stop(signum, frame):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    deadline = time.monotonic() + 250
    try:
        while not stopped and time.monotonic() < deadline:
            info = os.waitid(os.P_PID, child, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if info is not None: return info.si_status if info.si_code == os.CLD_EXITED else 128 + info.si_status
            time.sleep(0.01)
    finally:
        try: os.kill(child, signal.SIGKILL)  # Exact unreaped child; no PID reuse.
        except ProcessLookupError: pass
    return 143 if stopped else 124


if __name__ == '__main__': raise SystemExit(main())
