#!/usr/bin/env python3
"""Narrow SHM10 -> SHM11 installer. Recovery restores files, never old authority."""
import argparse
import ctypes
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
import uuid
import copy
import contextlib
import tarfile

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
ACTIVATION_SCRIPTS = ('runtime-upgrade-guard.py', 'gateway-services.sh')
# This source entry is test-only for CORE_NODES; factory staging hard-disables
# new core apply. Keep saved transaction validation/activation/recovery compatible.
CORE_TEST_APPLY_ALLOWED = True


def require(condition, message):
    if not condition:
        raise ValueError(message)


def core_apply_policy(approval):
    require('coreReadOnlyTest' not in approval or CORE_TEST_APPLY_ALLOWED is True,
            'core test apply is disabled in factory packages')


def mem_available_bytes():
    try:
        lines = Path('/proc/meminfo').read_text(encoding='ascii').splitlines()
    except (OSError, UnicodeError) as error:
        raise ValueError('cannot read MemAvailable: ' + str(error)) from error
    entries = [line for line in lines if line.startswith('MemAvailable:')]
    require(len(entries) == 1 and re.fullmatch(r'MemAvailable:\s+[0-9]+\s+kB\s*', entries[0]),
            'invalid or missing MemAvailable (expected one nonnegative kB value)')
    return int(entries[0].split()[1]) * 1024


def migration_capacity_preflight(segments, checkpoint):
    # MemAvailable/statvfs already account for all existing sources and retained
    # targets. Charge only new v11 pages; report (do not recharge) retained v10.
    v10, v11 = guard.POINT_STORE_SIZES[10], guard.POINT_STORE_SIZES[11]
    page = os.sysconf('SC_PAGE_SIZE')
    require(type(page) is int and page > 0, 'invalid memory page size')
    rounded = lambda size: ((size + page - 1) // page) * page
    count = len(segments)
    added = count * rounded(v11)
    # Serial copyOfflinePointStoreV10ToV11 keeps before/checked (v10) and
    # output/copied (v11) live together, independently of the new tmpfs pages.
    workspace = 2 * rounded(v10) + 2 * rounded(v11)
    # 64 MiB policy headroom for Python/staging, one serial backup's page cache
    # and kernel/service transients. This is a gate, not a memory reservation.
    reserve = 64 * 1024 * 1024
    available = mem_available_bytes()
    try:
        shm = os.statvfs(Path('/dev/shm'))
    except OSError as error:
        raise ValueError('cannot inspect /dev/shm capacity: ' + str(error)) from error
    require(type(shm.f_frsize) is int and shm.f_frsize > 0 and
            type(shm.f_bavail) is int and shm.f_bavail >= 0, 'invalid /dev/shm capacity')
    shm_available = shm.f_frsize * shm.f_bavail
    report = {'checkpoint': checkpoint, 'uniqueSegments': count,
              'v10LayoutBytes': v10, 'v11LayoutBytes': v11, 'pageBytes': page,
              'retainedSourceV10Bytes': count * v10, 'newTargetV11Bytes': count * v11,
              'migrationWorkBytes': workspace, 'systemReserveBytes': reserve,
              'memRequiredBytes': added + workspace + reserve, 'memAvailableBytes': available,
              'shmRequiredBytes': added, 'shmAvailableBytes': shm_available}
    print('CAPACITY_PRECHECK: ' + json.dumps(report, sort_keys=True), flush=True)
    require(available >= report['memRequiredBytes'], 'insufficient MemAvailable for SHM11 migration')
    require(shm_available >= added, 'insufficient /dev/shm space for new SHM11 targets')


def activation_capacity_preflight(units, checkpoint):
    # Admission-policy floors, NOT measured RSS maxima or cgroup reservations.
    # The 20261001 short 999 sample was RTU/MQTT/Monitor ~17/32/40 MiB RSS:
    # 128 MiB each leaves private startup/queue headroom, not load qualification.
    # Qt gets a larger UI floor; its pinned Requires= bridge is budgeted too.
    # Launcher/watchdog are shell helpers. No new SHM pages or CLI heaps here:
    # existing v10/v11 allocations already reduce current MemAvailable.
    mib = {MONITOR_UNIT: 128, guard.A_RTU_UNIT: 128, guard.A_MQTT_UNIT: 128,
           A_QT_UNIT: 256, 'gateway-services.service': 32, 'gateway-health-watchdog.service': 32}
    require(isinstance(units, list) and units and len(set(units)) == len(units) and
            all(unit in mib for unit in units), 'activation memory policy requires known unique units')
    service_bytes = {unit: mib[unit] * 1024 * 1024 for unit in units}
    if A_QT_UNIT in units:
        service_bytes['qt-display-bridge.service'] = 64 * 1024 * 1024
    # Independent 64 MiB policy slack for Python, systemd and OS transients.
    # Large point sets/UI projects still require separate peak/load evidence.
    reserve = 64 * 1024 * 1024
    required = sum(service_bytes.values()) + reserve
    available = mem_available_bytes()
    print('ACTIVATION_CAPACITY_PRECHECK: ' + json.dumps({
        'checkpoint': checkpoint, 'startUnits': units, 'serviceReserveBytes': service_bytes,
        'systemReserveBytes': reserve, 'memRequiredBytes': required, 'memAvailableBytes': available},
        sort_keys=True), flush=True)
    require(available >= required, 'insufficient MemAvailable for activation')


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


def publish_new(path, data, mode=0o600):
    # Never expose partial final bytes or clobber an unexpected destination.
    # Interrupted staging files remain as evidence and are not systemd .conf files.
    temp = path.with_name('.' + path.name + '.stage-' + uuid.uuid4().hex)
    write_new(temp, data, mode)
    require(regular(temp).read_bytes() == data and stat.S_IMODE(temp.stat().st_mode) == mode,
            'successor staging file changed')
    libc = ctypes.CDLL(None, use_errno=True)
    rename = getattr(libc, 'renameat2', None)
    args = (ctypes.c_int(-100), ctypes.c_char_p(os.fsencode(str(temp))),
            ctypes.c_int(-100), ctypes.c_char_p(os.fsencode(str(path))), ctypes.c_uint(1))
    if rename is not None:
        result = rename(*args)
    else:
        # Older target libc may omit the wrapper; these are the supported Linux ABIs.
        number = {'aarch64': 276, 'x86_64': 316}.get(os.uname().machine)
        require(number is not None, 'atomic no-replace publication unavailable')
        result = libc.syscall(ctypes.c_long(number), *args)
    if result != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(path))
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
    return guard.switch_names(value, names)


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
        if 'coreReadOnlyTest' in approval:
            require(MONITOR_IMPLICIT_SHM in names and not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink(),
                    'core implicit monitor binding must be absent and explicitly migrated')
            return {'old': None, 'new': MONITOR_ENV + names[MONITOR_IMPLICIT_SHM].encode() + b'\n',
                    'mode': 0o644, 'source': MONITOR_IMPLICIT_SHM, 'target': names[MONITOR_IMPLICIT_SHM]}
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
    if record['oldSha256'] is None:
        require('coreReadOnlyTest' in approval and 'systemMonitorShmDropinSha256' not in approval and
                record['source'] == MONITOR_IMPLICIT_SHM and record['mode'] == 0o644 and
                not (state_dir / 'monitor-dropin-old').exists() and
                not (state_dir / 'monitor-dropin-old').is_symlink(), 'invalid absent core monitor binding')
        names = {item['source']: item['target'] for item in approval['segments']}
        require(MONITOR_IMPLICIT_SHM in names and record['target'] == names[MONITOR_IMPLICIT_SHM],
                'core monitor SHM lineage changed')
        new = MONITOR_ENV + record['target'].encode() + b'\n'
        require(record['newSha256'] == hashlib.sha256(new).hexdigest() and
                regular(state_dir / 'monitor-dropin-new').read_bytes() == new, 'core monitor backup binding changed')
        return record, None, new
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


def scada_current_switch(home, approval, new):
    pins = approval['scadaReadOnlyProject']
    current = home / 'scada/current'
    require(current.is_symlink() and os.readlink(str(current)) in (pins['oldTarget'], pins['newTarget']),
            'read-only SCADA current drift; preserve unknown binding')
    side = 'new' if new else 'old'
    guard.scada_readonly_binding(home, approval, side, check_current=False)
    prior = os.readlink(str(current))
    target = pins[side + 'Target']
    if prior != target:
        temporary = current.with_name('.current.offline-' + uuid.uuid4().hex)
        temporary.symlink_to(target)
        sync_dir(current.parent)
        require(current.is_symlink() and os.readlink(str(current)) == prior,
                'read-only SCADA current changed before switch')
        os.replace(str(temporary), str(current))
        sync_dir(current.parent)


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
    if 'scadaReadOnlyProject' in approval:
        guard.scada_readonly_binding(home, approval, 'old')
        guard.scada_readonly_binding(home, approval, 'new', check_current=False)
    app = documents.get('apps/monitor-service.json')
    mqtt = app.get('mqtt') if isinstance(app, dict) else None
    monitor = app.get('systemMonitor') if isinstance(app, dict) else None
    direct = monitor.get('directMaintenance') if isinstance(monitor, dict) else None
    require(isinstance(mqtt, dict) and isinstance(direct, dict) and
            type(mqtt.get('enabled')) is bool and type(direct.get('enabled')) is bool and
            mqtt['enabled'] is direct['enabled'], 'A inbound maintenance source shape changed')
    disable_inbound = mqtt['enabled'] is True
    source_pin = approval.get('aInboundDisableSourceSha256')
    require((disable_inbound and source_pin == approval['configSha256'].get('apps/monitor-service.json') and
             source_pin == digest(home / 'config/runtime/apps/monitor-service.json')) or
            (not disable_inbound and source_pin is None),
            'A inbound maintenance disable approval missing or changed')
    target = names[MONITOR_IMPLICIT_SHM]
    return {'old': old, 'new': a_qt_env_new(old, target),
            'mode': stat.S_IMODE(envfile.stat().st_mode), 'target': target,
            'monitorNew': MONITOR_ENV + target.encode() + b'\n',
            'disableInbound': disable_inbound}


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


def a_joint_config_safe(home, approval, state, selected, documents, project_pending=False):
    require(approval_mode(approval) == 'standalone' and
            (selected in ([MONITOR_UNIT], [MONITOR_UNIT, A_QT_UNIT]) or
             approval.get('aReadonlyAcquisition') is True and selected == guard.A_ACQUISITION_READERS) and
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
            isinstance(camera_app, dict) and camera_app.get('deviceConfigFiles') in (None, []) and
            isinstance(camera_app.get('cameraService'), dict) and
            camera_app['cameraService'].get('enabled') is False and
            all('cameraService' not in doc or
                isinstance(doc['cameraService'], dict) and doc['cameraService'].get('enabled') is False
                for doc in (app, mqtt_app)) and
            isinstance(memory_store, dict) and memory_store.get('sharedMemoryName') in
            {s['target'] for s in state['segments']} and
            isinstance(app.get('systemMonitor'), dict) and app['systemMonitor'].get('enabled') is True,
            'A joint monitor loader or disabled camera shape changed')
    mqtt = app.get('mqtt')
    direct = app['systemMonitor'].get('directMaintenance')
    monitor_mqtt = ('aReadonlyAcquisition' in approval and
                    guard.a_acquisition_profile(approval) == guard.A_WINDOWS_PROFILE)
    require(isinstance(mqtt, dict) and mqtt.get('enabled') is monitor_mqtt and
            isinstance(direct, dict) and direct.get('enabled') is False,
            'A monitor inbound maintenance must be disabled')
    if monitor_mqtt:
        recording = app['systemMonitor'].get('recordingTransfer')
        require(isinstance(recording, dict) and recording.get('enabled') is False,
                'A Windows read-only recording transfer must be disabled')
    if 'scadaReadOnlyProject' in approval:
        scada = app.get('localDisplay', {}).get('scada', {})
        require(scada.get('enabled') is True and scada.get('autoReload') is False and
                scada.get('projectDirectory') == str(home / 'scada/current'),
                'read-only SCADA configuration changed')
        if project_pending:
            guard.scada_readonly_binding(home, approval, 'old')
        else:
            require(state.get('scadaReadOnlyProject') == approval['scadaReadOnlyProject'],
                    'read-only SCADA transaction binding changed')
        guard.scada_readonly_binding(home, approval, 'new', check_current=not project_pending)
    else:
        a_scada_empty_map(home, documents)


def a_joint_monitor_observe_safe(home, approval, state, selected, joint, documents, inhibited):
    record, _, new_env, new_dropin = joint
    a_joint_config_safe(home, approval, state, selected, documents)
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


