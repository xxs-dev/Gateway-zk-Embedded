#!/usr/bin/env python3
"""Dedicated X11 session and gated KY-EMS availability monitor (no desktop)."""
import argparse
import json
import os
from pathlib import Path
import pwd
import signal
import stat
import subprocess
import time


SESSION_NAME = 'gateway-ems-kiosk'
MARKER = 'gateway-ems-kiosk.json'
UNIT = 'ky-ems.service'
HELPER = '/usr/local/libexec/gateway-ems-kiosk.py'
INHIBIT_PATHS = (
    '/run/gateway-health-watchdog/manual-stop',
    '/run/gateway-health-watchdog/applying',
    '/opt/gateway-reprovision/pending.json',
    '/opt/gateway-runtime-bootstrap/pending.json',
    '/opt/gateway-runtime-bootstrap/activations/pending.json',
    '/opt/modbus-gateway/data/runtime-production/pending.json',
    '/opt/modbus-gateway/ota/staging/ota-in-progress',
)


def command(argv, timeout=10, env=None):
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=timeout, env=env)


def session():
    uid = os.getuid()
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR', '/nonexistent'))
    if uid == 0 or str(runtime) != '/run/user/' + str(uid):
        raise ValueError('kiosk session requires an unprivileged GDM user')
    if os.environ.get('DISPLAY') != ':0':
        raise ValueError('the approved native runtime requires DISPLAY=:0')
    authority = os.environ.get('XAUTHORITY')
    if authority != str(runtime / 'gdm/Xauthority') or not Path(authority).is_file():
        raise ValueError('unexpected GDM Xauthority')
    for args in (['xset', 's', 'off'], ['xset', 's', 'noblank'],
                 ['xsetroot', '-solid', 'black']):
        result = command(args)
        if result.returncode:
            raise RuntimeError(result.stderr)
    # Some framebuffer drivers have no DPMS extension. The Xorg flags and
    # screen-saver timeout still prevent blanking on these boards.
    result = command(['xset', '-dpms'])
    if result.returncode:
        print('DPMS extension unavailable: ' + result.stderr.strip(), flush=True)
    marker = runtime / MARKER
    value = dict(pid=os.getpid(), uid=uid, display=':0', authority=authority,
                 bootId=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    fd = os.open(str(marker), os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream)
    stopping = False

    def stop(_signum, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while not stopping:
            time.sleep(1)
    finally:
        if marker.is_file() and json.loads(marker.read_text()).get('pid') == os.getpid():
            marker.unlink()


def display_environment(user):
    account = pwd.getpwnam(user)
    if account.pw_uid == 0:
        raise ValueError('root display sessions are not supported')
    root = Path('/run/user') / str(account.pw_uid)
    marker = root / MARKER
    info = marker.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != account.pw_uid or info.st_size > 2048:
        raise ValueError('invalid kiosk readiness marker')
    data = json.loads(marker.read_text())
    pid = data['pid']
    if type(pid) is not int or pid <= 1 or data['uid'] != account.pw_uid:
        raise ValueError('invalid session process')
    proc = Path('/proc') / str(pid)
    if proc.stat().st_uid != account.pw_uid:
        raise ValueError('session process owner mismatch')
    argv = (proc / 'cmdline').read_bytes().split(b'\0')
    if HELPER.encode() not in argv or b'session' not in argv:
        raise ValueError('readiness marker does not belong to the kiosk session')
    if (data['bootId'] != Path('/proc/sys/kernel/random/boot_id').read_text().strip() or
            data['display'] != ':0' or data['authority'] != str(root / 'gdm/Xauthority')):
        raise ValueError('stale or unexpected display session')
    auth = Path(data['authority']).lstat()
    if not stat.S_ISREG(auth.st_mode) or auth.st_uid != account.pw_uid or auth.st_mode & 0o077:
        raise ValueError('unsafe Xauthority ownership or mode')
    env = dict(os.environ, DISPLAY=data['display'], XAUTHORITY=data['authority'])
    result = command(['xset', 'q'], env=env)
    if result.returncode:
        raise RuntimeError('dedicated X11 display is not ready')
    result = command(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'], env=env)
    if result.returncode or 'window id #' in result.stdout:
        raise RuntimeError('unexpected desktop/window manager in kiosk session')
    return env


def wait_ready(user, timeout):
    deadline = time.monotonic() + timeout
    error = 'no session'
    while time.monotonic() < deadline:
        try:
            display_environment(user)
            print('Dedicated EMS X11 session ready', flush=True)
            return
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
            error = str(exc)
            time.sleep(1)
    raise RuntimeError('kiosk readiness timed out: ' + error)


def needs_start(state):
    return state.get('ActiveState') in ('inactive', 'failed')


def maintenance_inhibited():
    # Reprovision's STOP is retained as history after first activation. The
    # production-unit guard remains authoritative for its effective fences.
    for name in INHIBIT_PATHS:
        path = Path(name)
        if path.exists() or path.is_symlink():
            return True
    result = command(['systemctl', 'show', 'gateway-services.service',
                      '-p', 'ActiveState', '-p', 'SubState'])
    state = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    return result.returncode != 0 or state != {'ActiveState': 'active', 'SubState': 'exited'}


def watch(user):
    while True:
        try:
            if maintenance_inhibited():
                time.sleep(3)
                continue
            display_environment(user)
            result = command(['systemctl', 'show', UNIT, '-p', 'ActiveState', '-p', 'SubState'])
            if result.returncode:
                raise RuntimeError(result.stderr)
            state = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
            if needs_start(state):
                # Go through the existing production-unit guard. Never reset
                # gates, run the binary directly, or launch a second instance.
                print('Requesting gated EMS service start: ' + str(state), flush=True)
                result = command(['systemctl', 'start', UNIT], timeout=120)
                if result.returncode:
                    print(result.stderr.strip(), flush=True)
                time.sleep(30)
            else:
                time.sleep(3)
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
            print('Kiosk waiting: ' + str(exc), flush=True)
            time.sleep(10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('session', 'ready', 'watch'))
    parser.add_argument('--user', default='szrl')
    parser.add_argument('--timeout', type=int, default=75)
    args = parser.parse_args()
    if args.action == 'session':
        session()
    elif args.action == 'ready':
        wait_ready(args.user, args.timeout)
    else:
        watch(args.user)


if __name__ == '__main__':
    main()
