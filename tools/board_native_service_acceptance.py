#!/usr/bin/env python3
"""Installed native EMS and Modbus service fixture, never production activation.

Run only with an externally pinned stopped-reprovision approval and a prepared
private /srv directory. All business processes get isolated network, IPC,
devices and SHM, zero capabilities, immutable binaries and test-only config.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import time
import urllib.request

import board_control_isolation_probe as isolation

HOME = Path('/opt/modbus-gateway')
PROBE_INDEXES = (910002, 920002, 930002)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    path.chmod(0o600)


def point_rows(payload):
    if not isinstance(payload, dict) or not isinstance(payload.get('points'), list):
        raise ValueError('missing point array')
    rows = {}
    for row in payload['points']:
        if not isinstance(row, dict) or type(row.get('index')) is not int or row['index'] in rows:
            raise ValueError('invalid/duplicate point index')
        rows[row['index']] = row
    return rows


def healthy(row, now_ms):
    return (row.get('quality') == 1 and row.get('stale') is False and
            type(row.get('ts')) is int and 0 <= now_ms - row['ts'] < 3000)


def commands(root):
    return {
        'simulator': [str(root / 'gateway-test-lab-sim'), 'serve', '--state-file',
                      str(root / 'data/simulator.state'), '--bind', '127.0.0.1', '--port', '25020'],
        'driver': [str(HOME / 'bin/ModbusRtu'), '--config',
                   str(HOME / 'config/runtime/devices/device_sim_modbus.json')],
        'monitor': [str(HOME / 'bin/SystemMonitor'), '--app-config',
                    str(HOME / 'config/runtime/apps/monitor-service.json')],
        'ui': ['/bin/sh', str(HOME / 'bin/gateway-qt-native-run.sh')],
    }


def check_scope(root, host_net, shared_net, host_ipc, shared_ipc):
    isolation.check_namespace(os.readlink('/proc/self/ns/net'), host_net, shared_net, 'network')
    isolation.check_namespace(os.readlink('/proc/self/ns/ipc'), host_ipc, shared_ipc, 'IPC')
    security = isolation.check_status(Path('/proc/self/status').read_text())
    isolation.check_mounts(isolation.mount_rows(Path('/proc/self/mountinfo').read_text()))
    links = json.loads(subprocess.check_output(['ip', '-j', 'link', 'show']))
    routes = []
    for family in ('-4', '-6'):
        routes.extend(json.loads(subprocess.check_output(['ip', '-j', family, 'route', 'show', 'table', 'all'])))
    isolation.check_links(links, routes)
    marker = Path('/dev/shm/gateway-native-service-marker')
    expected = json.loads((root / 'marker.json').read_text())
    value = marker.stat()
    assert {'dev': value.st_dev, 'ino': value.st_ino, 'sha256': sha(marker)} == expected
    assert len(Path('/proc/sysvipc/shm').read_text().splitlines()) == 1
    for path in ('/run/systemd/private', '/run/dbus/system_bus_socket'):
        assert not os.path.lexists(path)
    denied = {'can': isolation.blocked_socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW),
              'packet': isolation.blocked_socket(socket.AF_PACKET, socket.SOCK_RAW),
              'rawInet': isolation.blocked_socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)}
    return {'security': security, 'network': shared_net, 'ipc': shared_ipc,
            'devices': isolation.device_inventory(), 'socketDenials': denied,
            'hostRootDenial': isolation.blocked_host_root()}


def check_image(root):
    approval = json.loads((root / 'approval.json').read_text())
    for name, pin in approval['imageTree'].items():
        if name.split('/')[0] not in ('bin', 'ky-ems', 'scada'):
            continue
        path = HOME / name
        if pin['kind'] == 'file':
            assert not path.is_symlink() and path.is_file() and sha(path) == pin['sha256'], name
        elif pin['kind'] == 'symlink':
            assert path.is_symlink() and os.readlink(str(path)) == pin['target'], name
    pins = json.loads((root / 'config-pins.json').read_text())
    actual = {p.relative_to(HOME / 'config/runtime').as_posix(): sha(p)
              for p in (HOME / 'config/runtime').rglob('*') if p.is_file()}
    assert actual == pins, 'private fixture config changed'


def child(args):
    proof = check_scope(args.root, *args.namespaces)
    check_image(args.root)
    write_json(args.root / 'evidence' / ('entry-' + args.role + '-' + str(os.getpid()) + '.json'), proof)
    if args.role == 'observe':
        observe(args.root, args.phase)
        return
    runtime = Path('/tmp/native-service-runtime')
    runtime.mkdir(mode=0o700, exist_ok=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR=str(runtime))
    command = commands(args.root)[args.role]
    os.execve(command[0], command, env)


def observe(root, phase):
    simulator = root / 'gateway-test-lab-sim'

    def control(action, *extra):
        result = subprocess.run([str(simulator), action, '--state-file', str(root / 'data/simulator.state')]
                                + list(extra), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        assert result.returncode == 0, result.stdout

    if phase == 'initialize':
        control('init', '--slaves', '1,2,3')
        return
    if phase == 'changed':
        control('set-value', '--slave', '1', '--area', 'holding', '--address', '1', '--value', '4321')
    elif phase == 'fault':
        control('fault', '--slave', '2', '--mode', 'timeout')
    elif phase == 'recovered':
        control('recover', '--slave', '2')
    samples = []
    started = time.monotonic()
    duration = 180 if phase == 'soak' else 25
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    last = None
    while time.monotonic() - started < duration:
        try:
            with opener.open('http://127.0.0.1:29443/api/v1/realtime/points', timeout=5) as response:
                payload = json.load(response)
            rows = point_rows(payload)
            now = int(time.time() * 1000)
            good = all(index in rows and healthy(rows[index], now)
                       for index in PROBE_INDEXES if phase != 'fault' or index != 920002)
            if phase == 'fault':
                good = good and rows.get(920002, {}).get('quality') == 0
            if phase == 'changed':
                good = good and rows.get(910002, {}).get('value') == 432.1
            last = payload
            if good:
                samples.append({'observedAtMs': now, 'points': [rows[index] for index in PROBE_INDEXES]})
                if phase != 'soak':
                    break
            elif phase == 'soak':
                raise AssertionError('quality or freshness lost during soak')
        except (OSError, ValueError) as error:
            last = repr(error)
            if phase == 'soak':
                raise
        time.sleep(5 if phase == 'soak' else .5)
    result = {'phase': phase, 'samples': samples, 'last': last,
              'elapsedSeconds': time.monotonic() - started, 'simulatedData': True}
    write_json(root / 'evidence' / ('points-' + phase + '.json'), result)
    assert samples, (phase, last)
    if phase == 'soak':
        assert len(samples) >= 30
        for index in range(3):
            assert samples[-1]['points'][index]['ts'] > samples[0]['points'][index]['ts']
    print('POINT_CHAIN_PASS ' + phase, flush=True)


def prepare_config(root):
    config = root / 'config'
    shutil.copytree(str(HOME / 'config/runtime'), str(config))
    device = json.loads((root / 'device_modbus_tcp.template').read_text().replace(
        '__SIMULATOR_HOST__', '127.0.0.1').replace('__SIMULATOR_PORT__', '25020'))
    for meter in device['meters']:
        for point in meter['points']:
            point['write'] = {'enable': False}
    for path in (config / 'devices').glob('*.json'):
        value = json.loads(path.read_text())
        value['enabled'] = False
        write_json(path, value)
    write_json(config / 'devices/device_sim_modbus.json', device)
    for path in (config / 'apps').glob('*.json'):
        value = json.loads(path.read_text())
        value['deviceConfigFiles'] = [str(HOME / 'config/runtime/devices/device_sim_modbus.json')]
        for name in ('mqtt', 'mqttDriver', 'eventEngine', 'computeEngine', 'mqttForward',
                     'emsCluster', 'cameraService', 'agcAvc', 'ota', 'realtime'):
            if name in value:
                value[name]['enabled'] = False
        value.setdefault('mqttDriver', {'enabled': False})
        value['mqttDriver']['sharedMemoryName'] = 'gateway_test_lab_modbus'
        value['mqttDriver']['sharedMemoryNames'] = ['gateway_test_lab_modbus']
        if path.name == 'monitor-service.json':
            monitor = value['systemMonitor']
            monitor.update(enabled=True, diagEnabled=False, defaultIntervalMs=1000)
            monitor['cellular'] = {'enabled': False}
            monitor['recordingTransfer'] = {'enabled': False}
            monitor['scadaUpperComputerSafety'] = {'enabled': False}
            monitor['directMaintenance'] = {
                'enabled': True, 'listenHost': '127.0.0.1', 'listenHosts': ['127.0.0.1'],
                'listenPort': 29443, 'allowedClientCidrs': ['127.0.0.1/32'],
                'identityConfigFile': str(HOME / 'config/runtime/device_identity.json'),
                'appConfigFile': str(HOME / 'config/runtime/apps/monitor-service.json'),
                'otaAppConfigFile': str(HOME / 'config/runtime/apps/monitor-service.json'),
                'authStateFile': str(HOME / 'run/direct-maintenance-state.json'),
                'credentialFile': str(HOME / 'run/direct-maintenance-credential.json'),
                'otaStatusFile': str(HOME / 'run/ota-status.jsonl'),
                'includeKyEmsSnapshot': False, 'realtimeHeartbeatRequiresAuth': True,
                'maxRealtimePoints': 5000}
        write_json(path, value)
    pins = {p.relative_to(config).as_posix(): sha(p) for p in config.rglob('*') if p.is_file()}
    write_json(root / 'config-pins.json', pins)


def acceptance(args):
    root = args.root
    assert os.geteuid() == 0 and os.uname().machine == 'aarch64'
    assert root.parent == Path('/srv') and re.fullmatch(r'gateway-native-services-[A-Za-z0-9-]+', root.name)
    assert root.resolve() == root and stat.S_IMODE(root.stat().st_mode) == 0o700
    assert sha(root / 'approval.json') == args.approval_sha256
    approval = json.loads((root / 'approval.json').read_text())
    assert approval['controlEnabled'] is False and approval['activation'] == 'stopped-only'
    assert approval['gatewayHome'] == str(HOME)
    assert Path('/opt/gateway-reprovision/STOP').is_file()
    assert not Path('/opt/gateway-reprovision/pending.json').exists()
    journal = Path('/opt/gateway-reprovision')
    pointer = json.loads((journal / 'current.json').read_text())
    assert pointer == {'transaction': approval['transaction'], 'approvalSha256': args.approval_sha256}
    transaction = journal / 'transactions' / approval['transaction']
    assert sha(transaction / 'approval.json') == args.approval_sha256
    assert json.loads((transaction / 'state.json').read_text())['phase'] == 'INSTALLED_STOPPED'
    for name, pin in approval['imageTree'].items():
        path = HOME / name
        if pin['kind'] == 'file':
            assert not path.is_symlink() and path.is_file() and sha(path) == pin['sha256'], name
        elif pin['kind'] == 'symlink':
            assert path.is_symlink() and os.readlink(str(path)) == pin['target'], name
    for unit in approval['units']:
        if unit.endswith('@.service'):
            continue
        active = subprocess.check_output(['systemctl', 'show', '--property=ActiveState', '--value', unit]).decode().strip()
        main_pid = subprocess.check_output(['systemctl', 'show', '--property=MainPID', '--value', unit]).decode().strip()
        assert active in ('inactive', 'failed') and main_pid == '0', (unit, active, main_pid)
    helpers = json.loads((root / 'fixture-pins.json').read_text())
    for name, pin in helpers.items():
        assert sha(root / name) == pin, name
    for name in ('data', 'run', 'evidence', 'shm'):
        (root / name).mkdir(mode=0o700)
    prepare_config(root)
    common = ['PrivateNetwork=yes', 'PrivateDevices=yes', 'PrivateTmp=yes', 'ProtectSystem=strict',
              'ProtectHome=yes', 'ProtectKernelTunables=yes', 'ProtectKernelModules=yes',
              'ProtectKernelLogs=yes', 'ProtectControlGroups=yes', 'NoNewPrivileges=yes',
              'CapabilityBoundingSet=CAP_SYS_ADMIN CAP_SETPCAP', 'AmbientCapabilities=',
              'RestrictNamespaces=ipc', 'RestrictRealtime=yes', 'LockPersonality=yes',
              'SystemCallArchitectures=native', 'RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK',
              'UnsetEnvironment=LD_PRELOAD LD_LIBRARY_PATH PYTHONHOME PYTHONPATH',
              'TemporaryFileSystem=/run:rw,nosuid,nodev,noexec,size=16M /dev/pts:rw,nosuid,nodev,noexec,size=1M /opt:ro /var:ro',
              'BindReadOnlyPaths=' + str(HOME) + ' ' + str(root) + ' ' + str(root / 'config') + ':' + str(HOME / 'config/runtime'),
              'BindPaths=' + str(root / 'shm') + ':/dev/shm ' + str(root / 'data') + ':' + str(HOME / 'data') + ' ' +
              str(root / 'run') + ':' + str(HOME / 'run') + ' ' + str(root / 'data') + ' ' + str(root / 'evidence'),
              'ReadWritePaths=/dev/shm ' + str(HOME / 'data') + ' ' + str(HOME / 'run') + ' ' + str(root / 'data') + ' ' + str(root / 'evidence'),
              'WorkingDirectory=' + str(HOME), 'TimeoutStopSec=20', 'RuntimeMaxSec=600',
              'MemoryMax=768M', 'TasksMax=128']
    units = []
    anchor = root.name + '-anchor.service'
    mounted = False
    error = None
    samples = []
    counter = 0

    def command(argv, timeout=60):
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        assert result.returncode == 0, (argv, result.stdout.decode(errors='replace'))
        return result.stdout.decode()

    def properties(unit):
        keys = ('LoadState', 'ActiveState', 'SubState', 'MainPID', 'NRestarts', 'ExecMainStatus', 'Result', 'MemoryCurrent')
        text = command(['systemctl', 'show', unit] + ['--property=' + key for key in keys])
        return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)

    def start(unit, command_args, properties_extra=(), wait=False):
        units.append(unit)
        argv = ['systemd-run', '--unit=' + unit, '--collect']
        if wait:
            argv += ['--wait', '--pipe']
        argv += ['--property=' + value for value in common + list(properties_extra)]
        return command(argv + command_args, timeout=240 if wait else 60)

    try:
        assert not os.path.ismount(str(root / 'shm'))
        command(['mount', '-t', 'tmpfs', '-o', 'mode=700,nosuid,nodev,noexec,size=256M', 'tmpfs', str(root / 'shm')])
        mounted = True
        marker = root / 'shm/gateway-native-service-marker'
        marker.write_text(args.approval_sha256 + '\n')
        value = marker.stat()
        write_json(root / 'marker.json', {'dev': value.st_dev, 'ino': value.st_ino, 'sha256': sha(marker)})
        start(anchor, ['/usr/bin/unshare', '--ipc', '/bin/sleep', '600'])
        pid = properties(anchor)['MainPID']
        assert int(pid) > 1
        host_net, host_ipc = (os.readlink('/proc/1/ns/' + name) for name in ('net', 'ipc'))
        for unused in range(50):
            shared_net, shared_ipc = (os.readlink('/proc/' + pid + '/ns/' + name) for name in ('net', 'ipc'))
            if shared_net != host_net and shared_ipc != host_ipc:
                break
            time.sleep(.1)
        assert shared_net != host_net and shared_ipc != host_ipc
        joined = ['JoinsNamespaceOf=' + anchor, 'After=' + anchor, 'BindsTo=' + anchor]

        def entry(role, phase=None):
            argv = ['/usr/bin/nsenter', '--ipc=/proc/' + pid + '/ns/ipc', '/usr/bin/setpriv', '--no-new-privs',
                    '--bounding-set=-sys_admin,-setpcap', '--inh-caps=-sys_admin,-setpcap', '--ambient-caps=-sys_admin,-setpcap',
                    '/usr/bin/python3', '-B', str(root / 'board_native_service_acceptance.py'), 'entry',
                    '--root', str(root), '--role', role, '--namespaces', host_net, shared_net, host_ipc, shared_ipc]
            return argv + (['--phase', phase] if phase else [])

        def observe_phase(phase):
            nonlocal counter
            counter += 1
            output = start(root.name + '-observer-' + str(counter), entry('observe', phase), joined, wait=True)
            (root / 'evidence' / ('observer-' + phase + '.log')).write_text(output)
            print(output, flush=True)

        observe_phase('initialize')
        by_role = {}
        for role in ('simulator', 'driver', 'monitor', 'ui'):
            unit = root.name + '-' + role + '.service'
            by_role[role] = unit
            restart = 'on-failure' if role in ('ui', 'simulator') else 'always'
            delay = '3' if role == 'ui' else '5'
            start(unit, entry(role), joined + ['Restart=' + restart, 'RestartSec=' + delay, 'KillMode=mixed'])
            time.sleep(2)
            assert properties(unit)['ActiveState'] == 'active', properties(unit)
        for phase in ('normal', 'changed', 'fault', 'recovered'):
            observe_phase(phase)
        for role in ('driver', 'ui'):
            unit = by_role[role]
            before = properties(unit)
            assert before['SubState'] == 'running' and int(before['MainPID']) > 1
            command(['systemctl', 'kill', '--kill-who=main', '--signal=KILL', unit])
            for unused in range(40):
                after = properties(unit)
                if (after['SubState'] == 'running' and int(after['MainPID']) > 1 and
                        after['MainPID'] != before['MainPID'] and int(after['NRestarts']) > int(before['NRestarts'])):
                    break
                time.sleep(.5)
            else:
                raise AssertionError('service did not restart: ' + role)
            write_json(root / 'evidence' / ('restart-' + role + '.json'), {'before': before, 'after': after})
            observe_phase(role + '-restarted')
        samples.append({role: properties(unit) for role, unit in by_role.items()})
        observe_phase('soak')
        samples.append({role: properties(unit) for role, unit in by_role.items()})
        for role in by_role:
            assert samples[1][role]['SubState'] == 'running'
            assert samples[0][role]['MainPID'] == samples[1][role]['MainPID']
            assert samples[0][role]['NRestarts'] == samples[1][role]['NRestarts']
    except BaseException as failure:
        error = repr(failure)
    finally:
        cleanup_errors = []
        for unit in reversed(units):
            try:
                info = properties(unit)
                (root / 'evidence' / (unit + '.journal.log')).write_text(
                    command(['journalctl', '--no-pager', '-o', 'short-monotonic', '-u', unit, '-n', '200']))
                if info['LoadState'] != 'not-found':
                    command(['systemctl', 'stop', unit])
                    assert properties(unit)['MainPID'] == '0'
            except BaseException as failure:
                cleanup_errors.append(str(failure))
        if mounted:
            try:
                command(['umount', str(root / 'shm')])
            except BaseException as failure:
                cleanup_errors.append(str(failure))
        write_json(root / 'evidence/result.json', {'passed': error is None and not cleanup_errors,
                   'failure': error, 'cleanupErrors': cleanup_errors, 'serviceSamples': samples,
                   'canonicalActivationAccepted': False, 'simulatedAcquisition': True,
                   'physicalControlEnabled': False, 'approvalSha256': args.approval_sha256})
    assert error is None and not cleanup_errors, (error, cleanup_errors)
    print('NATIVE_SERVICE_ACQUISITION_RESTART_FIXTURE_PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    run = sub.add_parser('test')
    run.add_argument('--root', type=Path, required=True)
    run.add_argument('--approval-sha256', required=True)
    entry = sub.add_parser('entry')
    entry.add_argument('--root', type=Path, required=True)
    entry.add_argument('--role', choices=('simulator', 'driver', 'monitor', 'ui', 'observe'), required=True)
    entry.add_argument('--namespaces', nargs=4, required=True)
    entry.add_argument('--phase', choices=('initialize', 'normal', 'changed', 'fault', 'recovered',
                                          'driver-restarted', 'ui-restarted', 'soak'))
    args = parser.parse_args()
    (acceptance if args.action == 'test' else child)(args)


if __name__ == '__main__':
    main()