def batch_stop_results(owned):
    rows = [{'unit': unit, 'stopOk': False, 'activeState': 'unknown', 'mainPid': 'unknown'} for unit in owned]
    try:
        systemctl('stop', *owned)
        for row in rows:
            row['stopOk'] = True
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    try:
        output = systemctl('show', '--property=Id,ActiveState,MainPID', *owned)
        records = {}
        for block in output.split('\n\n'):
            fields = dict(line.split('=', 1) for line in block.splitlines())
            require(set(fields) == {'Id', 'ActiveState', 'MainPID'} and
                    fields['Id'] in owned and fields['Id'] not in records, 'unexpected offline stop status')
            records[fields['Id']] = fields
        require(set(records) == set(owned), 'incomplete offline stop status')
        for row in rows:
            row['activeState'] = records[row['unit']]['ActiveState']
            row['mainPid'] = records[row['unit']]['MainPID']
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return rows


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
    preserved = approval.get('coreReadOnlyTest', {}).get('preservedRuntimeSha256', {})
    require(set(installed) <= set(paths.values()) | set(preserved), 'installed runtime absent from approved paths')
    require(all(installed.get(p) == pin for p, pin in preserved.items()), 'preserved runtime inventory changed')
    require(installed == approval.get('installedRuntimeSha256'), 'installed runtime inventory changed')
    for relative in paths.values():
        destination = home / relative
        require(not destination.is_symlink() and not destination.parent.is_symlink(), 'symlink destination refused')
        require(not destination.exists() or relative in installed, 'unapproved existing destination')


def activation_scripts(approval):
    return guard.CORE_SCRIPTS if 'coreReadOnlyTest' in approval else ACTIVATION_SCRIPTS


def core_sources_unchanged(approval, identities):
    for segment in approval['segments']:
        path = regular(Path('/dev/shm') / segment['source'])
        value = path.stat()
        require(identities[segment['source']] == {'dev': value.st_dev, 'ino': value.st_ino},
                'core source SHM identity changed: ' + segment['source'])
        require(digest(path) == segment['sha256'], 'core source SHM bytes changed')
        guard.point_store_abi(path, 10)


def activation_script_pins(home, approval, side):
    pins = approval.get('activationStartupScripts')
    if pins is None:
        return None
    require(isinstance(pins, dict) and set(pins) == set(activation_scripts(approval)),
            'activation startup scripts require exact paired-file approval')
    for name in activation_scripts(approval):
        pin = pins[name]
        require(isinstance(pin, dict) and set(pin) == {'oldSha256', 'newSha256', 'mode'} and
                type(pin['mode']) is int and 0 < pin['mode'] <= 0o777 and
                (pin['oldSha256'] is None and name in ('runtime-upgrade-guard.py', 'ota-health-result.py') or
                 isinstance(pin['oldSha256'], str) and re.fullmatch('[0-9a-f]{64}', pin['oldSha256'])) and
                re.fullmatch('[0-9a-f]{64}', pin['newSha256']) and
                digest(Path(__file__).with_name(name)) == pin['newSha256'],
                'activation startup candidate not pinned: ' + name)
        live = home / 'bin' / name
        if side == 'old' and pin['oldSha256'] is None:
            require(not any(part.is_symlink() for part in (live,) + tuple(live.parents)) and
                    live.parent.is_dir() and not live.exists(), 'unexpected old activation guard')
        else:
            require(digest(regular(live)) == pin[side + 'Sha256'] and
                    stat.S_IMODE(live.stat().st_mode) == pin['mode'],
                    'activation startup installed script changed: ' + name)
    return pins


def no_mappings(names, include_fds=False):
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
            if include_fds:
                for fd in (entry / 'fd').iterdir():
                    try:
                        value = fd.stat()
                        require((os.major(value.st_dev), os.minor(value.st_dev), value.st_ino) not in identities,
                                'SHM still open by pid ' + entry.name)
                    except (FileNotFoundError, ProcessLookupError):
                        continue
        except (FileNotFoundError, ProcessLookupError):
            continue


