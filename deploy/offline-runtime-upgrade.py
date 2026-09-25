#!/usr/bin/env python3
"""Narrow SHM10 -> SHM11 installer. Recovery restores files, never old authority."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('upgrade_guard', str(Path(__file__).with_name('runtime-upgrade-guard.py')))
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
UNIT = re.compile(r'(?:(?:modbus-rtu|dlt645-driver|dio-driver|can-driver|iec-driver|compute-engine|agc-avc|ems-cluster|event-engine|local-display|local-display-qt|local-kiosk|system-monitor|camera-service|mqtt-tls-tunnel|mqtt-driver|mqtt-forwarder)@[A-Za-z0-9_.:-]*|gateway-services|gateway-health-watchdog|qt-display-bridge|ky-ems)\.service\Z')
NAME = re.compile(r'[A-Za-z0-9_-]{1,63}\Z')
OBSERVER_UNIT = re.compile(r'(?:compute-engine|ems-cluster|system-monitor)@[A-Za-z0-9_.:-]+\.service\Z')
MONITOR_UNIT = 'system-monitor@monitor-service.service'
MONITOR_DROPIN = Path('/etc/systemd/system/system-monitor@monitor-service.service.d/998-shm.conf')
MONITOR_ENV = b'[Service]\nEnvironment=GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME='
MONITOR_IMPLICIT_SHM = 'gateway_point_store_system_monitor'
A_QT_UNIT = 'ky-ems.service'
A_QT_UNIT_SHA256 = 'e101679abdc622ad35253ec6a79cbb7a9178fafc474d50802026d8c37f9d9593'
A_QT_WRAPPER_SHA256 = '9c0bf58d8002f76407b1a40bbf26a8957e63dc5bfc2df2fe13f9eb04332d5f52'
A_QT_BINARY_SHA256 = '27dcffc5dfa538640ac4d249f2fea934495fe49a87910d0693c4cbeb0994d4b6'
A_BRIDGE_UNIT_SHA256 = '3e219d806f05bd6746cc97a8e57453288577e2cc22fb2fb2aa74f65d1045d2d5'
A_EMPTY_RUNTIME_MAP_SHA256 = '37517e5f3dc66819f61f5a7bb8ace1921282415f10551d2defa5c3eb0985b570'
A_QT_ENV_KEY = b'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME'
B_MONITOR_BINARY_SHA256 = '91f5ab7fd6a1221931defa8f4531b1734f0d2c9b17da9bc0ae6b677d121f0928'
B_MONITOR_UNIT_SHA256 = '946e192e53d54482d0758aac4260575a617f2495b43ad87b4dd1a2f3257465a2'
B_MONITOR_APPS = {'camera-service.json', 'monitor-service.json', 'mqtt-service.json'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def regular(path):
    require(path.is_absolute(), 'absolute path required')
    for part in (path,) + tuple(path.parents):
        require(not part.is_symlink(), 'symlink refused: ' + str(part))
    require(path.is_file() and stat.S_ISREG(path.stat().st_mode) and path.stat().st_nlink == 1,
            'single-link regular file required: ' + str(path))
    return path


def digest(path):
    result = hashlib.sha256()
    with regular(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def checked_json(path, sha):
    require(re.fullmatch('[0-9a-f]{64}', sha or '') is not None, 'external SHA256 pin required')
    raw = regular(path).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == sha, 'SHA256 mismatch: ' + str(path))
    return json.loads(raw.decode('utf-8'))


def sync_dir(path):
    fd = os.open(str(path), os.O_DIRECTORY | os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_new(path, data, mode=0o600):
    for parent in path.parents:
        require(not parent.is_symlink(), 'symlink parent refused: ' + str(parent))
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    sync_dir(path.parent)


def replace(path, data, mode=0o600):
    if path.exists() or path.is_symlink():
        regular(path)
    temp = path.with_name(path.name + '.offline-' + str(os.getpid()))
    write_new(temp, data, mode)
    os.replace(str(temp), str(path))
    sync_dir(path.parent)


def save(path, data):
    replace(path, (json.dumps(data, sort_keys=True, indent=2) + '\n').encode())


def systemctl(*args):
    result = subprocess.run(['systemctl'] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True, timeout=45)
    require(result.returncode == 0, 'systemctl failed: ' + ' '.join(args) + ': ' + result.stderr.strip())
    return result.stdout.strip()


def tree_hashes(root):
    require(not any(p.is_symlink() for p in root.rglob('*')), 'symlink in config tree')
    return {str(path.relative_to(root)): digest(path) for path in sorted(root.rglob('*')) if not path.is_dir()}


def configured(root):
    return {str(p.relative_to(root)): json.loads(regular(p).read_text()) for p in sorted(root.rglob('*.json'))}


def switch_names(value, names):
    if isinstance(value, dict):
        return {key: (names.get(guard.shm_name(item), item) if key in guard.SINGLE_SHM_KEYS else
                      [names.get(guard.shm_name(v), v) for v in item] if key in guard.MULTI_SHM_KEYS
                      else switch_names(item, names)) for key, item in value.items()}
    if isinstance(value, list):
        return [switch_names(item, names) for item in value]
    return value


def no_old_references(value, names):
    if isinstance(value, str):
        require(value.lstrip('/') not in names, 'unmapped SHM reference in unsupported field')
    elif isinstance(value, dict):
        for item in value.values():
            no_old_references(item, names)
    elif isinstance(value, list):
        for item in value:
            no_old_references(item, names)


def controls_disabled(value):
    if isinstance(value, dict):
        if 'emsCluster' in value:
            require(isinstance(value['emsCluster'], dict) and value['emsCluster'].get('controlEnabled') is False,
                    'emsCluster.controlEnabled must explicitly be false')
        for item in value.values():
            controls_disabled(item)
    elif isinstance(value, list):
        for item in value:
            controls_disabled(item)


def monitor_dropin_name(raw):
    require(raw.startswith(MONITOR_ENV) and raw.endswith(b'\n') and raw.count(b'\n') == 2,
            'unsupported monitor SHM drop-in')
    value = raw[len(MONITOR_ENV):-1]
    try:
        name = value.decode('ascii')
    except UnicodeDecodeError:
        raise ValueError('unsupported monitor SHM drop-in')
    require(NAME.fullmatch(name) is not None, 'unsupported monitor SHM drop-in')
    return name


def monitor_binding(approval, names):
    pin = approval.get('systemMonitorShmDropinSha256')
    if pin is None:
        return None
    require(isinstance(pin, str) and re.fullmatch('[0-9a-f]{64}', pin) and
            MONITOR_UNIT in approval['units'], 'monitor SHM binding pin and unit required')
    path = regular(MONITOR_DROPIN)
    old = path.read_bytes()
    require(hashlib.sha256(old).hexdigest() == pin, 'monitor SHM drop-in SHA256 mismatch')
    source = monitor_dropin_name(old)
    require(source in names, 'monitor SHM binding source must be an approved segment')
    return {'old': old, 'new': MONITOR_ENV + names[source].encode() + b'\n',
            'mode': stat.S_IMODE(path.stat().st_mode), 'source': source, 'target': names[source]}


def checked_monitor_record(state_dir, state, approval):
    record = state.get('systemMonitorShmDropin')
    if record is None:
        return None
    require(MONITOR_UNIT in approval['units'] and isinstance(record, dict) and
            set(record) == {'oldSha256', 'newSha256', 'source', 'target', 'mode'} and
            type(record['mode']) is int and 0 <= record['mode'] <= 0o777,
            'invalid monitor SHM binding state')
    old = regular(state_dir / 'monitor-dropin-old').read_bytes()
    source = monitor_dropin_name(old)
    names = {item['source']: item['target'] for item in approval['segments']}
    require(source in names, 'monitor SHM binding source no longer approved')
    new = MONITOR_ENV + names[source].encode() + b'\n'
    require(record['source'] == source and record['target'] == names[source] and
            record['oldSha256'] == approval.get('systemMonitorShmDropinSha256') == hashlib.sha256(old).hexdigest() and
            record['newSha256'] == hashlib.sha256(new).hexdigest() and
            regular(state_dir / 'monitor-dropin-new').read_bytes() == new,
            'monitor SHM binding state/backup mismatch')
    return record, old, new


def monitor_effective_target(target):
    expected = 'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + target
    return (systemctl('show', '--property=Environment', '--value', MONITOR_UNIT) == expected and
            systemctl('show', '--property=EnvironmentFiles', '--value', MONITOR_UNIT) == '' and
            systemctl('show', '--property=UnsetEnvironment', '--value', MONITOR_UNIT) == '')


def a_scada_empty_map(home, documents):
    app = documents.get('apps/monitor-service.json')
    display = app.get('localDisplay') if isinstance(app, dict) else None
    scada = display.get('scada') if isinstance(display, dict) else None
    current = home / 'scada/current'
    require(isinstance(scada, dict) and scada.get('enabled') is True and
            scada.get('autoReload') is True and scada.get('projectDirectory') == str(current) and
            current.is_symlink(), 'A Qt SCADA project binding changed')
    relative = os.readlink(str(current))
    require(re.fullmatch(r'releases/[A-Za-z0-9_.-]+', relative) is not None and
            relative.split('/')[-1] not in ('.', '..'), 'A Qt SCADA current target changed')
    mapping = home / 'scada' / relative / 'runtime-map.json'
    require(digest(mapping) == A_EMPTY_RUNTIME_MAP_SHA256 and regular(mapping).read_bytes() == b'[]\n',
            'A Qt SCADA runtime map is not empty')


def a_joint_binding(home, approval, names, documents):
    pin = approval.get('qtDisplayEnvSha256')
    if pin is None:
        return None
    require(isinstance(pin, str) and re.fullmatch('[0-9a-f]{64}', pin) is not None and
            MONITOR_IMPLICIT_SHM in names and
            {MONITOR_UNIT, A_QT_UNIT} <= set(approval['units']) and
            pin == approval['configSha256'].get('qt-display.env'),
            'A Qt monitor environment approval incomplete')
    envfile = home / 'config/runtime/qt-display.env'
    old = regular(envfile).read_bytes()
    require(hashlib.sha256(old).hexdigest() == pin and A_QT_ENV_KEY not in old and
            b'\x00' not in old, 'A Qt monitor environment changed or already bound')
    require(digest(Path('/etc/systemd/system/ky-ems.service')) == A_QT_UNIT_SHA256 and
            digest(home / 'bin/gateway-qt-run.sh') == A_QT_WRAPPER_SHA256 and
            digest(Path('/etc/systemd/system/system-monitor@.service')) == B_MONITOR_UNIT_SHA256,
            'A Qt or monitor startup binding changed')
    a_scada_empty_map(home, documents)
    target = names[MONITOR_IMPLICIT_SHM]
    return {'old': old, 'new': a_qt_env_new(old, target),
            'mode': stat.S_IMODE(envfile.stat().st_mode), 'target': target,
            'monitorNew': MONITOR_ENV + target.encode() + b'\n'}


def a_qt_env_new(old, target):
    return old + (b'' if old.endswith(b'\n') else b'\n') + A_QT_ENV_KEY + b'=' + target.encode() + b'\n'


def checked_a_joint_record(state_dir, state, approval):
    record = state.get('aJointMonitorQt')
    if record is None:
        return None
    require(isinstance(record, dict) and set(record) ==
            {'oldSha256', 'newSha256', 'monitorSha256', 'target', 'mode'} and
            type(record['mode']) is int and 0 <= record['mode'] <= 0o777,
            'invalid A joint monitor/Qt binding state')
    names = {s['source']: s['target'] for s in approval['segments']}
    target = names.get(MONITOR_IMPLICIT_SHM)
    old = regular(state_dir / 'qt-env-old').read_bytes()
    new = a_qt_env_new(old, target) if target else b''
    monitor_new = MONITOR_ENV + target.encode() + b'\n' if target else b''
    require(target and record['target'] == target and
            record['oldSha256'] == approval.get('qtDisplayEnvSha256') == hashlib.sha256(old).hexdigest() and
            record['newSha256'] == hashlib.sha256(new).hexdigest() and
            record['monitorSha256'] == hashlib.sha256(monitor_new).hexdigest() and
            regular(state_dir / 'qt-env-new').read_bytes() == new and
            regular(state_dir / 'a-monitor-dropin-new').read_bytes() == monitor_new,
            'A joint monitor/Qt binding state/backup mismatch')
    return record, old, new, monitor_new


def monitor_default_unit_binding(approval, inhibited):
    template = Path('/etc/systemd/system/system-monitor@.service')
    require(digest(template) == B_MONITOR_UNIT_SHA256 and
            systemctl('show', '--property=FragmentPath', '--value', MONITOR_UNIT) == str(template),
            'monitor default template binding changed')
    expected = {str(MONITOR_DROPIN)}
    if inhibited:
        # The instance's same-named drop-in overrides the template's in systemd.
        expected.add(str(Path('/etc/systemd/system') / (MONITOR_UNIT + '.d') /
                         ('90-offline-' + approval['transactionId'] + '.conf')))
    actual = systemctl('show', '--property=DropInPaths', '--value', MONITOR_UNIT).split()
    require(len(actual) == len(expected) and set(actual) == expected,
            'monitor default effective drop-ins changed')


def b_monitor_default_safe(home, approval, manifest, state, selected, monitor, documents):
    require(approval_mode(approval) == 'standalone' and
            selected == [MONITOR_UNIT] and monitor is not None and
            monitor[0]['source'] != 'gateway_point_store' and
            any(item['source'] == 'gateway_point_store' for item in state['segments']),
            'unreferenced default SHM: B monitor default scope not proven')
    require(any(c.get('kind') == 'product' and c.get('target') == 'SystemMonitor' and
                c.get('sha256') == B_MONITOR_BINARY_SHA256 for c in manifest['components']),
            'B monitor default candidate mismatch')
    apps = home / 'config/runtime/apps'
    require(apps.is_dir() and not apps.is_symlink() and
            {p.name for p in apps.iterdir()} == B_MONITOR_APPS and
            all(regular(apps / name) for name in B_MONITOR_APPS),
            'B monitor default requires exact three effective apps')
    target = monitor[0]['target']
    for name in B_MONITOR_APPS:
        doc = documents.get('apps/' + name)
        require(isinstance(doc, dict) and doc.get('deviceConfigFiles') == [] and
                guard.configured_names(doc) == {target},
                'B monitor default app references or device list changed: ' + name)
        mqtt = doc.get('mqttDriver')
        camera = doc.get('cameraService')
        cluster = doc.get('emsCluster')
        require(isinstance(mqtt, dict) and mqtt.get('sharedMemoryName') == target and
                mqtt.get('sharedMemoryNames') == [target] and
                isinstance(camera, dict) and camera.get('enabled') is False and
                camera.get('sharedMemoryName') == target and 'agcAvc' not in doc and
                ('emsCluster' not in doc or
                 isinstance(cluster, dict) and cluster.get('enabled') is False),
                'B monitor default primary/camera store binding changed: ' + name)
        if name == 'camera-service.json':
            require('enabled' not in mqtt and 'computeEngine' not in doc and 'localDisplay' not in doc,
                    'B monitor default camera loader shape changed')
        else:
            compute, display = doc.get('computeEngine'), doc.get('localDisplay')
            require(mqtt.get('enabled') is False and isinstance(compute, dict) and
                    compute.get('enabled') is False and compute.get('sharedMemoryNames') == [target] and
                    compute.get('outputDefaultSharedMemoryName') == target and
                    isinstance(display, dict) and display.get('enabled') is False and
                    display.get('sharedMemoryNames') == [target],
                    'B monitor default sibling producer gate changed: ' + name)
        if name == 'monitor-service.json':
            require(isinstance(doc.get('systemMonitor'), dict) and
                    doc['systemMonitor'].get('enabled') is True,
                    'B monitor default primary service gate changed')
        elif 'systemMonitor' in doc:
            require(isinstance(doc['systemMonitor'], dict) and
                    doc['systemMonitor'].get('enabled') is False,
                    'B monitor default sibling service gate changed')


def a_joint_monitor_observe_safe(home, approval, state, selected, joint, documents, inhibited):
    record, _, new_env, new_dropin = joint
    require(approval_mode(approval) == 'standalone' and
            selected in ([MONITOR_UNIT], [MONITOR_UNIT, A_QT_UNIT]) and
            state.get('unreferencedDefaultShm') is True and
            {MONITOR_UNIT, A_QT_UNIT, 'qt-display-bridge.service'} <= set(approval['units']) and
            {s['source'] for s in state['segments']} >= {'gateway_point_store', MONITOR_IMPLICIT_SHM},
            'A joint monitor/Qt observation scope changed')
    apps = home / 'config/runtime/apps'
    require({p.name for p in apps.glob('*.json')} == B_MONITOR_APPS and
            all(regular(apps / name) for name in B_MONITOR_APPS),
            'A joint effective app set changed')
    app = documents.get('apps/monitor-service.json')
    device = 'devices/device_modbusRTU_2_readonly.json'
    device_file = str(home / 'config/runtime' / device)
    camera_app = documents.get('apps/camera-service.json')
    mqtt_app = documents.get('apps/mqtt-service.json')
    memory_store = documents[device].get('memoryStore') if isinstance(documents.get(device), dict) else None
    require(isinstance(app, dict) and app.get('deviceConfigFiles') ==
            [device_file] and isinstance(mqtt_app, dict) and
            mqtt_app.get('deviceConfigFiles') == [device_file] and
            isinstance(camera_app, dict) and camera_app.get('deviceConfigFiles') == [] and
            isinstance(camera_app.get('cameraService'), dict) and
            camera_app['cameraService'].get('enabled') is False and
            all(not isinstance(doc.get('cameraService'), dict) or
                doc['cameraService'].get('enabled') is False for doc in (app, mqtt_app)) and
            isinstance(memory_store, dict) and memory_store.get('sharedMemoryName') in
            {s['target'] for s in state['segments']} and
            isinstance(app.get('systemMonitor'), dict) and app['systemMonitor'].get('enabled') is True and
            isinstance(app.get('cameraService'), dict) and app['cameraService'].get('enabled') is False,
            'A joint monitor loader or disabled camera shape changed')
    mqtt = app.get('mqtt')
    direct = app['systemMonitor'].get('directMaintenance')
    require(isinstance(mqtt, dict) and mqtt.get('enabled') is False and
            isinstance(direct, dict) and direct.get('enabled') is False,
            'A monitor inbound maintenance must be disabled')
    a_scada_empty_map(home, documents)
    envfile = home / 'config/runtime/qt-display.env'
    require(regular(envfile).read_bytes() == new_env and digest(envfile) == record['newSha256'] and
            stat.S_IMODE(envfile.stat().st_mode) == record['mode'] and
            regular(MONITOR_DROPIN).read_bytes() == new_dropin and
            stat.S_IMODE(MONITOR_DROPIN.stat().st_mode) == 0o644,
            'A joint monitor/Qt binding changed')
    require(digest(Path('/etc/systemd/system/ky-ems.service')) == A_QT_UNIT_SHA256 and
            systemctl('show', '--property=FragmentPath', '--value', A_QT_UNIT) ==
            '/etc/systemd/system/ky-ems.service' and
            digest(home / 'bin/gateway-qt-run.sh') == A_QT_WRAPPER_SHA256,
            'A Qt startup binding changed')
    expected = str(home / 'config/runtime/qt-display.env') + ' (ignore_errors=yes)'
    require(systemctl('show', '--property=EnvironmentFiles', '--value', A_QT_UNIT) == expected and
            A_QT_ENV_KEY.decode() not in systemctl('show', '--property=Environment', '--value', A_QT_UNIT) and
            systemctl('show', '--property=UnsetEnvironment', '--value', A_QT_UNIT) == '' and
            systemctl('show', '--property=PassEnvironment', '--value', A_QT_UNIT) == '',
            'A Qt effective environment precedence changed')
    inhibit = Path('/etc/systemd/system/ky-ems.service.d') / ('90-offline-' + approval['transactionId'] + '.conf')
    require(systemctl('show', '--property=DropInPaths', '--value', A_QT_UNIT).split() ==
            ([str(inhibit)] if inhibited or A_QT_UNIT not in selected else []),
            'A Qt effective drop-ins changed')
    bridge = 'qt-display-bridge.service'
    bridge_inhibit = Path('/etc/systemd/system') / (bridge + '.d') / ('90-offline-' + approval['transactionId'] + '.conf')
    marker = home / 'data/runtime-upgrade-stop'
    body = ('[Unit]\nConditionPathExists=!' + str(marker) + '\n').encode()
    require(digest(Path('/etc/systemd/system') / bridge) == A_BRIDGE_UNIT_SHA256 and
            systemctl('show', '--property=FragmentPath', '--value', bridge) ==
            '/etc/systemd/system/qt-display-bridge.service' and
            systemctl('show', '--property=DropInPaths', '--value', bridge).split() == [str(bridge_inhibit)] and
            regular(bridge_inhibit).read_bytes() == body and
            systemctl('show', '--property=ActiveState', '--value', bridge) == 'inactive' and
            systemctl('show', '--property=MainPID', '--value', bridge) == '0',
            'A Qt bridge must remain pinned, fenced and stopped')
    monitor_default_unit_binding(approval, inhibited)
    require(monitor_effective_target(record['target']), 'A monitor effective environment changed')


def no_processes(home, component_names):
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            exe = os.readlink(str(entry / 'exe'))
            if exe.endswith(' (deleted)'):
                exe = exe[:-10]
        except FileNotFoundError:
            continue
        require(Path(exe).name not in guard.RUNTIME_BINARIES | set(component_names)
                and not exe.startswith(str(home) + '/'), 'runtime process still alive: ' + entry.name)


def units_stopped(units):
    for unit in units:
        if unit.endswith('@.service'):
            continue
        state = systemctl('show', '--property=ActiveState', '--value', unit)
        require(state in ('inactive', 'failed'), 'unit not stopped: ' + unit + '=' + state)


def approval_mode(approval):
    require(approval.get('schemaVersion') == 'offline-shm11-2', 'schema2 offline approval required')
    mode = approval.get('mode')
    require(mode in ('fixed-voter', 'standalone'), 'explicit fixed-voter or standalone mode required')
    return mode


def install_paths(components, approval):
    paths = approval.get('installPaths')
    targets = [c['target'] for c in components]
    require(isinstance(paths, dict) and targets and len(targets) == len(set(targets)) and
            set(paths) == set(targets) and 'installNames' not in approval, 'exact installPaths product mapping required')
    for target, relative in paths.items():
        require(isinstance(target, str) and NAME.fullmatch(target), 'unsafe component target')
        allowed = {'bin/' + target}
        if target in ('LocalDisplayQtEms', 'KY-EMS'):
            allowed.update(('bin/KY-EMS', 'ky-ems/KY-EMS'))
        require(isinstance(relative, str) and relative in allowed, 'unsupported program relative path')
    require(len(set(paths.values())) == len(paths), 'duplicate program destination')
    return paths


def installed_inventory(home, paths, approval):
    installed = {}
    for directory in (home / 'bin', home / 'ky-ems'):
        require(not directory.is_symlink(), 'symlink runtime directory refused')
        if not directory.exists():
            continue
        for path in directory.rglob('*'):
            require(not path.is_symlink(), 'symlink in runtime tree refused: ' + str(path))
            if path.is_file() and guard.runtime_file(path, path):
                installed[str(path.relative_to(home))] = digest(path)
    require(set(installed) <= set(paths.values()), 'installed runtime absent from approved paths')
    require(installed == approval.get('installedRuntimeSha256'), 'installed runtime inventory changed')
    for relative in paths.values():
        destination = home / relative
        require(not destination.is_symlink() and not destination.parent.is_symlink(), 'symlink destination refused')
        require(not destination.exists() or relative in installed, 'unapproved existing destination')


def no_mappings(names):
    identities = set()
    for name in names:
        value = regular(Path('/dev/shm') / name).stat()
        identities.add((os.major(value.st_dev), os.minor(value.st_dev), value.st_ino))
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            with (entry / 'maps').open() as stream:
                for line in stream:
                    fields = line.split()
                    device = fields[3].split(':')
                    require((int(device[0], 16), int(device[1], 16), int(fields[4])) not in identities,
                            'SHM still mapped by pid ' + entry.name)
        except (FileNotFoundError, ProcessLookupError):
            continue


def read_inputs(args):
    approval = checked_json(args.approval, args.approval_sha256)
    approval_mode(approval)
    require(guard.compatibility(approval) is not None, 'runtimeCompatibility required')
    require(approval.get('controlEnabled') is False, 'control must remain disabled')
    require(NAME.fullmatch(approval.get('transactionId', '')) is not None, 'unsafe transaction id')
    home = Path(approval['gatewayHome'])
    require(re.fullmatch('/[A-Za-z0-9_/-]+', str(home)) and home.resolve() == home and
            str(home) not in ('/', '/opt'), 'unsafe gateway home')
    manifest = checked_json(args.manifest, approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, approval)
    require('memory_point_store_migrate' in paths, 'complete product set with migration CLI required')
    for component in components:
        require(NAME.fullmatch(component['target']) is not None, 'unsafe component target')
        path = args.payload / component['archivePath']
        require(args.payload in path.resolve().parents, 'component escapes payload')
        require(digest(path) == component['sha256'] and path.stat().st_size == component['bytes'],
                'component hash/size mismatch: ' + component['target'])
    installed_inventory(home, paths, approval)
    units = approval['units']
    require(isinstance(units, list) and len(units) == len(set(units)) and
            'gateway-services.service' in units and
            all(isinstance(u, str) and UNIT.fullmatch(u) for u in units), 'invalid Gateway-only unit set')
    if 'gateway-health-watchdog.service' not in units:
        require(systemctl('show', '--property=LoadState', '--value', 'gateway-health-watchdog.service') == 'not-found',
                'omitted watchdog must have LoadState=not-found')
    discovered = set()
    for action in ('list-units', 'list-unit-files'):
        for line in systemctl(action, '--all', '--plain', '--no-legend').splitlines():
            if line.split() and UNIT.fullmatch(line.split()[0]):
                discovered.add(line.split()[0])
    require(discovered <= set(units), 'Gateway unit missing from offline scope: ' + ','.join(sorted(discovered - set(units))))
    return approval, manifest, components, home


def local_identity(home, approval):
    mode = approval_mode(approval)
    config = home / 'config/runtime'
    require(digest(config / 'device_identity.json') == approval['identitySha256'], 'identity changed')
    identity = json.loads((config / 'device_identity.json').read_text())
    require(identity.get('machineCode') == approval['nodeId'], 'foreign node approval')
    clusters = []
    def find_clusters(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key == 'emsCluster':
                    require(isinstance(item, dict) and type(item.get('enabled', False)) is bool,
                            'ambiguous EMS cluster configuration')
                    if item.get('enabled', False):
                        clusters.append(item)
                find_clusters(item)
        elif isinstance(value, list):
            for item in value:
                find_clusters(item)
    for document in configured(config).values():
        find_clusters(document)
    if mode == 'standalone':
        require(not clusters, 'standalone cannot bypass an enabled EMS cluster')
        require('membershipSha256' not in approval and 'offlineVoters' not in approval,
                'standalone must use local evidence, not a fabricated roster/voter set')
        return set()
    require('offlineLocal' not in approval, 'fixed-voter mode requires full voter evidence')
    roster = checked_json(home / 'data/cluster-membership.json', approval['membershipSha256'])
    require(isinstance(roster, dict) and roster.get('schemaVersion') == '1.0' and 'members' not in roster and
            isinstance(roster.get('clusterId'), str) and bool(roster['clusterId']) and
            type(roster.get('membershipEpoch')) is int and 0 < roster['membershipEpoch'] <= 0xffffffffffffffff,
            'invalid fixed roster schema/epoch')
    assignments = roster.get('assignments')
    require(isinstance(assignments, list) and 2 <= len(assignments) <= 5 and all(
            isinstance(item, dict) and isinstance(item.get('nodeId'), str) and bool(item['nodeId']) and
            type(item.get('cabinetNo')) is int and 1 <= item['cabinetNo'] <= 5 for item in assignments),
            'invalid fixed roster assignments')
    voters = {item['nodeId'] for item in assignments}
    cabinets = {item['cabinetNo'] for item in assignments}
    require(len(voters) == len(assignments) == len(cabinets) and approval['nodeId'] in voters,
            'duplicate or missing local fixed voter')
    require(clusters, 'enabled fixed-voter cluster configuration required')
    own = next(item['cabinetNo'] for item in assignments if item['nodeId'] == approval['nodeId'])
    for cluster in clusters:
        expected, maximum = cluster.get('expectedMembers'), cluster.get('maxMembers')
        locked = cluster.get('lockedCabinetNo', 0)
        require(cluster.get('clusterId') == roster['clusterId'] and type(expected) is int and type(maximum) is int and
                2 <= expected <= maximum <= 5 and len(assignments) == expected and max(cabinets) <= maximum and
                type(locked) is int and locked in (0, own), 'fixed voter set does not match local cluster configuration')
        require(cluster.get('membershipFile', str(home / 'data/cluster-membership.json')) == str(home / 'data/cluster-membership.json'),
                'non-default membershipFile is not supported by this offline entrypoint')
    return voters


def fence(home, approval, state_dir, state):
    marker = home / 'data/runtime-upgrade-stop'
    if marker.exists():
        require(marker.read_text().strip() == approval['transactionId'], 'another offline operation holds stop fence')
    else:
        write_new(marker, (approval['transactionId'] + '\n').encode())
    # Conditions persist across reboot without altering enabled/masked policy.
    for unit in approval['units']:
        dropin = Path('/etc/systemd/system') / (unit + '.d') / ('90-offline-' + approval['transactionId'] + '.conf')
        body = ('[Unit]\nConditionPathExists=!' + str(marker) + '\n').encode()
        if dropin.exists():
            require(regular(dropin).read_bytes() == body, 'foreign unit inhibition file')
        else:
            write_new(dropin, body, 0o644)
    state['phase'] = 'FENCED'
    save(state_dir / 'state.json', state)
    systemctl('daemon-reload')
    systemctl('stop', *(u for u in approval['units'] if not u.endswith('@.service')))
    units_stopped(approval['units'])


def apply(args):
    approval, manifest, components, home = read_inputs(args)
    require(type(approval.get('expiresAtUnix')) is int and time.time() < approval['expiresAtUnix'], 'offline approval expired')
    voters = local_identity(home, approval)
    if approval_mode(approval) == 'fixed-voter':
        require(set(approval['offlineVoters']) == voters, 'all fixed voters require offline evidence')
        records = approval['offlineVoters']
    else:
        record = approval.get('offlineLocal', {})
        require(isinstance(record, dict) and record.get('nodeId') == approval['nodeId'], 'local offline evidence required')
        records = {approval['nodeId']: record}
    for node, record in records.items():
        require(record.get('controlDisabled') is True and record.get('participantsStopped') is True and
                record.get('restartInhibited') is True and re.fullmatch('[0-9a-f]{64}', record.get('evidenceSha256', '')),
                'incomplete offline evidence: ' + node)
    configs = home / 'config/runtime'
    require(tree_hashes(configs) == approval['configSha256'], 'config set or bytes changed')
    documents = configured(configs)
    for doc in documents.values():
        controls_disabled(doc)
    references = set().union(*(guard.configured_names(doc) for doc in documents.values()))
    segments = approval['segments']
    names = {s['source']: s['target'] for s in segments}
    require(len(names) == len(segments) and len(set(names.values())) == len(names) and names and
            all(NAME.fullmatch(n) for n in list(names) + list(names.values())) and not set(names) & set(names.values()),
            'distinct unique source and target SHM names required')
    # The fixed default may exist without a JSON reference; retain and migrate it
    # under the same explicit segment, digest, quiescence and backup gates.
    unreferenced_default = 'gateway_point_store' in names and 'gateway_point_store' not in references
    require(references <= set(names) and set(names) - references <= {'gateway_point_store', MONITOR_IMPLICIT_SHM},
            'all SHM references must be explicit and covered exactly')
    monitor_path = Path('/dev/shm', MONITOR_IMPLICIT_SHM)
    require(not (monitor_path.exists() or monitor_path.is_symlink()) or MONITOR_IMPLICIT_SHM in names,
            'uncovered implicit SystemMonitor SHM segment')
    if MONITOR_IMPLICIT_SHM in names:
        targets = {item['target'] for item in components}
        require(approval_mode(approval) == 'standalone' and
                {MONITOR_UNIT, 'ky-ems.service'} <= set(approval['units']) and
                {'SystemMonitor', 'LocalDisplayQtEms'} <= targets and
                approval['installPaths']['LocalDisplayQtEms'] == 'ky-ems/KY-EMS' and
                not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink() and
                'systemMonitorShmDropinSha256' not in approval,
                'unbound implicit SystemMonitor SHM requires stopped monitor and KY-EMS scope')
    binding = monitor_binding(approval, names)
    a_binding = a_joint_binding(home, approval, names, documents)
    if a_binding:
        require(any(c.get('kind') == 'product' and c.get('target') == 'LocalDisplayQtEms' and
                    c.get('sha256') == A_QT_BINARY_SHA256 for c in manifest['components']) and
                any(c.get('kind') == 'product' and c.get('target') == 'SystemMonitor' and
                    c.get('sha256') == B_MONITOR_BINARY_SHA256 for c in manifest['components']),
                'A monitor/Qt ABI11 candidate mismatch')
    for doc in documents.values():
        no_old_references(switch_names(doc, names), names)
    # An existing default segment may be an implicit reader not covered by this recipe.
    require(not Path('/dev/shm/gateway_point_store').exists() or 'gateway_point_store' in names,
            'uncovered default SHM segment')
    for segment in segments:
        source = Path('/dev/shm') / segment['source']
        require(digest(source) == segment['sha256'], 'source SHM changed')
        require(not (Path('/dev/shm') / segment['target']).exists(), 'target SHM already exists')
    require(not args.state.exists(), 'state directory already exists; use recover, never replay apply')
    require(args.state.is_absolute() and args.state.resolve() == args.state and
            home not in args.state.parents and args.state not in home.parents, 'separate persistent state directory required')
    fs = subprocess.check_output(['stat', '-f', '-c', '%T', str(args.state.parent)], universal_newlines=True).strip()
    require(fs in ('ext2/ext3', 'ext4', 'xfs', 'btrfs'), 'state requires persistent ext-family/XFS/Btrfs filesystem')
    args.state.mkdir(mode=0o700)
    write_new(args.state / 'approval.json', regular(args.approval).read_bytes())
    write_new(args.state / 'program-manifest.json', regular(args.manifest).read_bytes())
    state = {'phase': 'PREPARED', 'stopConfirmed': False, 'approvalSha256': args.approval_sha256, 'files': [], 'segments': [],
             'unreferencedDefaultShm': unreferenced_default,
             'implicitMonitorShmUnqualified': MONITOR_IMPLICIT_SHM in names and a_binding is None,
             'unitStates': {u: ({'enabled': 'template-file', 'active': 'not-instance'} if u.endswith('@.service') else
                               {'enabled': systemctl('show', '--property=UnitFileState', '--value', u),
                                'active': systemctl('show', '--property=ActiveState', '--value', u)}) for u in approval['units']}}
    save(args.state / 'state.json', state)
    try:
        fence(home, approval, args.state, state)
        no_processes(home, [c['target'] for c in components])
        require(not (monitor_path.exists() or monitor_path.is_symlink()) or MONITOR_IMPLICIT_SHM in names,
                'uncovered implicit SystemMonitor SHM segment after stop')
        state['stopConfirmed'] = True
        require(tree_hashes(configs) == approval['configSha256'], 'config changed during stop')
        if binding:
            path = regular(MONITOR_DROPIN)
            require(path.read_bytes() == binding['old'] and stat.S_IMODE(path.stat().st_mode) == binding['mode'],
                    'monitor SHM drop-in changed during stop')
            write_new(args.state / 'monitor-dropin-old', binding['old'])
            write_new(args.state / 'monitor-dropin-new', binding['new'])
            state['systemMonitorShmDropin'] = {
                'oldSha256': hashlib.sha256(binding['old']).hexdigest(),
                'newSha256': hashlib.sha256(binding['new']).hexdigest(),
                'source': binding['source'], 'target': binding['target'], 'mode': binding['mode']}
            save(args.state / 'state.json', state)
        if a_binding:
            require(regular(home / 'config/runtime/qt-display.env').read_bytes() == a_binding['old'] and
                    stat.S_IMODE((home / 'config/runtime/qt-display.env').stat().st_mode) == a_binding['mode'],
                    'A Qt monitor environment changed during stop')
            a_scada_empty_map(home, configured(configs))
            require(digest(Path('/etc/systemd/system/ky-ems.service')) == A_QT_UNIT_SHA256 and
                    digest(home / 'bin/gateway-qt-run.sh') == A_QT_WRAPPER_SHA256 and
                    digest(Path('/etc/systemd/system/system-monitor@.service')) == B_MONITOR_UNIT_SHA256,
                    'A Qt or monitor startup binding changed during stop')
            write_new(args.state / 'qt-env-old', a_binding['old'])
            write_new(args.state / 'qt-env-new', a_binding['new'])
            write_new(args.state / 'a-monitor-dropin-new', a_binding['monitorNew'])
            state['aJointMonitorQt'] = {
                'oldSha256': hashlib.sha256(a_binding['old']).hexdigest(),
                'newSha256': hashlib.sha256(a_binding['new']).hexdigest(),
                'monitorSha256': hashlib.sha256(a_binding['monitorNew']).hexdigest(),
                'target': a_binding['target'], 'mode': a_binding['mode']}
            save(args.state / 'state.json', state)
        paths = install_paths(components, approval)
        installed_inventory(home, paths, approval)
        changed = []
        for component in components:
            path = args.payload / component['archivePath']
            payload = regular(path).read_bytes()
            require(hashlib.sha256(payload).hexdigest() == component['sha256'], 'staged component changed')
            changed.append((home / paths[component['target']], payload, 0o755))
        for relative, doc in documents.items():
            updated = switch_names(doc, names)
            if updated != doc:
                require(relative != 'device_identity.json', 'identity cannot contain migrated references')
                changed.append((configs / relative, (json.dumps(updated, indent=2) + '\n').encode(), stat.S_IMODE((configs / relative).stat().st_mode)))
        if a_binding:
            changed.append((configs / 'qt-display.env', a_binding['new'], a_binding['mode']))
        for index, (destination, data, mode) in enumerate(changed):
            backup = args.state / 'files' / str(index)
            prior = digest(destination) if destination.exists() else None
            old_mode = stat.S_IMODE(destination.stat().st_mode) if prior else None
            if prior:
                write_new(backup, regular(destination).read_bytes())
                require(digest(backup) == prior, 'file backup verification failed')
            stage = args.state / 'new' / str(index)
            write_new(stage, data, mode)
            state['files'].append({'path': str(destination.relative_to(home)), 'oldSha256': prior,
                                   'newSha256': digest(stage), 'oldMode': old_mode, 'mode': mode})
        save(args.state / 'state.json', state)
        cli = next(c for c in components if c['target'] == 'memory_point_store_migrate')
        cli_path = args.state / 'new' / str(components.index(cli))
        require(digest(cli_path) == cli['sha256'], 'private migration CLI mismatch')
        for segment in segments:
            source = Path('/dev/shm') / segment['source']
            require(digest(source) == segment['sha256'], 'source SHM changed during stop')
            command = [str(cli_path), '--shm', segment['source'], '--copy-to-v11', segment['target'],
                       '--offline-confirmed', '--backup', str(args.state / (segment['source'] + '.v10.bak'))]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            write_new(args.state / (segment['source'] + '.migration.log'), result.stdout)
            require(result.returncode == 0, 'migration refused/failed for ' + segment['source'] + ': ' + result.stdout.decode(errors='replace'))
            require(digest(source) == segment['sha256'], 'migration changed source')
            state['segments'].append({'source': segment['source'], 'target': segment['target'],
                                      'sourceSha256': digest(source), 'targetSha256': digest(Path('/dev/shm') / segment['target'])})
            save(args.state / 'state.json', state)
        state['phase'] = 'SWITCHING'
        save(args.state / 'state.json', state)
        no_processes(home, [c['target'] for c in components])
        if binding:
            require(regular(MONITOR_DROPIN).read_bytes() == binding['old'],
                    'monitor SHM drop-in changed before switch')
            require(digest(args.state / 'monitor-dropin-new') == state['systemMonitorShmDropin']['newSha256'],
                    'monitor SHM staged drop-in changed')
            replace(MONITOR_DROPIN, regular(args.state / 'monitor-dropin-new').read_bytes(), binding['mode'])
        if a_binding:
            require(not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink(),
                    'A monitor drop-in appeared before switch')
            require(regular(configs / 'qt-display.env').read_bytes() == a_binding['old'],
                    'A Qt monitor environment changed before switch')
            write_new(MONITOR_DROPIN, regular(args.state / 'a-monitor-dropin-new').read_bytes(), 0o644)
        for index, item in enumerate(state['files']):
            stage = args.state / 'new' / str(index)
            require(digest(stage) == item['newSha256'], 'staged file changed')
            replace(home / item['path'], stage.read_bytes(), item['mode'])
        if binding or a_binding:
            systemctl('daemon-reload')
        state['phase'] = 'UPGRADED_STOPPED'
        state['configSha256'] = tree_hashes(configs)
        save(args.state / 'state.json', state)
        print('UPGRADED_STOPPED: SHM11 files installed; no services started, no physical control authorized')
    except BaseException:
        state['phase'] = 'FAILED_STOPPED' if state['stopConfirmed'] else 'FAILED_STOP_UNCONFIRMED'
        save(args.state / 'state.json', state)
        raise


def recover(args):
    state = json.loads(regular(args.state / 'state.json').read_text())
    approval = checked_json(args.state / 'approval.json', args.approval_sha256)
    require(state['approvalSha256'] == args.approval_sha256, 'state approval mismatch')
    home = Path(approval['gatewayHome'])
    local_identity(home, approval)
    fence(home, approval, args.state, state)
    no_processes(home, [])
    manifest = checked_json(args.state / 'program-manifest.json', approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, approval)
    products = {paths[c['target']]: c for c in components}
    monitor = checked_monitor_record(args.state, state, approval)
    if monitor:
        record, _, _ = monitor
        require(digest(MONITOR_DROPIN) in (record['oldSha256'], record['newSha256']),
                'monitor SHM drop-in changed since transaction')
    a_joint = checked_a_joint_record(args.state, state, approval)
    if a_joint:
        record, _, _, _ = a_joint
        require(not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink() or
                (digest(MONITOR_DROPIN) == record['monitorSha256'] and
                 stat.S_IMODE(MONITOR_DROPIN.stat().st_mode) == 0o644),
                'A monitor drop-in changed since transaction')
    # Validate every backup before restoring anything. Durable data and SHM are never restored.
    for index, item in enumerate(state['files']):
        relative = Path(item['path'])
        require(not relative.is_absolute() and '..' not in relative.parts and
                (str(relative) in products or relative.parts[:2] == ('config', 'runtime')) and
                relative != Path('config/runtime/device_identity.json'), 'unsafe recovery path')
        if str(relative) in products:
            require(item['newSha256'] == products[str(relative)]['sha256'] and
                    item['oldSha256'] == approval['installedRuntimeSha256'].get(str(relative)), 'unapproved binary recovery')
        else:
            require(item['oldSha256'] == approval['configSha256'].get(str(relative.relative_to('config/runtime'))),
                    'unapproved config recovery')
        if item['oldSha256']:
            require(digest(args.state / 'files' / str(index)) == item['oldSha256'], 'recovery backup mismatch')
        live = home / relative
        require(not live.exists() or digest(live) in (item['oldSha256'], item['newSha256']), 'live file changed since transaction')
    for index, item in enumerate(state['files']):
        live = home / item['path']
        if item['oldSha256']:
            replace(live, (args.state / 'files' / str(index)).read_bytes(), item['oldMode'])
        elif live.exists():
            # Retain newly introduced executables as evidence outside the live bin directory.
            retained = args.state / ('recovered-new-' + str(index))
            if not retained.exists():
                write_new(retained, regular(live).read_bytes(), item['mode'])
            require(digest(retained) == item['newSha256'], 'retained new file verification failed')
            live.unlink()
            sync_dir(live.parent)
    if monitor:
        record, old, _ = monitor
        replace(MONITOR_DROPIN, old, record['mode'])
        systemctl('daemon-reload')
    if a_joint:
        if MONITOR_DROPIN.exists():
            MONITOR_DROPIN.unlink()
            sync_dir(MONITOR_DROPIN.parent)
        systemctl('daemon-reload')
    state['phase'] = 'RECOVERED_STOPPED'
    save(args.state / 'state.json', state)
    print('RECOVERED_STOPPED: old files restored; SHM/dedup/consensus preserved; old control MUST NOT restart')


def observe(args):
    require(args.ready and args.ready_sha256, 'observe requires separate externally approved ready receipt')
    ready = checked_json(args.ready, args.ready_sha256)
    require(ready.get('schemaVersion') == 'offline-shm11-observe-2' and ready.get('controlEnabled') is False and
            type(ready.get('expiresAtUnix')) is int and time.time() < ready['expiresAtUnix'], 'invalid/expired observe approval')
    state = checked_json(args.state / 'state.json', ready['stateSha256'])
    require(state['phase'] == 'UPGRADED_STOPPED' and state['approvalSha256'] == args.approval_sha256,
            'observe requires unchanged successful stopped upgrade')
    require(state.get('implicitMonitorShmUnqualified') is not True,
            'implicit SystemMonitor SHM readers are not jointly bound for observe')
    approval = checked_json(args.state / 'approval.json', args.approval_sha256)
    a_joint = checked_a_joint_record(args.state, state, approval)
    require(ready.get('approvalSha256') == args.approval_sha256 and ready.get('transactionId') == approval['transactionId'] and
            ready.get('programManifestSha256') == approval['programManifestSha256'], 'observe approval lineage mismatch')
    home = Path(approval['gatewayHome'])
    mode = approval_mode(approval)
    require(ready.get('mode') == mode, 'observe mode must match original approval')
    voters = local_identity(home, approval)
    if mode == 'fixed-voter':
        require('local' not in ready and set(ready['voters']) == voters, 'all fixed voters must confirm upgraded/stopped')
        receipts = ready['voters']
    else:
        receipt = ready.get('local', {})
        require('voters' not in ready and isinstance(receipt, dict) and receipt.get('nodeId') == approval['nodeId'],
                'standalone local ready receipt required')
        receipts = {approval['nodeId']: receipt}
    for node, receipt in receipts.items():
        require(receipt.get('phase') == 'UPGRADED_STOPPED' and receipt.get('controlEnabled') is False and
                receipt.get('programManifestSha256') == approval['programManifestSha256'] and
                re.fullmatch('[0-9a-f]{64}', receipt.get('stateSha256', '')), 'voter not ready: ' + node)
    require(receipts[approval['nodeId']]['stateSha256'] == ready['stateSha256'], 'local ready receipt mismatch')
    require((home / 'data/runtime-upgrade-stop').read_text().strip() == approval['transactionId'], 'persistent fence missing')
    manifest = checked_json(args.state / 'program-manifest.json', approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, approval)
    for component in components:
        require(digest(home / paths[component['target']]) == component['sha256'], 'installed component no longer approved')
    installed_inventory(home, paths, {'installedRuntimeSha256': {
        paths[c['target']]: c['sha256'] for c in components}})
    require(tree_hashes(home / 'config/runtime') == state['configSha256'], 'upgraded configuration changed')
    documents = configured(home / 'config/runtime')
    for doc in documents.values():
        controls_disabled(doc)
    for segment in state['segments']:
        for side in ('source', 'target'):
            require(digest(Path('/dev/shm') / segment[side]) == segment[side + 'Sha256'], 'SHM changed before observe')
    no_processes(home, [c['target'] for c in components])
    no_mappings([s[k] for s in state['segments'] for k in ('source', 'target')])
    units_stopped(approval['units'])
    selected = ready['startUnits']
    require(isinstance(selected, list) and selected and len(selected) == len(set(selected)) and
            set(selected) <= set(approval['units']) and
            all(OBSERVER_UNIT.fullmatch(u) or (a_joint and u == A_QT_UNIT) for u in selected) and
            (mode != 'standalone' or not any(u.startswith('ems-cluster@') for u in selected)),
            'only explicitly approved compute/cluster/monitor observers may start; physical participants remain inhibited')
    monitor = None
    if any(unit.startswith('system-monitor@') for unit in selected):
        require({unit for unit in selected if unit.startswith('system-monitor@')} == {MONITOR_UNIT},
                'only the pinned monitor SHM instance may observe')
        if a_joint is None:
            monitor = checked_monitor_record(args.state, state, approval)
            require(monitor is not None, 'monitor SHM binding evidence required before observe')
            record, _, new = monitor
            require(regular(MONITOR_DROPIN).read_bytes() == new and digest(MONITOR_DROPIN) == record['newSha256'] and
                    stat.S_IMODE(MONITOR_DROPIN.stat().st_mode) == record['mode'],
                    'monitor SHM drop-in changed before observe')
            require(monitor_effective_target(record['target']),
                    'monitor SHM effective environment is not the approved target')
    if a_joint:
        a_joint_monitor_observe_safe(home, approval, state, selected, a_joint, documents, True)
        if A_QT_UNIT in selected:
            require(systemctl('show', '--property=ActiveState', '--value', 'graphical.target') == 'active',
                    'A Qt observe requires an already active graphical target')
    if state.get('unreferencedDefaultShm') is True:
        if a_joint is None:
            b_monitor_default_safe(home, approval, manifest, state, selected, monitor, documents)
    for unit in selected:
        before = state['unitStates'][unit]['enabled']
        require(before not in ('masked', 'masked-runtime') and
                systemctl('show', '--property=UnitFileState', '--value', unit) == before, 'unit policy changed or was masked')
    dropins = []
    marker = home / 'data/runtime-upgrade-stop'
    inhibit_units = set(selected)
    for unit in selected:
        template = unit.split('@', 1)[0] + '@.service'
        if template in approval['units']:
            inhibit_units.add(template)
    for unit in sorted(inhibit_units):
        path = Path('/etc/systemd/system') / (unit + '.d') / ('90-offline-' + approval['transactionId'] + '.conf')
        body = ('[Unit]\nConditionPathExists=!' + str(marker) + '\n').encode()
        require(regular(path).read_bytes() == body, 'unit inhibition changed')
        dropins.append((path, body))
    if state.get('unreferencedDefaultShm') is True:
        monitor_default_unit_binding(approval, True)
    state['phase'] = 'STARTING_OBSERVERS'
    state['observeApprovalSha256'] = args.ready_sha256
    save(args.state / 'state.json', state)
    try:
        for path, _ in dropins:
            path.unlink()
            sync_dir(path.parent)
        systemctl('daemon-reload')
        if state.get('unreferencedDefaultShm') is True:
            monitor_default_unit_binding(approval, False)
            if a_joint:
                a_joint_monitor_observe_safe(home, approval, state, selected, a_joint, documents, False)
            else:
                require(regular(MONITOR_DROPIN).read_bytes() == new and
                        stat.S_IMODE(MONITOR_DROPIN.stat().st_mode) == record['mode'],
                        'monitor SHM drop-in changed before start')
        if any(unit.startswith('system-monitor@') for unit in selected):
            target = a_joint[0]['target'] if a_joint else state['systemMonitorShmDropin']['target']
            require(monitor_effective_target(target),
                    'monitor SHM effective environment changed before start')
        if state.get('unreferencedDefaultShm') is True:
            require(tree_hashes(home / 'config/runtime') == state['configSha256'],
                    'B monitor default config changed before start')
        for unit in selected:
            systemctl('start', unit)
            systemctl('is-active', '--quiet', unit)
        if a_joint:
            require(tree_hashes(home / 'config/runtime') == state['configSha256'],
                    'A joint config changed after start')
            a_joint_monitor_observe_safe(home, approval, state, selected, a_joint, documents, False)
            if A_QT_UNIT in selected:
                require(systemctl('show', '--property=ConditionResult', '--value', 'qt-display-bridge.service') == 'no',
                        'A Qt bridge inhibition condition did not skip startup')
            units_stopped([unit for unit in approval['units'] if unit not in selected])
        if state.get('unreferencedDefaultShm') is True:
            for segment in state['segments']:
                require(digest(Path('/dev/shm') / segment['source']) == segment['sourceSha256'],
                        'B monitor default source SHM changed after start')
            no_mappings([segment['source'] for segment in state['segments']])
        elif monitor:
            no_mappings([monitor[0]['source']])
        for path, body in dropins:
            write_new(path, body, 0o644)
        systemctl('daemon-reload')
        state['phase'] = 'OBSERVING_CONTROL_DISABLED'
        state['startedUnits'] = selected
        save(args.state / 'state.json', state)
    except BaseException:
        state['phase'] = 'FAILED_OBSERVER_START'
        # Best effort restoration must not skip stopping already-started observers.
        for path, body in dropins:
            try:
                if not path.exists():
                    write_new(path, body, 0o644)
            except OSError:
                state['phase'] = 'FAILED_INHIBITION_RESTORE'
        try:
            systemctl('daemon-reload')
        except (OSError, ValueError, subprocess.SubprocessError):
            state['phase'] = 'FAILED_INHIBITION_RESTORE'
        try:
            stopped = selected + (['qt-display-bridge.service'] if a_joint and A_QT_UNIT in selected else [])
            systemctl('stop', *stopped)
            units_stopped(stopped)
            if a_joint:
                require(all(systemctl('show', '--property=MainPID', '--value', unit) == '0'
                            for unit in stopped), 'A joint observer process still active after failed start')
        except BaseException:
            state['phase'] = 'FAILED_STOP_UNCONFIRMED'
        save(args.state / 'state.json', state)
        raise
    print('OBSERVING_CONTROL_DISABLED: selected observers active; drivers/launcher remain fenced; no physical approval')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'recover', 'observe'))
    parser.add_argument('--approval', type=Path)
    parser.add_argument('--approval-sha256', required=True)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--ready', type=Path)
    parser.add_argument('--ready-sha256')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'root in host PID/mount namespace required')
    if args.action == 'apply':
        require(args.approval and args.manifest and args.payload, 'apply requires approval, manifest and payload')
        args.payload = args.payload.absolute()
    lock_path = Path('/run/gateway-offline-runtime.lock')
    fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        globals()[args.action](args)
    finally:
        os.close(fd)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('offline upgrade refused/failed: ' + str(error) + '\nKeep all participants stopped; no automatic ABI rollback.', file=sys.stderr)
        sys.exit(2)
