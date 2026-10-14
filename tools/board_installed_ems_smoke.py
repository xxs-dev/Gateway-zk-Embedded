#!/usr/bin/env python3
"""Bounded installed-program smoke after an isolated, zero-capability preflight.

This does not activate production units or grant hardware access. Data/run are
private bind mounts supplied by the systemd harness. No acquisition is simulated.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import uuid

import board_control_isolation_probe as isolation

HOME = Path('/opt/modbus-gateway')
COMMANDS = (
    ('monitor', [str(HOME / 'bin/SystemMonitor'), '--app-config',
                 str(HOME / 'config/runtime/apps/monitor-service.json')]),
    ('bridge', [str(HOME / 'bin/QtDisplayBridge'), '--app-config',
                str(HOME / 'config/runtime/apps/mqtt-service.json'), '--var-list',
                str(HOME / 'ky-ems/VarList.xml'), '--interval-ms', '500']),
    ('ems', ['/bin/sh', str(HOME / 'bin/gateway-qt-run.sh')]),
)


def commands(mode):
    isolation.require(mode in ('legacy-bridge', 'native-direct-probe'), 'unknown display probe')
    return tuple(row for row in COMMANDS if mode == 'legacy-bridge' or row[0] != 'bridge')


def check_sysv(text, mode):
    rows = text.splitlines()[1:]
    if mode == 'legacy-bridge':
        isolation.require(any(int(line.split()[0]) == 0x1234 for line in rows),
                          'bridge did not create its private legacy segment')
    else:
        isolation.require(not rows, 'native display created unexpected legacy shared memory')


def sha(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            result.update(block)
    return result.hexdigest()


def check_image(approval, generated_varlist=False):
    rows = approval['imageTree']
    for name, pin in rows.items():
        if generated_varlist and name == 'ky-ems/VarList.xml':
            continue
        if name.split('/')[0] not in ('bin', 'config', 'ky-ems', 'scada'):
            continue
        path = HOME / name
        if pin['kind'] == 'file':
            isolation.require(not path.is_symlink() and path.is_file() and
                              sha(path) == pin['sha256'], 'installed image drift: ' + name)
        elif pin['kind'] == 'symlink':
            isolation.require(path.is_symlink() and os.readlink(str(path)) == pin['target'],
                              'installed link drift: ' + name)


def probe_posix_shm(expected_dev):
    runtime = ctypes.CDLL('librt.so.1', use_errno=True)
    runtime.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    runtime.shm_open.restype = ctypes.c_int
    runtime.shm_unlink.argtypes = [ctypes.c_char_p]
    name = ('/gateway_smoke_' + uuid.uuid4().hex).encode()
    fd = runtime.shm_open(name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    if fd < 0:
        raise OSError(ctypes.get_errno(), 'private POSIX shm_open failed')
    try:
        isolation.require(os.fstat(fd).st_dev == expected_dev,
                          'libc selected a different shared-memory backing')
    finally:
        os.close(fd)
        isolation.require(runtime.shm_unlink(name) == 0, 'private POSIX probe cleanup failed')


def snapshot(children, args):
    result = {}
    for name, process in children:
        isolation.require(process.poll() is None, 'installed program exited: ' + name)
        root = Path('/proc') / str(process.pid)
        security = isolation.check_status((root / 'status').read_text())
        isolation.check_namespace(os.readlink(str(root / 'ns/net')), args.host_netns,
                                  args.shared_netns, 'network')
        isolation.check_namespace(os.readlink(str(root / 'ns/ipc')), args.host_ipcns,
                                  args.shared_ipcns, 'IPC')
        result[name] = {'pid': process.pid, 'security': security,
                        'executable': os.readlink(str(root / 'exe'))}
    return result


def terminate(children):
    errors = []
    for name, process in reversed(children):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    for name, process in reversed(children):
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
            errors.append(name + ' required SIGKILL')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host-netns', 'shared-netns', 'host-ipcns', 'shared-ipcns',
                 'shm-marker', 'marker-sha256', 'approval-sha256'):
        parser.add_argument('--' + name, required=True)
    for name in ('marker-dev', 'marker-ino'):
        parser.add_argument('--' + name, type=int, required=True)
    parser.add_argument('--approval', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=45)
    parser.add_argument('--display-probe', choices=('legacy-bridge', 'native-direct-probe'),
                        default='legacy-bridge')
    args = parser.parse_args()
    isolation.require(10 <= args.seconds <= 120, 'bounded smoke interval required')
    proof = isolation.probe(args)
    probe_posix_shm(args.marker_dev)
    isolation.require(sha(args.approval) == args.approval_sha256, 'approval digest mismatch')
    approval = json.loads(args.approval.read_text())
    isolation.require(approval['gatewayHome'] == str(HOME) and approval['controlEnabled'] is False,
                      'unexpected installation approval')
    check_image(approval)
    for name in ('data', 'run'):
        marker = HOME / name / 'isolated-smoke-marker'
        isolation.require(marker.read_text().strip() == args.approval_sha256,
                          'private runtime backing not proved')
    for path in (Path('/dev/shm'), HOME / 'data', HOME / 'run', args.evidence):
        with tempfile.NamedTemporaryFile(prefix='.isolated-smoke-write-', dir=str(path)) as stream:
            stream.write(b'private writable backing probe\n')
            stream.flush()
            os.fsync(stream.fileno())
    args.evidence.mkdir(exist_ok=True)
    (args.evidence / 'isolation.json').write_text(json.dumps(proof, indent=2) + '\n')
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR='/tmp/gateway-ems-smoke')
    Path(env['XDG_RUNTIME_DIR']).mkdir(mode=0o700, exist_ok=True)
    children, streams, observations = [], [], []
    failure = None
    # PID is also the new process-group leader so every owned child is retired.
    try:
        for name, command in commands(args.display_probe):
            stream = (args.evidence / (name + '.log')).open('wb')
            streams.append(stream)
            children.append((name, subprocess.Popen(command, cwd=str(HOME), env=env,
                              stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)))
            time.sleep(2)
            snapshot(children, args)
        begin = time.monotonic()
        while time.monotonic() - begin < args.seconds:
            observations.append(snapshot(children, args))
            time.sleep(1)
        legacy = Path('/proc/sysvipc/shm').read_text()
        check_sysv(legacy, args.display_probe)
        (args.evidence / 'private-sysv-shm.txt').write_text(legacy)
        generated = HOME / 'ky-ems/VarList.xml'
        (args.evidence / 'generated-varlist.xml').write_bytes(generated.read_bytes())
        check_image(approval, generated_varlist=args.display_probe == 'legacy-bridge')
    except BaseException as error:
        failure = repr(error)
    finally:
        stop_errors = terminate(children)
        stop_errors.extend(name + ' exited with code ' + str(process.returncode)
                           for name, process in children if process.returncode != 0)
        try:
            check_image(approval, generated_varlist=args.display_probe == 'legacy-bridge')
        except BaseException as error:
            failure = failure or repr(error)
        for stream in streams:
            stream.close()
        result = {'format': 'installed-ems-isolated-smoke-v2', 'approvalSha256': args.approval_sha256,
                  'displayProbe': args.display_probe,
                  'seconds': args.seconds, 'observations': observations, 'failure': failure,
                  'stopErrors': stop_errors, 'physicalControlEnabled': False,
                  'exitCodes': {name: process.returncode for name, process in children},
                  'generatedVarList': {'originalSha256': approval['imageTree']['ky-ems/VarList.xml']['sha256'],
                                       'runtimeSha256': sha(HOME / 'ky-ems/VarList.xml')},
                  'productionActivationAccepted': False}
        (args.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    isolation.require(not failure and not stop_errors, 'smoke failed: ' + str(failure or stop_errors))
    print('INSTALLED_EMS_ISOLATED_SMOKE_PASS ' + args.display_probe)


if __name__ == '__main__':
    main()