def read_inputs(args):
    approval = checked_json(args.approval, args.approval_sha256)
    core_apply_policy(approval)
    approval_mode(approval)
    require(guard.compatibility(approval) is not None, 'runtimeCompatibility required')
    require(approval.get('controlEnabled') is False, 'control must remain disabled')
    require(NAME.fullmatch(approval.get('transactionId', '')) is not None, 'unsafe transaction id')
    home = Path(approval['gatewayHome'])
    require(re.fullmatch('/[A-Za-z0-9_/-]+', str(home)) and home.resolve() == home and
            str(home) not in ('/', '/opt'), 'unsafe gateway home')
    if 'aReadonlyAcquisition' in approval or 'aReadonlyWindowsIntegration' in approval:
        guard.a_acquisition_scope(home, approval)
    if 'coreReadOnlyTest' in approval:
        guard.core_scope(home, approval)
        require(approval.get('activationStartupScripts') is not None, 'core requires paired startup/runtime scripts')
    activation_script_pins(home, approval, 'old')
    manifest = checked_json(args.manifest, approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    if 'coreReadOnlyTest' in approval:
        guard.core_scope(home, approval, manifest)
    paths = install_paths(components, approval)
    require('memory_point_store_migrate' in paths, 'complete product set with migration CLI required')
    for component in components:
        require(NAME.fullmatch(component['target']) is not None, 'unsafe component target')
        path = args.payload / component['archivePath']
        require(args.payload in path.resolve().parents, 'component escapes payload')
        require(digest(path) == component['sha256'] and path.stat().st_size == component['bytes'],
                'component hash/size mismatch: ' + component['target'])
        if 'coreReadOnlyTest' in approval:
            with regular(path).open('rb') as stream: header = stream.read(20)
            require(header[:6] == b'\x7fELF\x02\x01' and header[18:20] == b'\xb7\x00',
                    'core candidate must be AArch64 ELF64 little-endian: ' + component['target'])
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


def recovered_predecessor(home, approval, state_dir):
    link = approval['recoveredFrom']
    require(approval_mode(approval) == 'standalone' and isinstance(link, dict) and set(link) ==
            {'stateDir', 'stateSha256', 'approvalSha256', 'receiptPath', 'receiptSha256'},
            'recovered predecessor requires exact standalone pins')
    previous = Path(link['stateDir'])
    require(previous.is_absolute() and previous.resolve() == previous and previous != state_dir and
            previous not in state_dir.parents and state_dir not in previous.parents and
            home != previous and home not in previous.parents and previous not in home.parents,
            'recovered predecessor state directory overlaps transaction')
    old = checked_json(previous / 'approval.json', link['approvalSha256'])
    ancestor, directory = old, previous
    seen_ids, seen_dirs = {approval['transactionId']}, {state_dir}
    while True:
        transaction = ancestor.get('transactionId', '')
        require(isinstance(transaction, str) and NAME.fullmatch(transaction) and transaction not in seen_ids,
                'recovered predecessor ancestor transaction ID reused')
        require(directory.is_absolute() and directory.resolve() == directory and directory not in seen_dirs and
                all(directory not in p.parents and p not in directory.parents for p in seen_dirs | {home}) and
                directory != home and approval_mode(ancestor) == 'standalone' and
                ancestor.get('controlEnabled') is False and
                all(ancestor.get(key) == approval.get(key) for key in ('gatewayHome', 'nodeId')),
                'recovered predecessor ancestor scope or directory changed')
        seen_ids.add(transaction)
        seen_dirs.add(directory)
        if 'recoveredFrom' not in ancestor:
            break
        parent = ancestor['recoveredFrom']
        require(isinstance(parent, dict) and set(parent) == set(link), 'invalid recovered ancestor pins')
        directory = Path(parent['stateDir'])
        ancestor = checked_json(directory / 'approval.json', parent['approvalSha256'])
    recovered = checked_json(previous / 'state.json', link['stateSha256'])
    receipt = checked_json(Path(link['receiptPath']), link['receiptSha256'])
    require(approval_mode(old) == 'standalone' and recovered.get('phase') == 'RECOVERED_STOPPED' and
            recovered.get('approvalSha256') == link['approvalSha256'] and
            old.get('controlEnabled') is False and NAME.fullmatch(old.get('transactionId', '')) and
            old['transactionId'] != approval['transactionId'] and
            all(old.get(key) == approval.get(key) for key in
                ('gatewayHome', 'nodeId', 'identitySha256', 'configSha256', 'installedRuntimeSha256',
                 'systemMonitorShmDropinSha256', 'qtDisplayEnvSha256', 'aInboundDisableSourceSha256')) and
            set(old['units']) == set(approval['units']), 'recovered predecessor scope or phase changed')
    require(receipt.get('schemaVersion') == 'offline-shm11-recovered-1' and
            receipt.get('phase') == 'RECOVERED_STOPPED' and receipt.get('controlEnabled') is False and
            receipt.get('stateSha256') == link['stateSha256'] and
            receipt.get('approvalSha256') == link['approvalSha256'] and
            all(receipt.get(key) == old[key] for key in ('transactionId', 'nodeId', 'gatewayHome')),
            'recovered predecessor receipt lineage changed')
    old_sources = {s['source']: s for s in old['segments']}
    require(len(old_sources) == len(old['segments']) and
            all(NAME.fullmatch(s[key]) for s in old['segments'] for key in ('source', 'target')) and
            {s['source']: s['sha256'] for s in approval['segments']} ==
            {name: s['sha256'] for name, s in old_sources.items()} and
            not {s['target'] for s in approval['segments']} &
            {s[key] for s in old['segments'] for key in ('source', 'target')},
            'recovered predecessor sources changed or target reused')
    retained = receipt.get('retainedTargetsSha256')
    require(isinstance(retained, dict) and set(retained) ==
            {s['target'] for s in old['segments'] if (Path('/dev/shm') / s['target']).exists() or
             (Path('/dev/shm') / s['target']).is_symlink()}, 'recovered predecessor retained target set changed')
    require(all(s['source'] in old_sources and s['target'] == old_sources[s['source']]['target'] and
                s['sourceSha256'] == old_sources[s['source']]['sha256'] and s['target'] in retained
                for s in recovered['segments']), 'recovered predecessor completed target missing or changed')
    for segment in old['segments']:
        require(NAME.fullmatch(segment['source']) and NAME.fullmatch(segment['target']),
                'unsafe recovered predecessor SHM name')
        require(digest(Path('/dev/shm') / segment['source']) == segment['sha256'],
                'recovered predecessor source changed')
        backup = previous / (segment['source'] + '.v10.bak')
        if segment['target'] in retained or backup.exists() or backup.is_symlink():
            require(digest(backup) == segment['sha256'], 'recovered predecessor SHM backup changed')
        if segment['target'] in retained:
            require(digest(Path('/dev/shm') / segment['target']) == retained[segment['target']],
                    'recovered predecessor retained target changed')
    manifest = checked_json(previous / 'program-manifest.json', old['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, old)
    installed_inventory(home, paths, old)
    require(tree_hashes(home / 'config/runtime') == old['configSha256'],
            'recovered predecessor configuration not restored')
    for index, item in enumerate(recovered['files']):
        relative = Path(item['path'])
        require(not relative.is_absolute() and '..' not in relative.parts and
                (str(relative) in paths.values() or relative.parts[:2] == ('config', 'runtime') or
                 str(relative) in {'bin/' + name for name in activation_scripts(old)}),
                'unsafe recovered predecessor backup path')
        live = home / relative
        if item['oldSha256'] is not None:
            require(digest(previous / 'files' / str(index)) == item['oldSha256'] and
                    digest(live) == item['oldSha256'] and stat.S_IMODE(live.stat().st_mode) == item['oldMode'],
                    'recovered predecessor file or backup changed')
        else:
            require(not live.exists() and not live.is_symlink(), 'recovered predecessor absence changed')
            archived = previous / ('recovered-new-' + str(index))
            if archived.exists() or archived.is_symlink():
                require(digest(archived) == item['newSha256'], 'recovered predecessor retained file changed')
    monitor = checked_monitor_record(previous, recovered, old)
    joint = checked_a_joint_record(previous, recovered, old)
    if 'scadaReadOnlyProject' in old:
        require(recovered.get('scadaReadOnlyProject') == old['scadaReadOnlyProject'],
                'recovered read-only SCADA binding state changed')
        guard.scada_readonly_binding(home, old, 'old')
    if monitor:
        require((not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink()) if monitor[1] is None else
                digest(MONITOR_DROPIN) == monitor[0]['oldSha256'] and
                stat.S_IMODE(MONITOR_DROPIN.stat().st_mode) == monitor[0]['mode'],
                'recovered predecessor monitor binding not restored')
    if joint:
        require(not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink() and
                digest(home / 'config/runtime/qt-display.env') == joint[0]['oldSha256'],
                'recovered predecessor Qt binding not restored')
    require(not (home / 'data' / guard.ACTIVATION_FILE).exists() and
            not (home / 'data' / guard.ACTIVATION_FILE).is_symlink(), 'recovered predecessor still activated')
    return old, receipt


def successor_handoff(home, approval, state_dir, state=None):
    old, receipt = recovered_predecessor(home, approval, state_dir)
    marker = home / 'data/runtime-upgrade-stop'
    owner = regular(marker).read_text().strip()
    require(owner == old['transactionId'] or state is not None and owner == approval['transactionId'],
            'recovered predecessor marker changed')
    pairs = []
    for unit in approval['units']:
        previous, body = inhibition_dropin(home, old['transactionId'], unit)
        current, _ = inhibition_dropin(home, approval['transactionId'], unit)
        archived = state_dir / 'previous-fences' / (unit + '.conf')
        for path in (previous, current, archived):
            require(not any(part.is_symlink() for part in (path,) + tuple(path.parents)),
                    'recovered predecessor fence alias: ' + str(path))
            if path.exists() or path.is_symlink():
                require(regular(path).read_bytes() == body and stat.S_IMODE(path.stat().st_mode) == 0o644,
                        'recovered predecessor fence drift: ' + str(path))
        require(previous.exists() or owner == approval['transactionId'] and archived.exists(),
                'recovered predecessor fence missing')
        require(state is not None or not current.exists(), 'successor fence already exists')
        pairs.append((previous, current, archived, body))
    no_processes(home, [])
    no_mappings(list(s['source'] for s in old['segments']) + list(receipt['retainedTargetsSha256']))
    if state is None:
        return
    state['handoffStopResult'] = batch_stop_results([u for u in approval['units'] if not u.endswith('@.service')])
    save(state_dir / 'state.json', state)
    require(all(r['stopOk'] and r['activeState'] in ('inactive', 'failed') and r['mainPid'] == '0'
                for r in state['handoffStopResult']), 'successor stop unconfirmed')
    recovered_predecessor(home, approval, state_dir)
    no_processes(home, [])
    no_mappings(list(s['source'] for s in old['segments']) + list(receipt['retainedTargetsSha256']))
    (state_dir / 'previous-fences').mkdir(exist_ok=True)
    for previous, _, archived, body in pairs:
        if previous.exists() or previous.is_symlink():
            require(regular(previous).read_bytes() == body and stat.S_IMODE(previous.stat().st_mode) == 0o644,
                    'predecessor fence changed during stop')
        if not archived.exists():
            publish_new(archived, regular(previous).read_bytes(), 0o644)
        require(regular(archived).read_bytes() == body and stat.S_IMODE(archived.stat().st_mode) == 0o644,
                'successor fence backup changed')
    receipt_file = state_dir / 'predecessor-receipt.json'
    if not receipt_file.exists():
        publish_new(receipt_file, regular(Path(approval['recoveredFrom']['receiptPath'])).read_bytes())
    require(digest(receipt_file) == approval['recoveredFrom']['receiptSha256'], 'successor receipt backup changed')
    require(regular(marker).read_text().strip() == owner, 'successor marker changed during stop')
    # The old Condition checks marker existence, so this replacement has no unfenced interval.
    replace(marker, (approval['transactionId'] + '\n').encode())
    for _, current, _, body in pairs:
        if not current.exists():
            publish_new(current, body, 0o644)
        require(regular(current).read_bytes() == body and stat.S_IMODE(current.stat().st_mode) == 0o644,
                'successor fence changed')
    for previous, _, _, body in pairs:
        if previous.exists() or previous.is_symlink():
            require(regular(previous).read_bytes() == body and stat.S_IMODE(previous.stat().st_mode) == 0o644,
                    'predecessor fence changed before archive')
            previous.unlink()
            sync_dir(previous.parent)
    systemctl('daemon-reload')
    state['successorHandoffComplete'] = True
    state['phase'] = 'FENCED'
    save(state_dir / 'state.json', state)


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
    if 'recoveredFrom' in approval:
        rows = batch_stop_results([u for u in approval['units'] if not u.endswith('@.service')])
        state['handoffStopResult'] = rows
        stopped = all(r['stopOk'] and r['activeState'] in ('inactive', 'failed') and r['mainPid'] == '0' for r in rows)
        if not stopped:
            state['phase'] = 'FAILED_STOP_UNCONFIRMED'
        save(state_dir / 'state.json', state)
        require(stopped, 'successor stop unconfirmed')
    else:
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
    core = 'coreReadOnlyTest' in approval
    source_identities = {}
    if core:
        guard.core_documents_safe(home, approval, documents)
        require(approval.get('systemMonitorShmDropinSha256') is not None or MONITOR_IMPLICIT_SHM in names,
                'core must cover implicit monitor SHM')
        for segment in segments:
            path = Path('/dev/shm') / segment['source']
            guard.point_store_abi(path, 10)
            value = path.stat()
            source_identities[segment['source']] = {'dev': value.st_dev, 'ino': value.st_ino}
        no_processes(home, [c['target'] for c in components])
        no_mappings(list(names), include_fds=True)
        units_stopped(approval['units'])
        guard.core_denied_units(approval, [])
    if MONITOR_IMPLICIT_SHM in names and not core:
        targets = {item['target'] for item in components}
        require(approval_mode(approval) == 'standalone' and
                {MONITOR_UNIT, 'ky-ems.service'} <= set(approval['units']) and
                {'SystemMonitor', 'LocalDisplayQtEms'} <= targets and
                approval['installPaths']['LocalDisplayQtEms'] == 'ky-ems/KY-EMS' and
                not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink() and
                'systemMonitorShmDropinSha256' not in approval,
                'unbound implicit SystemMonitor SHM requires stopped monitor and KY-EMS scope')
    binding = monitor_binding(approval, names)
    a_binding = None if core else a_joint_binding(home, approval, names, documents)
    if 'aReadonlyAcquisition' in approval:
        guard.a_acquisition_scope(home, approval)
        require(a_binding is not None and a_binding['disableInbound'] and
                approval.get('activationStartupScripts') is not None and
                {key for key in documents if key.startswith(('apps/', 'devices/'))} == set(guard.A_SOURCE_CONFIGS),
                'A acquisition requires pinned scripts, original inbound settings and exact config inventory')
        for relative, pin in guard.A_SOURCE_CONFIGS.items():
            require(digest(configs / relative) == pin, 'A acquisition original config changed: ' + relative)
        require({'ModbusRtu', 'MqttDriver'} <= {c['target'] for c in components} and
                all(approval['installPaths'].get(name) == 'bin/' + name for name in ('ModbusRtu', 'MqttDriver')),
                'A acquisition requires both pinned driver components')
    require('scadaReadOnlyProject' not in approval or a_binding is not None or core,
            'read-only SCADA approval requires A joint profile')
    if 'scadaReadOnlyProject' in approval and not core:
        projected = {key: switch_names(doc, names) for key, doc in documents.items()}
        monitor_app = projected['apps/monitor-service.json']
        monitor_app['localDisplay']['scada']['autoReload'] = False
        if a_binding['disableInbound']:
            monitor_app['mqtt']['enabled'] = guard.a_acquisition_profile(approval) == guard.A_WINDOWS_PROFILE
            monitor_app['systemMonitor']['directMaintenance']['enabled'] = False
        if 'aReadonlyAcquisition' in approval:
            guard.a_acquisition_patch('apps/monitor-service.json', monitor_app, approval)
        a_joint_config_safe(home, approval, {'segments': segments, 'unreferencedDefaultShm': unreferenced_default},
                            [MONITOR_UNIT, A_QT_UNIT], projected, project_pending=True)
    require('aInboundDisableSourceSha256' not in approval or a_binding is not None or core,
            'A inbound maintenance disable approval requires A joint binding')
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
    migration_capacity_preflight(segments, 'initial')
    if 'recoveredFrom' in approval:
        require(approval_mode(approval) == 'standalone' and approval.get('activationStartupScripts') is not None,
                'successor requires standalone activation script approval')
        projected = {key: switch_names(doc, names) for key, doc in documents.items()}
        profile_state = {'segments': segments, 'unreferencedDefaultShm': unreferenced_default}
        if core:
            guard.core_documents_safe(home, approval, projected)
        elif a_binding:
            if a_binding['disableInbound']:
                projected['apps/monitor-service.json']['mqtt']['enabled'] = guard.a_acquisition_profile(approval) == guard.A_WINDOWS_PROFILE
                projected['apps/monitor-service.json']['systemMonitor']['directMaintenance']['enabled'] = False
            if 'aReadonlyAcquisition' in approval:
                guard.a_acquisition_patch('apps/monitor-service.json', projected['apps/monitor-service.json'], approval)
            if 'scadaReadOnlyProject' in approval:
                projected['apps/monitor-service.json']['localDisplay']['scada']['autoReload'] = False
            a_joint_config_safe(home, approval, profile_state, [MONITOR_UNIT, A_QT_UNIT], projected, project_pending=True)
        else:
            require(binding is not None, 'successor requires pinned A or B monitor profile')
            b_monitor_default_safe(home, approval, manifest, profile_state, [MONITOR_UNIT], (binding,), projected)
        successor_handoff(home, approval, args.state)
    # main holds the existing flock throughout apply. Re-read after all read-only
    # predecessor gates, immediately before the first transaction/fence mutation.
    migration_capacity_preflight(segments, 'before-state')
    args.state.mkdir(mode=0o700)
    write_new(args.state / 'approval.json', regular(args.approval).read_bytes())
    write_new(args.state / 'program-manifest.json', regular(args.manifest).read_bytes())
    state = {'phase': 'PREPARED', 'stopConfirmed': False, 'approvalSha256': args.approval_sha256, 'files': [], 'segments': [],
             'unreferencedDefaultShm': unreferenced_default,
             'implicitMonitorShmUnqualified': MONITOR_IMPLICIT_SHM in names and a_binding is None and not core,
             'unitStates': {u: ({'enabled': 'template-file', 'active': 'not-instance'} if u.endswith('@.service') else
                               {'enabled': systemctl('show', '--property=UnitFileState', '--value', u),
                                'active': systemctl('show', '--property=ActiveState', '--value', u)}) for u in approval['units']}}
    if 'recoveredFrom' in approval:
        state['successorHandoffComplete'] = False
    if core:
        state['coreSourceIdentities'] = source_identities
    if 'scadaReadOnlyProject' in approval:
        state['scadaReadOnlyProject'] = approval['scadaReadOnlyProject']
    save(args.state / 'state.json', state)
    try:
        if 'recoveredFrom' in approval:
            successor_handoff(home, approval, args.state, state)
        else:
            fence(home, approval, args.state, state)
        no_processes(home, [c['target'] for c in components])
        require(not (monitor_path.exists() or monitor_path.is_symlink()) or MONITOR_IMPLICIT_SHM in names,
                'uncovered implicit SystemMonitor SHM segment after stop')
        state['stopConfirmed'] = True
        require(tree_hashes(configs) == approval['configSha256'], 'config changed during stop')
        if core:
            core_sources_unchanged(approval, source_identities)
        if binding:
            if binding['old'] is None:
                require(not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink(), 'core monitor binding appeared')
            else:
                path = regular(MONITOR_DROPIN)
                require(path.read_bytes() == binding['old'] and stat.S_IMODE(path.stat().st_mode) == binding['mode'],
                        'monitor SHM drop-in changed during stop')
                write_new(args.state / 'monitor-dropin-old', binding['old'])
            write_new(args.state / 'monitor-dropin-new', binding['new'])
            state['systemMonitorShmDropin'] = {
                'oldSha256': hashlib.sha256(binding['old']).hexdigest() if binding['old'] is not None else None,
                'newSha256': hashlib.sha256(binding['new']).hexdigest(),
                'source': binding['source'], 'target': binding['target'], 'mode': binding['mode']}
            save(args.state / 'state.json', state)
        if a_binding:
            require(regular(home / 'config/runtime/qt-display.env').read_bytes() == a_binding['old'] and
                    stat.S_IMODE((home / 'config/runtime/qt-display.env').stat().st_mode) == a_binding['mode'],
                    'A Qt monitor environment changed during stop')
            a_scada_empty_map(home, configured(configs))
            if 'scadaReadOnlyProject' in approval:
                guard.scada_readonly_binding(home, approval, 'old')
                guard.scada_readonly_binding(home, approval, 'new', check_current=False)
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
        if approval.get('activationStartupScripts') is not None:
            activation_script_pins(home, approval, 'old')
            for name in activation_scripts(approval):
                changed.append((home / 'bin' / name, regular(Path(__file__).with_name(name)).read_bytes(),
                                approval['activationStartupScripts'][name]['mode']))
        for relative, doc in documents.items():
            updated = switch_names(doc, names)
            if approval.get('aReadonlyAcquisition') is True:
                guard.a_acquisition_patch(relative, updated, approval)
            if a_binding and a_binding['disableInbound'] and relative == 'apps/monitor-service.json':
                updated['mqtt']['enabled'] = guard.a_acquisition_profile(approval) == guard.A_WINDOWS_PROFILE
                updated['systemMonitor']['directMaintenance']['enabled'] = False
            if 'scadaReadOnlyProject' in approval and not core and relative == 'apps/monitor-service.json':
                updated['localDisplay']['scada']['autoReload'] = False
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
            if core:
                core_sources_unchanged(approval, source_identities)
            command = [str(cli_path), '--shm', segment['source'], '--copy-to-v11', segment['target'],
                       '--offline-confirmed', '--backup', str(args.state / (segment['source'] + '.v10.bak'))]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            write_new(args.state / (segment['source'] + '.migration.log'), result.stdout)
            require(result.returncode == 0, 'migration refused/failed for ' + segment['source'] + ': ' + result.stdout.decode(errors='replace'))
            require(digest(source) == segment['sha256'], 'migration changed source')
            if core:
                core_sources_unchanged(approval, source_identities)
                guard.point_store_abi(source, 10)
                guard.point_store_abi(Path('/dev/shm') / segment['target'], 11)
                require(digest(args.state / (segment['source'] + '.v10.bak')) == segment['sha256'],
                        'core durable SHM backup verification failed')
            state['segments'].append({'source': segment['source'], 'target': segment['target'],
                                      'sourceSha256': digest(source), 'targetSha256': digest(Path('/dev/shm') / segment['target'])})
            save(args.state / 'state.json', state)
        state['phase'] = 'SWITCHING'
        save(args.state / 'state.json', state)
        no_processes(home, [c['target'] for c in components])
        if core:
            core_sources_unchanged(approval, source_identities)
            no_mappings([s[k] for s in segments for k in ('source', 'target')], include_fds=True)
            for segment in segments:
                require(digest(Path('/dev/shm') / segment['source']) == segment['sha256'], 'core source changed before replace')
                guard.point_store_abi(Path('/dev/shm') / segment['target'], 11)
        if binding:
            require((not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink()) if binding['old'] is None else
                    regular(MONITOR_DROPIN).read_bytes() == binding['old'],
                    'monitor SHM drop-in changed before switch')
            require(digest(args.state / 'monitor-dropin-new') == state['systemMonitorShmDropin']['newSha256'],
                    'monitor SHM staged drop-in changed')
            (write_new if binding['old'] is None else replace)(
                MONITOR_DROPIN, regular(args.state / 'monitor-dropin-new').read_bytes(), binding['mode'])
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
        if 'scadaReadOnlyProject' in approval and not core:
            guard.scada_readonly_binding(home, approval, 'old')
            scada_current_switch(home, approval, True)
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
    core = 'coreReadOnlyTest' in approval
    local_identity(home, approval)
    if 'recoveredFrom' in approval and not state.get('successorHandoffComplete', False):
        successor_handoff(home, approval, args.state, state)
    elif state.get('activationProfile') in ('B_MONITOR', 'A_MONITOR_QT', guard.A_ACQUISITION_PROFILE, guard.A_WINDOWS_PROFILE, guard.CORE_PROFILE):
        refence_activation(home, approval, args.state, state)
    else:
        fence(home, approval, args.state, state)
    no_processes(home, [])
    if 'scadaReadOnlyProject' in approval:
        require(state.get('scadaReadOnlyProject') == approval['scadaReadOnlyProject'],
                'read-only SCADA transaction binding changed')
    manifest = checked_json(args.state / 'program-manifest.json', approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, approval)
    products = {paths[c['target']]: c for c in components}
    monitor = checked_monitor_record(args.state, state, approval)
    if monitor:
        record, _, _ = monitor
        require(not MONITOR_DROPIN.exists() and not MONITOR_DROPIN.is_symlink() and monitor[1] is None or
                digest(MONITOR_DROPIN) in (record['oldSha256'], record['newSha256']),
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
                (str(relative) in products or relative.parts[:2] == ('config', 'runtime') or
                 str(relative) in {'bin/' + name for name in activation_scripts(approval)}) and
                relative != Path('config/runtime/device_identity.json'), 'unsafe recovery path')
        if str(relative) in products:
            require(item['newSha256'] == products[str(relative)]['sha256'] and
                    item['oldSha256'] == approval['installedRuntimeSha256'].get(str(relative)), 'unapproved binary recovery')
        elif str(relative) in {'bin/' + name for name in activation_scripts(approval)}:
            pins = approval.get('activationStartupScripts')
            pin = pins.get(relative.name) if isinstance(pins, dict) else None
            require(isinstance(pin, dict) and item['oldSha256'] == pin.get('oldSha256') and
                    item['newSha256'] == pin.get('newSha256') and
                    item['oldMode'] == (None if pin.get('oldSha256') is None else pin.get('mode')),
                    'unapproved activation script recovery')
        else:
            require(item['oldSha256'] == approval['configSha256'].get(str(relative.relative_to('config/runtime'))),
                    'unapproved config recovery')
        if item['oldSha256']:
            require(digest(args.state / 'files' / str(index)) == item['oldSha256'], 'recovery backup mismatch')
        live = home / relative
        require(not live.exists() or digest(live) in (item['oldSha256'], item['newSha256']), 'live file changed since transaction')
    if 'scadaReadOnlyProject' in approval and not core:
        # Stop and validate backups first; retain any changed candidate bytes.
        scada_current_switch(home, approval, False)
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
        if old is None:
            if MONITOR_DROPIN.exists() or MONITOR_DROPIN.is_symlink():
                require(regular(MONITOR_DROPIN).read_bytes() == monitor[2], 'core monitor binding changed before recovery')
                MONITOR_DROPIN.unlink()
                sync_dir(MONITOR_DROPIN.parent)
        else:
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
    require('aReadonlyAcquisition' not in approval, 'A acquisition requires combined activate, not observe')
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
    no_mappings([s[k] for s in state['segments'] for k in ('source', 'target')],
                include_fds='coreReadOnlyTest' in approval)
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


def activation_units(approval, readers):
    if 'coreReadOnlyTest' in approval:
        require(readers == guard.core_scope(Path(approval['gatewayHome']), approval),
                'core requires the exact approved reader group')
        return ['gateway-services.service'] + readers
    if 'aReadonlyAcquisition' in approval:
        require(approval['aReadonlyAcquisition'] is True and readers == guard.A_ACQUISITION_READERS,
                'A acquisition requires the exact combined group')
    watchdog = ('gateway-health-watchdog.service' in approval['units'] and readers == [MONITOR_UNIT])
    return ['gateway-services.service'] + (['gateway-health-watchdog.service'] if watchdog else []) + readers


def inhibition_dropin(home, transaction, unit):
    path = Path('/etc/systemd/system') / (unit + '.d') / ('90-offline-' + transaction + '.conf')
    body = ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n').encode()
    return path, body


def refence_activation(home, approval, state_dir, state):
    allowed = state.get('activatedUnits', [])
    require(isinstance(allowed, list) and allowed == activation_units(approval,
            guard.core_scope(home, approval) if state['activationProfile'] == guard.CORE_PROFILE else
            guard.A_ACQUISITION_READERS if state['activationProfile'] in (guard.A_ACQUISITION_PROFILE, guard.A_WINDOWS_PROFILE) else
            [MONITOR_UNIT] if state['activationProfile'] == 'B_MONITOR' else [MONITOR_UNIT, A_QT_UNIT]),
            'invalid activation recovery scope')
    require(regular(home / 'data/runtime-upgrade-stop').read_text().strip() == approval['transactionId'],
            'activation recovery marker changed')
    active_path = home / 'data' / guard.ACTIVATION_FILE
    if active_path.exists() or active_path.is_symlink():
        record = json.loads(regular(active_path).read_text())
        require(record.get('stateDir') == str(state_dir) and
                record.get('transactionId') == approval['transactionId'] and
                record.get('allowedUnits') == allowed, 'foreign activation record')
    owned = [unit for unit in approval['units'] if not unit.endswith('@.service')]
    require(set(allowed) <= set(owned), 'activation recovery unit scope changed')
    stop_results = batch_stop_results(owned)
    state['activationStopResult'] = stop_results
    drift = []
    for unit in approval['units']:
        path, inhibited = inhibition_dropin(home, approval['transactionId'], unit)
        _, active = (guard.active_dropin(home, approval['transactionId'], unit) if unit in allowed else
                     guard.denied_dropin(home, approval['transactionId'], unit))
        try:
            current = regular(path).read_bytes()
            if current not in (inhibited, active):
                drift.append(unit)
            elif current != inhibited:
                replace(path, inhibited, 0o644)
        except (OSError, ValueError):
            drift.append(unit)
    try:
        systemctl('daemon-reload')
    except (OSError, ValueError, subprocess.SubprocessError):
        drift.append('daemon-reload')
    if active_path.exists():
        active_path.unlink()
        sync_dir(active_path.parent)
    stopped = all(row['stopOk'] and row['activeState'] in ('inactive', 'failed') and row['mainPid'] == '0'
                  for row in stop_results)
    if not stopped or drift:
        state['phase'] = 'FAILED_STOP_UNCONFIRMED'
        state['activationFenceDrift'] = drift
        save(state_dir / 'state.json', state)
        raise ValueError('activation refence unconfirmed; stop results retained; drift=' + ','.join(drift))
    save(state_dir / 'state.json', state)


def activate(args):
    require(args.ready and args.ready_sha256, 'activate requires separate fixed approval')
    ready = checked_json(args.ready, args.ready_sha256)
    require(ready.get('schemaVersion') == 'offline-shm11-activate-1' and
            ready.get('controlEnabled') is False and type(ready.get('expiresAtUnix')) is int and
            time.time() < ready['expiresAtUnix'], 'invalid/expired activation approval')
    state = checked_json(args.state / 'state.json', ready['stateSha256'])
    require(state['phase'] == 'UPGRADED_STOPPED' and state['approvalSha256'] == args.approval_sha256,
            'activate requires unchanged upgraded/stopped state')
    approval = checked_json(args.state / 'approval.json', args.approval_sha256)
    home = Path(approval['gatewayHome'])
    core = 'coreReadOnlyTest' in approval
    mode = approval_mode(approval)
    require(mode == 'standalone' and ready.get('mode') == mode and
            ready.get('transactionId') == approval['transactionId'] and
            ready.get('approvalSha256') == args.approval_sha256 and
            ready.get('programManifestSha256') == approval['programManifestSha256'] and
            'voters' not in ready and isinstance(ready.get('local'), dict) and
            ready['local'].get('nodeId') == approval['nodeId'] and
            ready['local'].get('phase') == 'UPGRADED_STOPPED' and
            ready['local'].get('stateSha256') == ready['stateSha256'] and
            ready['local'].get('programManifestSha256') == approval['programManifestSha256'] and
            ready['local'].get('controlEnabled') is False,
            'activation ready lineage or participant changed')
    local_identity(home, approval)
    require(regular(home / 'data/runtime-upgrade-stop').read_text().strip() == approval['transactionId'] and
            not (home / 'data' / guard.ACTIVATION_FILE).exists(), 'activation fence/record changed')
    require(activation_script_pins(home, approval, 'new') is not None and
            {'bin/' + name for name in activation_scripts(approval)} <= {item['path'] for item in state['files']},
            'activation requires installed paired startup scripts')
    manifest = checked_json(args.state / 'program-manifest.json', approval['programManifestSha256'])
    components = [c for c in manifest['components'] if c.get('kind') == 'product']
    paths = install_paths(components, approval)
    for component in components:
        require(digest(home / paths[component['target']]) == component['sha256'],
                'installed component no longer approved')
    expected = {paths[c['target']]: c['sha256'] for c in components}
    inventory = {'installedRuntimeSha256': expected}
    if core:
        expected.update(approval['coreReadOnlyTest']['preservedRuntimeSha256'])
        inventory['coreReadOnlyTest'] = approval['coreReadOnlyTest']
        guard.core_scope(home, approval, manifest)
        core_sources_unchanged(approval, state['coreSourceIdentities'])
    installed_inventory(home, paths, inventory)
    require(tree_hashes(home / 'config/runtime') == state['configSha256'], 'activated configuration changed')
    documents = configured(home / 'config/runtime')
    for doc in documents.values():
        controls_disabled(doc)
    for segment in state['segments']:
        for side in ('source', 'target'):
            require(digest(Path('/dev/shm') / segment[side]) == segment[side + 'Sha256'],
                    'SHM changed before activation')
    no_processes(home, [c['target'] for c in components])
    no_mappings([s[k] for s in state['segments'] for k in ('source', 'target')], include_fds=core)
    units_stopped(approval['units'])
    readers = ready.get('startUnits')
    monitor = checked_monitor_record(args.state, state, approval)
    joint = checked_a_joint_record(args.state, state, approval)
    if core:
        require(monitor and not joint and readers == guard.core_scope(home, approval),
                'core activation requires exact approved readers and monitor binding')
        guard.core_config(home, args.state, approval, state)
        guard.core_denied_units(approval, activation_units(approval, readers), inhibited=True)
        for segment in state['segments']:
            guard.point_store_abi(Path('/dev/shm') / segment['target'], 11)
        state['coreReadonlyUnitProof'] = guard.a_acquisition_units(
            home, approval, monitor[0]['target'], inhibited=True, core=True)
        profile, monitor_source = guard.CORE_PROFILE, monitor[0]['source']
    elif 'aReadonlyAcquisition' in approval:
        guard.a_acquisition_scope(home, approval)
        require(joint and readers == guard.A_ACQUISITION_READERS, 'A acquisition ready group changed')
        profile, monitor_source = guard.a_acquisition_profile(approval), MONITOR_IMPLICIT_SHM
        guard.a_acquisition_config(home, args.state, approval, state)
        a_joint_monitor_observe_safe(home, approval, state, readers, joint, documents, True)
        state['aReadonlyUnitProof'] = guard.a_acquisition_units(home, approval, joint[0]['target'], inhibited=True)
        require(systemctl('show', '--property=ActiveState', '--value', 'graphical.target') == 'active',
                'A acquisition requires already-active graphical target')
    elif joint and readers == [MONITOR_UNIT, A_QT_UNIT]:
        profile, monitor_source = 'A_MONITOR_QT', MONITOR_IMPLICIT_SHM
        a_joint_monitor_observe_safe(home, approval, state, readers, joint, documents, True)
        require(systemctl('show', '--property=ActiveState', '--value', 'graphical.target') == 'active',
                'A Qt activation requires already-active graphical target')
    elif monitor and not joint and readers == [MONITOR_UNIT]:
        profile, monitor_source = 'B_MONITOR', monitor[0]['source']
        b_monitor_default_safe(home, approval, manifest, state, readers, monitor, documents)
        monitor_default_unit_binding(approval, True)
        require(monitor_effective_target(monitor[0]['target']), 'B monitor effective target changed')
    else:
        raise ValueError('only pinned B monitor or A joint monitor/Qt activation supported')
    allowed = activation_units(approval, readers)
    activation_capacity_preflight(allowed, 'initial')
    for unit in approval['units']:
        path, body = inhibition_dropin(home, approval['transactionId'], unit)
        require(regular(path).read_bytes() == body, 'activation inhibition changed: ' + unit)
    source_meta = {}
    for segment in state['segments']:
        info = regular(Path('/dev/shm') / segment['source']).stat()
        source_meta[segment['source']] = {'dev': info.st_dev, 'ino': info.st_ino, 'size': info.st_size,
                                          'mtimeNs': info.st_mtime_ns, 'ctimeNs': info.st_ctime_ns}
    # Still under main's flock; refuse drift before ready/state/fences/start.
    activation_capacity_preflight(allowed, 'before-ready')
    write_new(args.state / 'activation-ready.json', regular(args.ready).read_bytes())
    state.update(phase='ACTIVATING', activationProfile=profile, activationMonitorSource=monitor_source,
                 activationReadySha256=args.ready_sha256, activatedUnits=allowed,
                 activationBootId=regular(Path('/proc/sys/kernel/random/boot_id')).read_text().strip(),
                 activationSourceMetadata=source_meta,
                 activationGuardSha256=digest(home / 'bin/runtime-upgrade-guard.py'),
                 activationLauncherSha256=digest(home / 'bin/gateway-services.sh'))
    save(args.state / 'state.json', state)
    try:
        for unit in approval['units']:
            path, _ = inhibition_dropin(home, approval['transactionId'], unit)
            _, body = (guard.active_dropin(home, approval['transactionId'], unit) if unit in allowed else
                       guard.denied_dropin(home, approval['transactionId'], unit))
            replace(path, body, 0o644)
        systemctl('daemon-reload')
        state['phase'] = 'ACTIVATED_CONTROL_DISABLED'
        save(args.state / 'state.json', state)
        active = {'stateDir': str(args.state), 'stateSha256': digest(args.state / 'state.json'),
                  'approvalSha256': args.approval_sha256, 'readySha256': args.ready_sha256,
                  'transactionId': approval['transactionId'], 'allowedUnits': allowed}
        write_new(home / 'data' / guard.ACTIVATION_FILE, (json.dumps(active, sort_keys=True) + '\n').encode())
        for unit in readers:
            guard.activated_unit(home, unit)
            systemctl('start', unit)
            systemctl('is-active', '--quiet', unit)
        guard.activated_unit(home, 'gateway-services.service')
        systemctl('start', 'gateway-services.service')
        if 'gateway-health-watchdog.service' in allowed:
            systemctl('start', 'gateway-health-watchdog.service')
        units_stopped([unit for unit in approval['units'] if unit not in allowed])
    except BaseException:
        try:
            refence_activation(home, approval, args.state, state)
            state['phase'] = 'FAILED_ACTIVATION_STOPPED'
        except BaseException:
            state['phase'] = 'FAILED_STOP_UNCONFIRMED'
        save(args.state / 'state.json', state)
        raise
    print('ACTIVATED_CONTROL_DISABLED: pinned readers only; all other units fenced; no physical control')


@contextlib.contextmanager
def production_lock():
    path = Path('/run/gateway-offline-runtime.lock')
    regular(path)
    fd = os.open(str(path), os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(fd)
        require(before.st_uid == 0 and before.st_size == 0 and stat.S_IMODE(before.st_mode) == 0o600,
                'production requires existing root-owned offline lock')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        named = path.stat()
        require((before.st_dev, before.st_ino) == (named.st_dev, named.st_ino), 'production lock replaced')
        yield
    finally:
        os.close(fd)


def production_gate(home, unit):
    # %n covers a template's concrete instance without making every instance eligible.
    return ('[Unit]\nRequires=gateway-ota-recover.service\nAfter=gateway-ota-recover.service\n'
            '[Service]\nExecStartPre=/usr/bin/python3 ' + str(home / 'bin/runtime-upgrade-guard.py') +
            ' production-unit ' + str(home) + ' %n\n').encode()


def production_inhibit(home):
    return ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n').encode()


def production_copy(source, destination, pin, new=False, temporary=None):
    """Bounded, fsynced paired-file copy; no file-sized recovery heap."""
    source = regular(source)
    require(not any(p.is_symlink() for p in (destination,) + tuple(destination.parents)), 'symlink copy destination')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not new and destination.exists():
        regular(destination)
    temporary = destination if new else temporary
    require(temporary is not None and (new or temporary.parent == destination.parent and
            temporary.name.startswith('.' + destination.name + '.production-')),
            'production copy requires transaction-owned staging name')
    before = source.stat()
    with source.open('rb') as src:
        opened = os.fstat(src.fileno())
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        require(identity(before) == identity(opened), 'production copy source changed before open')
        fd = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, pin['mode'])
        value, count = hashlib.sha256(), 0
        with os.fdopen(fd, 'wb') as out:
            for data in iter(lambda: src.read(65536), b''):
                count += len(data)
                require(count <= pin['bytes'], 'production copy exceeds approved size')
                value.update(data)
                out.write(data)
            require(count == pin['bytes'] and value.hexdigest() == pin['sha256'] and
                    identity(os.fstat(src.fileno())) == identity(opened) and identity(source.stat()) == identity(opened),
                    'production copy source bytes/identity changed')
            os.fchmod(out.fileno(), pin['mode'])
            out.flush()
            os.fsync(out.fileno())
    if not new:
        os.replace(str(temporary), str(destination))
    sync_dir(destination.parent)


def production_owner_proof():
    lock = regular(Path('/run/gateway-offline-runtime.lock')).stat()
    pid = str(os.getpid())
    ticks = regular(Path('/proc') / pid / 'stat').read_text().rsplit(')', 1)[1].split()[19]
    return dict(pid=pid, startTicks=ticks, bootId=regular(Path('/proc/sys/kernel/random/boot_id')).read_text().strip(),
                lockDev=lock.st_dev, lockInode=lock.st_ino)


def production_locked_owner(state):
    proof = state['ownerProof']
    if proof['bootId'] != regular(Path('/proc/sys/kernel/random/boot_id')).read_text().strip():
        return False
    try:
        ticks = regular(Path('/proc') / proof['pid'] / 'stat').read_text().rsplit(')', 1)[1].split()[19]
    except FileNotFoundError:
        return False
    if ticks != proof['startTicks']:
        return False
    lock = regular(Path('/run/gateway-offline-runtime.lock')).stat()
    require((lock.st_dev, lock.st_ino) == (proof['lockDev'], proof['lockInode']), 'production owner lock replaced')
    matches = 0
    for line in regular(Path('/proc/locks')).read_text().splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[1:5] == ['FLOCK', 'ADVISORY', 'WRITE', proof['pid']]:
            major, minor, inode = fields[5].split(':')
            if (int(major, 16), int(minor, 16), int(inode)) == (os.major(lock.st_dev), os.minor(lock.st_dev), lock.st_ino):
                matches += 1
    return matches == 1


class ProductionRuntime:
    """Approved same-ABI file lifecycle; never copy, unlink or rewind live SHM.

    Baseline inventory is independent of CORE node IDs. The initial startup
    policy admits only known non-control readers; broader configured business
    roles require their own reviewed admission/health policies, not core pins.
    """
    def __init__(self, home):
        self.home = home
        self.root = home / 'data' / guard.PRODUCTION_DIR

    def record(self, path, value):
        write_new(path, (json.dumps(value, sort_keys=True) + '\n').encode())

    def state(self, transaction, value):
        save(transaction / 'state.json', value)

    def verify_legacy(self, approval):
        legacy = approval['legacy']
        require(set(legacy) == {'verifierPath', 'verifierSha256', 'activeSha256', 'stateSha256',
                               'approvalSha256', 'readySha256'}, 'invalid historical adoption proof')
        verifier = regular(Path(legacy['verifierPath']))
        require(digest(verifier) == legacy['verifierSha256'] and verifier.stat().st_uid == 0 and
                not verifier.stat().st_mode & 0o022, 'historical verifier changed')
        active_path = self.home / 'data' / guard.ACTIVATION_FILE
        active = checked_json(active_path, legacy['activeSha256'])
        require(active['stateSha256'] == legacy['stateSha256'] and
                active['approvalSha256'] == legacy['approvalSha256'] and
                active['readySha256'] == legacy['readySha256'], 'historical authority mismatch')
        state_dir = Path(active['stateDir'])
        state = checked_json(state_dir / 'state.json', legacy['stateSha256'])
        old_approval = checked_json(state_dir / 'approval.json', legacy['approvalSha256'])
        checked_json(state_dir / 'activation-ready.json', legacy['readySha256'])
        snapshot = approval['previous']
        mandatory = {str(active_path), str(self.home / 'data/runtime-upgrade-stop'), str(verifier)}
        mandatory.update(str(p) for p in state_dir.rglob('*') if p.is_file() or p.is_symlink())
        require(mandatory <= set(snapshot['historical']) and
                all(guard.production_pin(Path(p)) == snapshot['historical'][p] for p in mandatory),
                'adoption must bind complete historical state/backups and sentinel')
        require(set(snapshot['units']) == set(old_approval['units']) and
                {s['name'] for s in snapshot['segments']} == {s['target'] for s in state['segments']} and
                set(snapshot['retiredSources']) == {s['source'] for s in state['segments']},
                'adoption participants/retained v11/source roles differ from legacy proof')
        for unit in active['allowedUnits']:
            result = subprocess.run(['/usr/bin/python3', str(verifier), 'activated-unit', str(self.home), unit],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
            require(result.returncode == 0, 'pinned legacy activation verifier rejected ' + unit)
        return active

    def inputs(self, args, adoption):
        approval = checked_json(args.approval, args.approval_sha256)
        require(guard.production_json(args.approval) == approval, 'ambiguous production approval')
        required = {'format', 'transaction', 'expiresAtUnix', 'gatewayHome', 'machineCode', 'candidate',
                    'parentSha256', 'previous', 'legacy', 'operation'}
        require(set(approval) == required and approval['format'] == 'runtime-provisioning-v1' and
                approval['operation'] == ('adopt' if adoption else 'install') and
                approval['gatewayHome'] == str(self.home) and
                re.fullmatch(r'[A-Za-z0-9_-]{1,63}', approval['transaction']) and
                type(approval['expiresAtUnix']) is int and time.time() < approval['expiresAtUnix'],
                'invalid/expired production approval')
        transaction = self.root / 'transactions' / approval['transaction']
        require(not transaction.exists() and not transaction.is_symlink() and
                not (self.root / 'pending.json').exists() and not (self.root / 'pending.json').is_symlink(),
                'production transaction consumed or pending')
        candidate, previous = approval['candidate'], approval['previous']
        guard.production_schema(candidate, self.home)
        require(candidate['machineCode'] == approval['machineCode'] and
                candidate['parentSha256'] == approval['parentSha256'], 'production candidate parent/identity mismatch')
        require(os.uname().machine == candidate['architecture'], 'production architecture mismatch')
        if adoption:
            require(approval['parentSha256'] is None and candidate['generation'] == 1 and
                    not (self.root / 'current.json').exists() and not (self.root / 'current.json').is_symlink(),
                    'production adoption requires no baseline')
            active = self.verify_legacy(approval)
            require(previous['allowedUnits'] == active['allowedUnits'], 'adoption prior service scope mismatch')
        else:
            require(approval['legacy'] is None, 'installation cannot re-adopt historical authority')
            current = guard.production_context(self.home)
            pointer = guard.production_json(self.root / 'current.json')
            require(current == previous and pointer['sha256'] == approval['parentSha256'] and
                    candidate['generation'] == current['generation'] + 1, 'production baseline parent mismatch')
        # Provisioning config writes and authority/participant expansion are deliberately
        # outside this first same-ABI slice. Inventory itself includes ALL participants.
        for key in ('gatewayHome', 'machineCode', 'architecture', 'pointStoreAbi', 'pointStoreSize',
                    'clusterProtocol', 'controlEnabled', 'configSha256', 'units', 'allowedUnits',
                    'gates', 'segments', 'retiredSources', 'historical', 'bootId', 'trust', 'healthPolicy'):
            require(candidate[key] == previous[key], 'same-ABI preservation mismatch: ' + key)
        introduced = set(candidate['files']) - set(previous['files'])
        require((not introduced or adoption and introduced <= {'bin/' + n for n in guard.PRODUCTION_HELPERS}) and
                set(previous['files']) <= set(candidate['files']) and
                set(candidate['unitFiles']) == set(previous['unitFiles']), 'paired inventory expansion is not approved')
        for path in introduced:
            require(not (self.home / path).exists() and not (self.home / path).is_symlink(),
                    'unexpected introduced production helper')
        for unit, path in candidate['gates'].items():
            body = production_gate(self.home, unit)
            require(candidate['unitFiles'][path] == dict(sha256=hashlib.sha256(body).hexdigest(), mode=0o644, bytes=len(body)),
                    'production gate must be exact owned guard action')
        for path in set(previous['unitFiles']) - set(previous['gates'].values()):
            if path == str(Path('/etc/systemd/system') / guard.PRODUCTION_RECOVERY_UNIT):
                require(candidate['unitFiles'][path] == (guard.production_pin(Path(__file__).with_name(guard.PRODUCTION_RECOVERY_UNIT))
                        if adoption else previous['unitFiles'][path]),
                        'production boot recovery unit is not the reviewed paired entry')
            else:
                require(candidate['unitFiles'][path] == previous['unitFiles'][path], 'non-gate unit write refused')
        boot_unit = str(Path('/etc/systemd/system') / guard.PRODUCTION_RECOVERY_UNIT)
        require(boot_unit in candidate['unitFiles'] and candidate['unitFiles'][boot_unit] ==
                (guard.production_pin(Path(__file__).with_name(guard.PRODUCTION_RECOVERY_UNIT)) if adoption else previous['unitFiles'][boot_unit]),
                'production baseline requires installed paired boot recovery')
        guard.production_verify(self.home, previous)
        for name in ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py'):
            pin = candidate['files']['bin/' + name]
            require(digest(Path(__file__).with_name(name)) == pin['sha256'], 'production paired helper not staged: ' + name)
        payload = args.payload.absolute()
        require(not payload.is_symlink() and payload.is_dir(), 'production payload directory required')
        require(not any(p.is_symlink() for p in payload.rglob('*')), 'symlink production payload')
        actual = {str(p.relative_to(payload)) for p in payload.rglob('*') if not p.is_dir()}
        require(actual == set(candidate['files']), 'production payload inventory mismatch')
        for path, pin in candidate['files'].items():
            require(guard.production_pin(payload / path) == pin, 'production payload pin mismatch: ' + path)
            guard.production_architecture(payload / path)
        total = sum(pin['bytes'] for pin in previous['files'].values()) + sum(pin['bytes'] for pin in candidate['files'].values())
        require(total <= 512 * 1024 * 1024 and os.statvfs(self.home).f_bavail * os.statvfs(self.home).f_frsize >= total + 64 * 1024 * 1024,
                'insufficient production backup/install disk')
        activation_capacity_preflight(candidate['allowedUnits'], 'production-before-intent')
        require(time.time() < approval['expiresAtUnix'], 'production install window expired')
        return approval, transaction

    def stop(self, baseline):
        concrete = [u for u in baseline['units'] if not u.endswith('@.service')]
        rows = batch_stop_results(concrete)
        require(all(r['stopOk'] and r['activeState'] in ('inactive', 'failed') and r['mainPid'] == '0' for r in rows),
                'production participant stop unconfirmed')
        no_processes(self.home, [])
        no_mappings([s['name'] for s in baseline['segments']] + list(baseline['retiredSources']), include_fds=True)

    def backup(self, transaction, approval):
        previous = approval['previous']
        for index, (path, pin) in enumerate(sorted(previous['files'].items())):
            require(guard.production_pin(self.home / path) == pin, 'stopped production file drift')
            production_copy(self.home / path, transaction / 'files' / str(index), pin, new=True)
            require(digest(transaction / 'files' / str(index)) == pin['sha256'], 'production backup verification failed')
        path = str(Path('/etc/systemd/system') / guard.PRODUCTION_RECOVERY_UNIT)
        production_copy(Path(path), transaction / 'boot-unit', previous['unitFiles'][path], new=True)
        self.record(transaction / 'backups-complete.json', {'approvalSha256': digest(transaction / 'approval.json')})

    def fence(self, transaction, state, previous):
        # Archive ALL owned gates before the first live gate write. The stop sentinel
        # is retained; its exact old bytes are part of the immutable lineage proof.
        require(regular(self.home / 'data/runtime-upgrade-stop').stat().st_size > 0,
                'production adoption requires the preserved stop sentinel')
        for index, (unit, path) in enumerate(sorted(previous['gates'].items())):
            require(guard.production_pin(Path(path)) == previous['unitFiles'][path], 'production gate drift before fence')
            write_new(transaction / 'gates' / str(index), regular(Path(path)).read_bytes(), 0o644)
        self.record(transaction / 'gates-complete.json', {'approvalSha256': state['approvalSha256']})
        state['phase'] = 'FENCING'
        self.state(transaction, state)
        for unit, path in previous['gates'].items():
            replace(Path(path), production_inhibit(self.home), 0o644)
        systemctl('daemon-reload')

    def convert_gates(self, transaction, baseline):
        for unit, path in baseline['gates'].items():
            replace(Path(path), production_gate(self.home, unit), 0o644)
        systemctl('daemon-reload')

    def copy_name(self, transaction, path, phase):
        destination = self.home / path
        return destination.with_name('.' + destination.name + '.production-' + transaction.name + '-' + phase)

    def copy_ownership(self, transaction, approval):
        owners = {}
        for phase, baseline in (('install', approval['candidate']), ('recover', approval['previous'])):
            for path, pin in baseline['files'].items():
                temp = self.copy_name(transaction, path, phase)
                require(not temp.exists() and not temp.is_symlink(), 'production staging namespace occupied')
                owners[str(temp)] = {'path': path, 'phase': phase, 'pin': pin}
        self.record(transaction / 'copy-owners.json', owners)

    def archive_copies(self, transaction, approval):
        owners = guard.production_json(transaction / 'copy-owners.json')
        expected = {str(self.copy_name(transaction, path, phase)): {'path': path, 'phase': phase, 'pin': pin}
                    for phase, baseline in (('install', approval['candidate']), ('recover', approval['previous']))
                    for path, pin in baseline['files'].items()}
        require(owners == expected, 'production staging ownership journal drift')
        for index, (name, owner) in enumerate(sorted(owners.items())):
            temp = Path(name)
            if not temp.exists() and not temp.is_symlink():
                continue
            pin = guard.production_pin(temp)
            require(pin['bytes'] <= owner['pin']['bytes'] and pin['mode'] == owner['pin']['mode'],
                    'interrupted copy type/size/mode differs from owned intent')
            retained = transaction / 'interrupted-copies' / (str(index) + '-' + uuid.uuid4().hex)
            retained.parent.mkdir(mode=0o700, exist_ok=True)
            os.rename(str(temp), str(retained))
            sync_dir(temp.parent)
            sync_dir(retained.parent)

    def install(self, args, adoption=False):
        approval, transaction = self.inputs(args, adoption)
        previous, candidate = approval['previous'], approval['candidate']
        # Revalidate while the offline lock is held, before publishing any intent.
        guard.production_verify(self.home, previous)
        # Approval identity is the externally pinned RAW bytes, not our JSON formatting.
        write_new(transaction / 'approval.json', regular(args.approval).read_bytes())
        require(digest(transaction / 'approval.json') == args.approval_sha256, 'production approval changed during staging')
        self.record(transaction / 'previous.json', previous)
        self.record(transaction / 'candidate.json', candidate)
        self.copy_ownership(transaction, approval)
        if not adoption:
            write_new(transaction / 'previous-pointer.json', regular(self.root / 'current.json').read_bytes())
        state = {'format': 'runtime-install-v1', 'transaction': approval['transaction'],
                 'approvalSha256': args.approval_sha256, 'adoption': adoption, 'phase': 'INTENT',
                 'candidateSha256': digest(transaction / 'candidate.json'), 'previousSha256': digest(transaction / 'previous.json'),
                 'ownerProof': production_owner_proof()}
        self.state(transaction, state)
        self.record(self.root / 'pending.json', {'transaction': approval['transaction'], 'approvalSha256': args.approval_sha256})
        try:
            self.fence(transaction, state, previous)
            self.stop(previous)
            guard.production_verify(self.home, previous, units=False)
            self.backup(transaction, approval)
            state['phase'] = 'INSTALLING'
            self.state(transaction, state)
            self.convert_gates(transaction, candidate)
            require(time.time() < approval['expiresAtUnix'], 'production install window expired after stop')
            for path, pin in candidate['files'].items():
                require(guard.production_pin(args.payload / path) == pin, 'staged production file drift')
                production_copy(args.payload / path, self.home / path, pin,
                                temporary=self.copy_name(transaction, path, 'install'))
            boot_unit = Path('/etc/systemd/system') / guard.PRODUCTION_RECOVERY_UNIT
            if adoption:
                replace(boot_unit, regular(Path(__file__).with_name(guard.PRODUCTION_RECOVERY_UNIT)).read_bytes(), 0o644)
            systemctl('daemon-reload')
            guard.production_verify(self.home, candidate)
            self.start(transaction, state, candidate, 'STARTING')
            health = self.health(candidate)
            health.update(baselineSha256=state['candidateSha256'], transaction=approval['transaction'], status='healthy')
            self.record(transaction / 'health.json', health)
            self.promote(transaction, state, candidate)
        except BaseException:
            # Promotion is the commit point. A terminal-journal failure must never
            # roll back a durably proved healthy pointer, even if its fsync returned uncertain.
            if self.promoted(transaction, state):
                raise
            state['phase'] = 'FAILED_PENDING_RECOVERY'
            self.state(transaction, state)
            raise
        return transaction

    def start(self, transaction, state, baseline, phase):
        guard.production_verify(self.home, baseline)
        activation_capacity_preflight(baseline['allowedUnits'], 'production-before-start')
        state.update(phase=phase, startingBaselineSha256=(state['candidateSha256'] if phase == 'STARTING' else state['previousSha256']),
                     ownerProof=production_owner_proof())
        self.state(transaction, state)
        # Readers first; launcher and watchdog may only see a complete provisional baseline.
        ordered = [u for u in baseline['allowedUnits'] if u in guard.PRODUCTION_READERS]
        ordered += [u for u in ('gateway-services.service', 'gateway-health-watchdog.service') if u in baseline['allowedUnits']]
        for unit in ordered:
            guard.production_unit(self.home, unit)
            systemctl('start', unit)
            systemctl('is-active', '--quiet', unit)

    def health(self, baseline):
        def sample():
            guard.production_verify(self.home, baseline)
            readers = {}
            expected = {}
            for unit in baseline['allowedUnits']:
                if unit not in guard.PRODUCTION_READERS:
                    continue
                require(guard.systemctl_value(unit, 'ActiveState') == 'active' and
                        guard.systemctl_value(unit, 'SubState') == 'running', 'production reader unhealthy')
                pid = guard.systemctl_value(unit, 'MainPID')
                require(pid.isdigit() and int(pid) > 1 and pid not in expected, 'production reader PID invalid/duplicate')
                executable = self.home / guard.PRODUCTION_READERS[unit]
                proc = Path('/proc') / pid
                require(os.readlink(str(proc / 'exe')) == str(executable) and
                        guard.sha(proc / 'exe') == baseline['files'][guard.PRODUCTION_READERS[unit]]['sha256'],
                        'production actual executable mismatch')
                text = regular(proc / 'maps').read_text()
                mapped = set()
                for line in text.splitlines():
                    fields = line.split()
                    if len(fields) >= 6 and fields[5].startswith('/dev/shm/'):
                        require(not line.rstrip().endswith(' (deleted)'), 'deleted production mapping')
                        name = fields[5].split('/')[-1]
                        segment = next((s for s in baseline['segments'] if s['name'] == name), None)
                        require(segment is not None and int(fields[4]) == segment['ino'] and
                                fields[3] == '%02x:%02x' % (os.major(segment['dev']), os.minor(segment['dev'])),
                                'production actual SHM mapping mismatch')
                        mapped.add(name)
                require(mapped, 'production reader has no approved v11 mapping')
                readers[unit] = {'pid': pid, 'restarts': guard.systemctl_value(unit, 'NRestarts'),
                                 'startTicks': regular(proc / 'stat').read_text().rsplit(')', 1)[1].split()[19],
                                 'mappedSegments': sorted(mapped)}
                expected[pid] = str(executable)
            for entry in Path('/proc').iterdir():
                if not entry.name.isdigit() or int(entry.name) == os.getpid():
                    continue
                try:
                    exe = os.readlink(str(entry / 'exe'))
                except FileNotFoundError:
                    continue
                if Path(exe[:-10] if exe.endswith(' (deleted)') else exe).name in guard.RUNTIME_BINARIES:
                    require(expected.get(entry.name) == exe, 'production unapproved/duplicate runtime process')
            for unit in set(baseline['units']) - set(baseline['allowedUnits']):
                if not unit.endswith('@.service'):
                    require(guard.systemctl_value(unit, 'ActiveState') in ('inactive', 'failed') and
                            guard.systemctl_value(unit, 'MainPID') == '0', 'production excluded participant running')
            return readers
        before = sample()
        policy = baseline['healthPolicy']
        # --disable must be first: no ~/.curlrc. One fixed GET, no redirects,
        # proxy/environment/config/client-file options or caller-supplied argv.
        command = ['/usr/bin/curl', '--disable', '--noproxy', '*', '--proto', '=http',
                   '--max-time', str(policy['timeoutSeconds']), '--fail', '--silent', '--show-error',
                   '--', policy['url']]
        response = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=15,
                                  env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
        require(response.returncode == 0, 'production endpoint health failed')
        time.sleep(baseline['healthPolicy']['stableSeconds'])
        after = sample()
        require(before == after, 'production PID/restarts/mapping stability failed')
        return {'format': 'runtime-health-v1', 'readers': after, 'bootId': baseline['bootId'],
                'configSha256': baseline['configSha256']}

    def promoted(self, transaction, state):
        path = self.root / 'current.json'
        if not path.exists() and not path.is_symlink():
            return False
        pointer = guard.production_json(path)
        return pointer.get('sha256') == state['candidateSha256'] and pointer.get('transaction') == state['transaction']

    def promote(self, transaction, state, candidate):
        health = guard.production_json(transaction / 'health.json')
        require(health['status'] == 'healthy' and health['baselineSha256'] == state['candidateSha256'],
                'production promotion missing bound health')
        record = self.root / 'baselines' / (candidate['baselineId'] + '.json')
        if not record.exists():
            write_new(record, regular(transaction / 'candidate.json').read_bytes(), 0o444)
        require(digest(record) == state['candidateSha256'], 'immutable production baseline conflicts')
        state['phase'] = 'PROMOTING'
        self.state(transaction, state)
        replace(self.root / 'current.json', (json.dumps({'baselineId': candidate['baselineId'],
                'sha256': state['candidateSha256'], 'healthSha256': digest(transaction / 'health.json'),
                'transaction': state['transaction']}, sort_keys=True) + '\n').encode())
        sync_dir(self.root)
        state['phase'] = 'COMPLETED'
        self.state(transaction, state)
        self.clear_pending(state)

    def clear_pending(self, state):
        path = self.root / 'pending.json'
        if path.exists() or path.is_symlink():
            require(guard.production_json(path) == {'transaction': state['transaction'], 'approvalSha256': state['approvalSha256']},
                    'production pending pointer changed')
            path.unlink()
            sync_dir(path.parent)

    def recover(self, transaction, approval_sha, start=True):
        try:
            return self.restore(transaction, approval_sha, start)
        except BaseException:
            # Best-effort exact refence, never overwrite an unrecognized gate.
            approval = checked_json(transaction / 'approval.json', approval_sha)
            state = guard.production_json(transaction / 'state.json')
            if not self.promoted(transaction, state):
                for unit, path in approval['previous']['gates'].items():
                    body = production_inhibit(self.home)
                    inhibited = dict(sha256=hashlib.sha256(body).hexdigest(), mode=0o644, bytes=len(body))
                    require(guard.production_pin(Path(path)) in (approval['previous']['unitFiles'][path],
                            approval['candidate']['unitFiles'][path], inhibited), 'foreign recovery gate; refuse overwrite')
                for path in approval['previous']['gates'].values():
                    replace(Path(path), production_inhibit(self.home), 0o644)
                systemctl('daemon-reload')
                self.stop(approval['previous'])
                state['phase'] = 'RECOVERY_FAILED_STOPPED'
                self.state(transaction, state)
            raise

    def restore(self, transaction, approval_sha, start=True):
        approval = checked_json(transaction / 'approval.json', approval_sha)
        state = guard.production_json(transaction / 'state.json')
        require(state['approvalSha256'] == approval_sha and
                transaction == self.root / 'transactions' / state['transaction'] and
                digest(transaction / 'previous.json') == state['previousSha256'] and
                digest(transaction / 'candidate.json') == state['candidateSha256'], 'production recovery lineage changed')
        previous, candidate = approval['previous'], approval['candidate']
        require(guard.production_json(transaction / 'previous.json') == previous and
                guard.production_json(transaction / 'candidate.json') == candidate, 'production recovery baseline mismatch')
        if self.promoted(transaction, state):
            guard.production_context(self.home)
            self.promote(transaction, state, candidate)
            return 'COMPLETED'
        require(state['phase'] not in ('COMPLETED', 'RECOVERED'), 'production terminal transaction is consumed')
        # Unresolved pending authority blocks every provisional gate during recovery.
        state['phase'] = 'RECOVERING_STOP'
        self.state(transaction, state)
        self.stop(previous)
        guard.production_verify(self.home, previous, files=False, units=False)
        self.archive_copies(transaction, approval)
        actual = guard.production_files(self.home)
        require(set(approval['previous']['files']) <= set(actual) <= set(approval['candidate']['files']) and
                all(pin in (previous['files'].get(path), candidate['files'].get(path)) for path, pin in actual.items()),
                'unowned production runtime file during recovery')
        gates_complete = transaction / 'gates-complete.json'
        if gates_complete.exists():
            require(guard.production_json(gates_complete) == {'approvalSha256': approval_sha}, 'production gate receipt drift')
            body = production_inhibit(self.home)
            inhibited = dict(sha256=hashlib.sha256(body).hexdigest(), mode=0o644, bytes=len(body))
            for index, (unit, path) in enumerate(sorted(previous['gates'].items())):
                require(guard.production_pin(transaction / 'gates' / str(index)) == previous['unitFiles'][path] and
                        guard.production_pin(Path(path)) in (previous['unitFiles'][path], candidate['unitFiles'][path], inhibited),
                        'production restore gate/backup drift')
        complete = transaction / 'backups-complete.json'
        if not complete.exists():
            # Before verified backups no live file/gate writes were authorized.
            guard.production_verify(self.home, previous, units=False)
        else:
            require(guard.production_json(complete) == {'approvalSha256': approval_sha}, 'production backup receipt changed')
            for index, (path, old) in enumerate(sorted(previous['files'].items())):
                backup = transaction / 'files' / str(index)
                require(guard.production_pin(backup) == old and
                        guard.production_pin(self.home / path) in (old, candidate['files'][path]),
                        'production restore file/backup drift')
            for path in set(candidate['files']) - set(previous['files']):
                live = self.home / path
                require(not live.is_symlink() and (not live.exists() or guard.production_pin(live) == candidate['files'][path]),
                        'introduced production helper drift')
            boot_unit = Path('/etc/systemd/system') / guard.PRODUCTION_RECOVERY_UNIT
            require(guard.production_pin(transaction / 'boot-unit') == previous['unitFiles'][str(boot_unit)] and
                    guard.production_pin(boot_unit) in (previous['unitFiles'][str(boot_unit)], candidate['unitFiles'][str(boot_unit)]),
                    'production boot recovery backup/file drift')
            for index, (path, pin) in enumerate(sorted(previous['files'].items())):
                production_copy(transaction / 'files' / str(index), self.home / path, pin,
                                temporary=self.copy_name(transaction, path, 'recover'))
            for path in set(candidate['files']) - set(previous['files']):
                live = self.home / path
                if live.exists():
                    live.unlink()
                    sync_dir(live.parent)
            replace(boot_unit, regular(transaction / 'boot-unit').read_bytes(), previous['unitFiles'][str(boot_unit)]['mode'])
        if gates_complete.exists():
            for index, (unit, path) in enumerate(sorted(previous['gates'].items())):
                replace(Path(path), regular(transaction / 'gates' / str(index)).read_bytes(), 0o644)
            systemctl('daemon-reload')
        guard.production_verify(self.home, previous)
        if state['adoption']:
            self.verify_legacy(approval)
            # Preserve legacy authority, records and owned gates byte-for-byte.
            if start:
                activation_capacity_preflight(previous['allowedUnits'], 'production-legacy-recovery-start')
                for unit in previous['allowedUnits']:
                    systemctl('start', unit)
                self.health(previous)
        else:
            pointer = guard.production_json(transaction / 'previous-pointer.json')
            require(pointer['sha256'] == approval['parentSha256'], 'production prior pointer changed')
            replace(self.root / 'current.json', regular(transaction / 'previous-pointer.json').read_bytes())
            if start:
                self.start(transaction, state, previous, 'RECOVERING_START')
                self.health(previous)
        state['phase'] = 'RECOVERED' if start else 'RECOVERED_STOPPED'
        if not start:
            for path in previous['gates'].values():
                replace(Path(path), production_inhibit(self.home), 0o644)
            systemctl('daemon-reload')
        self.state(transaction, state)
        if start:
            self.clear_pending(state)
        return state['phase']


def production_adopt(args):
    approval = checked_json(args.approval, args.approval_sha256)
    return ProductionRuntime(Path(approval['gatewayHome'])).install(args, adoption=True)


def production_install(args):
    approval = checked_json(args.approval, args.approval_sha256)
    return ProductionRuntime(Path(approval['gatewayHome'])).install(args)


def production_recover(args):
    approval = checked_json(args.state / 'approval.json', args.approval_sha256)
    return ProductionRuntime(Path(approval['gatewayHome'])).recover(args.state, args.approval_sha256,
                                                                   start=not args.no_restart)


def production_boot(args):
    home = args.home
    runtime = ProductionRuntime(home)
    pending = runtime.root / 'pending.json'
    if pending.exists() or pending.is_symlink():
        ref = guard.production_json(pending)
        require(isinstance(ref, dict) and set(ref) == {'transaction', 'approvalSha256'} and
                isinstance(ref['transaction'], str) and re.fullmatch(r'[A-Za-z0-9_-]{1,63}', ref['transaction']) and
                isinstance(ref['approvalSha256'], str) and re.fullmatch('[0-9a-f]{64}', ref['approvalSha256']),
                'production boot pending pointer invalid')
        state = guard.production_json(runtime.root / 'transactions' / ref['transaction'] / 'state.json')
        require(state.get('approvalSha256') == ref['approvalSha256'], 'production boot approval mismatch')
        if state['phase'] in ('STARTING', 'RECOVERING_START') and production_locked_owner(state):
            # Starting a reader may start its Requires= recovery oneshot. A
            # proved live same-boot lock owner is NOT an interrupted transaction.
            guard.production_unit(home, 'gateway-services.service')
            return
    staging = home / 'ota/staging'
    # The job journal may still be unresolved AFTER pending was cleared by
    # promotion/recovery. Resolve it before consulting the baseline pointer.
    if staging.exists():
        spec = importlib.util.spec_from_file_location('production_ota_health', str(Path(__file__).with_name('ota-health-result.py')))
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        records = list(staging.glob('ota_handoff_*.json'))
        require(len(records) <= 128, 'production boot handoff inventory exceeds bound')
        for path in records:
            record = guard.production_json(path)
            if record.get('runtimeMayChange') is True and record.get('phase') != 'OWNER_FINISHED':
                helper.RuntimeHandoff(staging, record['jobId'], record['token']).recover()
    pending = runtime.root / 'pending.json'
    if pending.exists() or pending.is_symlink():
        ref = guard.production_json(pending)
        require(isinstance(ref, dict) and set(ref) == {'transaction', 'approvalSha256'} and
                isinstance(ref['transaction'], str) and re.fullmatch(r'[A-Za-z0-9_-]{1,63}', ref['transaction']) and
                isinstance(ref['approvalSha256'], str) and re.fullmatch('[0-9a-f]{64}', ref['approvalSha256']),
                'production boot pending pointer invalid')
        with production_lock():
            runtime.recover(runtime.root / 'transactions' / ref['transaction'], ref['approvalSha256'], start=False)
    current = runtime.root / 'current.json'
    if current.exists() or current.is_symlink():
        # Missing/new-boot SHM deliberately blocks BEFORE launcher until a native
        # creator ordering is provisioned; no Python files or raw queue restoration.
        with production_lock():
            guard.production_context(home)


def production_ota_owner(protocol, record):
    """Consume a signed/checksummed artifact only after independent lease handoff.

    The OtaService intent is written only after outer RSA/SHA256/size validation.
    No ordinary-manifest guard bypass: this is the distinct proved baseline path.
    """
    intent = protocol.intent()
    home = protocol.root.parent.parent
    runtime = ProductionRuntime(home)
    with production_lock():
        baseline = guard.production_context(home)
        require(baseline['machineCode'] == intent['machineCode'], 'runtime OTA device/baseline mismatch')
        pointer = guard.production_json(runtime.root / 'current.json')
        artifact = regular(Path(intent['artifactPath']))
        require(protocol.artifact_hash(artifact, int(intent['artifactSize'])) == intent['artifactSha256'],
                'runtime OTA artifact changed after owner acquisition')
        work = protocol.root / ('runtime-payload-' + protocol.job)
        require(not work.exists() and not work.is_symlink(), 'runtime OTA work path consumed')
        with tarfile.open(str(artifact), 'r:gz') as archive:
            members = archive.getmembers()
            require(len(members) <= 512 and all(m.isfile() and not m.issym() and not m.islnk() and
                    m.size <= 128 * 1024 * 1024 for m in members) and
                    len({m.name for m in members}) == len(members), 'runtime release archive type/inventory invalid')
            metadata = [m for m in members if m.name == 'runtime-manifest.json']
            require(len(metadata) == 1 and metadata[0].size <= 1024 * 1024, 'runtime manifest missing/oversized')
            # Use the same duplicate-field/ownership parser as persistent records.
            raw = archive.extractfile(metadata[0]).read()
            work.mkdir(mode=0o700)
            write_new(work / 'runtime-manifest.json', raw)
            manifest = guard.production_json(work / 'runtime-manifest.json')
            require(set(manifest) == {'format', 'version', 'packageType', 'fromAbi', 'toAbi', 'clusterProtocol',
                                     'architecture', 'parents', 'expiresAtUnix', 'files'} and
                    manifest['format'] == 'runtime-release-v1' and manifest['version'] == intent['version'] and
                    manifest['packageType'] == 'full' and manifest['fromAbi'] == manifest['toAbi'] == 11 and
                    manifest['clusterProtocol'] == 2 and manifest['architecture'] == 'aarch64' and
                    isinstance(manifest['parents'], list) and pointer['sha256'] in manifest['parents'] and
                    type(manifest['expiresAtUnix']) is int and time.time() < manifest['expiresAtUnix'],
                    'runtime release incompatible/expired')
            candidate = copy.deepcopy(baseline)
            transaction_id = 'OTA_' + hashlib.sha256((protocol.job + protocol.token).encode()).hexdigest()[:32]
            candidate.update(baselineId=transaction_id, generation=baseline['generation'] + 1,
                             parentSha256=pointer['sha256'], release=manifest['version'], files=manifest['files'])
            guard.production_schema(candidate, home)
            require(set(candidate['files']) == set(baseline['files']) and
                    {m.name for m in members} == {'runtime-manifest.json'} | set(candidate['files']) and
                    sum(m.size for m in members) <= 512 * 1024 * 1024, 'runtime package write set differs from baseline')
            payload = work / 'payload'
            for member in members:
                if member.name == 'runtime-manifest.json':
                    continue
                pin = candidate['files'][member.name]
                require(member.size == pin['bytes'], 'runtime member size mismatch')
                dest = payload / member.name
                dest.parent.mkdir(parents=True, exist_ok=True)
                fd = os.open(str(dest), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, pin['mode'])
                with archive.extractfile(member) as source, os.fdopen(fd, 'wb') as output:
                    value, count = hashlib.sha256(), 0
                    for chunk in iter(lambda: source.read(65536), b''):
                        value.update(chunk); count += len(chunk); output.write(chunk)
                    require(count == pin['bytes'] and value.hexdigest() == pin['sha256'], 'runtime member hash mismatch')
                    output.flush(); os.fsync(output.fileno())
                    os.fchmod(output.fileno(), pin['mode'])
            sync_dir(work)
        approval = dict(format='runtime-provisioning-v1', transaction=transaction_id, operation='install',
                        expiresAtUnix=manifest['expiresAtUnix'], gatewayHome=str(home), machineCode=intent['machineCode'],
                        parentSha256=pointer['sha256'], previous=baseline, candidate=candidate, legacy=None)
        path = work / 'approval.json'
        runtime.record(path, approval)
        args = argparse.Namespace(approval=path, approval_sha256=digest(path), payload=payload)
        # Admission is rerun inside install. Persist the mutation barrier before
        # the first intent/fence so donor cancellation can never claim unchanged.
        runtime.inputs(args, adoption=False)
        with protocol.mutex():
            fresh = protocol.read()
            require(fresh['phase'] == 'OWNER_OWNED', 'runtime ownership changed before install')
            fresh.update(phase='OWNER_INSTALLING', runtimeMayChange=True, productionTransaction=transaction_id,
                         productionApprovalSha256=args.approval_sha256, gatewayHome=str(home))
            protocol.save(fresh)
        transaction = runtime.root / 'transactions' / transaction_id
        try:
            runtime.install(args)
            status, rollback = 'healthy', 'none'
        except BaseException:
            if not (transaction / 'state.json').exists():
                raise  # barrier is conservative: unresolved, never an unchanged shortcut
            result = runtime.recover(transaction, args.approval_sha256)
            status, rollback = ('healthy', 'none') if result == 'COMPLETED' else ('failed', 'succeeded')
        production_ota_finish(protocol, transaction_id, status, rollback)


def production_ota_finish(protocol, transaction_id, status, rollback):
        intent = protocol.intent()
        runtime = ProductionRuntime(protocol.root.parent.parent)
        decision = dict(format='ota-health-v1', jobId=protocol.job, token=protocol.token, version=intent['version'],
                        status=status, rollback=rollback, ts=str(int(time.time() * 1000)))
        health_path = protocol.root / ('health_' + protocol.job + '.txt')
        with protocol.lock('ota_state.lock', wait=2):
            if health_path.exists() or health_path.is_symlink():
                old = guard.production_json(health_path)
                require(all(old.get(k) == v for k, v in decision.items() if k != 'ts'), 'runtime final health conflict')
            else:
                runtime.record(health_path, decision)
            if status == 'healthy':
                replace(protocol.root / 'current_version.txt', ('jobId=' + protocol.job + '\nversion=' + intent['version'] + '\n').encode())
                replace(protocol.root / 'applied_version.txt', (intent['version'] + '\n').encode())
        with protocol.mutex():
            fresh = protocol.read()
            fresh.update(phase='OWNER_FINISHED', completionProof=dict(status=status, rollback=rollback,
                         healthSha256=digest(health_path), transaction=transaction_id,
                         healthRecord=regular(health_path).read_text()))
            protocol.save(fresh)


def production_ota_resume(protocol, record):
    # Called with the ORIGINAL job lease, positive dead-owner/no-cgroup-task
    # proof, and pinned staged helper bytes. Never extract, download or install.
    home = protocol.root.parent.parent
    require(record.get('gatewayHome') == str(home) and
            isinstance(record.get('productionTransaction'), str) and
            re.fullmatch(r'OTA_[0-9a-f]{32}', record['productionTransaction']) and
            isinstance(record.get('productionApprovalSha256'), str) and
            re.fullmatch('[0-9a-f]{64}', record['productionApprovalSha256']), 'runtime recovery job lineage invalid')
    runtime = ProductionRuntime(home)
    transaction_id = record['productionTransaction']
    transaction = runtime.root / 'transactions' / transaction_id
    with production_lock():
        state_path = transaction / 'state.json'
        if not state_path.exists() and not state_path.is_symlink():
            approval_path = protocol.root / ('runtime-payload-' + protocol.job) / 'approval.json'
            approval = checked_json(approval_path, record['productionApprovalSha256'])
            require(approval['transaction'] == transaction_id and approval['machineCode'] == protocol.intent()['machineCode'] and
                    not (runtime.root / 'pending.json').exists() and not (runtime.root / 'pending.json').is_symlink() and
                    guard.production_context(home) == approval['previous'], 'pre-intent runtime unchanged proof failed')
            runtime.health(approval['previous'])
            result = 'RECOVERED'
        else:
            state = guard.production_json(state_path)
            approval = checked_json(transaction / 'approval.json', record['productionApprovalSha256'])
            if state['phase'] == 'RECOVERED':
                require(guard.production_context(home) == approval['previous'], 'runtime recovered baseline drift')
                runtime.health(approval['previous'])
                result = 'RECOVERED'
            else:
                result = runtime.recover(transaction, record['productionApprovalSha256'])
        status, rollback = ('healthy', 'none') if result == 'COMPLETED' else ('failed', 'succeeded')
        production_ota_finish(protocol, transaction_id, status, rollback)


def main():
    if len(sys.argv) == 5 and sys.argv[1] == 'ota-owner':
        spec = importlib.util.spec_from_file_location('ota_health', str(Path(__file__).with_name('ota-health-result.py')))
        health = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(health)
        health.RuntimeHandoff(Path(sys.argv[2]), sys.argv[3], sys.argv[4]).own(production_ota_owner)
        return
    if len(sys.argv) == 3 and sys.argv[1] == 'production-boot':
        require(os.geteuid() == 0, 'production boot recovery requires root')
        # Each job acquires lease BEFORE offline lock; baseline-only work follows.
        production_boot(argparse.Namespace(home=Path(sys.argv[2])))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'recover', 'observe', 'activate',
                                         'production-adopt', 'production-install', 'production-recover'))
    parser.add_argument('--approval', type=Path)
    parser.add_argument('--approval-sha256', required=True)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--ready', type=Path)
    parser.add_argument('--ready-sha256')
    parser.add_argument('--no-restart', action='store_true')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'root in host PID/mount namespace required')
    if args.action.startswith('production-'):
        with production_lock():
            globals()[args.action.replace('-', '_')](args)
        return
    if args.action == 'apply':
        require(args.approval and args.manifest and args.payload, 'apply requires approval, manifest and payload')
        args.payload = args.payload.absolute()
        if CORE_TEST_APPLY_ALLOWED is not True:
            # Refuse production core requests before even creating the main lock.
            core_apply_policy(checked_json(args.approval, args.approval_sha256))
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
