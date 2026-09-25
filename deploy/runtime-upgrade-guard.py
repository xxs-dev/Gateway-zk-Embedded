#!/usr/bin/env python3
"""Read-only deployment gates. No ABI migration or control approval is implicit."""
import json
import hashlib
import os
import re
from pathlib import Path
import struct
import stat
import subprocess
import sys

COMPATIBILITY = {"pointStoreAbi": 11, "clusterProtocol": 2,
                 "upgradeMode": "offline-all-participants"}
SINGLE_SHM_KEYS = frozenset(('sharedMemoryName', 'virtualSharedMemoryName',
                           'outputSharedMemoryName', 'outputDefaultSharedMemoryName'))
MULTI_SHM_KEYS = frozenset(('sharedMemoryNames',))
RUNTIME_BINARIES = frozenset((
    "ModbusRtu", "Dlt645Driver", "DioDriver", "CanDriver", "IecDriver", "MqttDriver",
    "MqttForwarder", "EventEngine", "EventStore", "ComputeEngine", "AgcAvcController",
    "EmsParityCheck", "EmsClusterCoordinator", "SystemMonitor", "LocalDisplay", "LocalDisplayQtEms",
    "QtDisplayBridge", "CameraService", "pointctl", "KY-EMS", "memory_point_store_migrate"))
ACTIVATION_FILE = 'runtime-upgrade-active.json'
MONITOR_UNIT = 'system-monitor@monitor-service.service'
QT_UNIT = 'ky-ems.service'
MONITOR_BINARY = '91f5ab7fd6a1221931defa8f4531b1734f0d2c9b17da9bc0ae6b677d121f0928'
QT_BINARY = '27dcffc5dfa538640ac4d249f2fea934495fe49a87910d0693c4cbeb0994d4b6'
MONITOR_TEMPLATE = '946e192e53d54482d0758aac4260575a617f2495b43ad87b4dd1a2f3257465a2'
QT_UNIT_SHA = 'e101679abdc622ad35253ec6a79cbb7a9178fafc474d50802026d8c37f9d9593'
QT_WRAPPER_SHA = '9c0bf58d8002f76407b1a40bbf26a8957e63dc5bfc2df2fe13f9eb04332d5f52'
BRIDGE_UNIT_SHA = '3e219d806f05bd6746cc97a8e57453288577e2cc22fb2fb2aa74f65d1045d2d5'
EMPTY_MAP_SHA = '37517e5f3dc66819f61f5a7bb8ace1921282415f10551d2defa5c3eb0985b570'
LAUNCHER_UNIT_SHA = 'eb6ca219d61ea2c832a1b2e08b1c2b7e7edbde9d3bd1285d040bd4306fb82705'
WATCHDOG_UNIT_SHA = '0f15974d09185ca6b042b817eb686975988a9050c85dac08f1119faccbb1d323'
WATCHDOG_SCRIPT_SHA = 'fa44162d8deae152ed76bc0bf3bf743c3f2515b289ab4227d1597f1630f64da8'


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def exact_file(path):
    if any(part.is_symlink() for part in (path,) + tuple(path.parents)) or not path.is_file() or path.stat().st_nlink != 1:
        raise ValueError('activation file must be single-link regular: ' + str(path))
    return path


def scada_project_files(home, target, pins):
    if (not isinstance(target, str) or re.fullmatch(r'releases/[A-Za-z0-9_-][A-Za-z0-9_.-]*', target) is None or
            not isinstance(pins, dict) or not 1 <= len(pins) <= 64):
        raise ValueError('invalid read-only SCADA release pins')
    root = home / 'scada' / target
    paths = list(root.rglob('*'))
    if (not root.is_dir() or any(p.is_symlink() for p in [root] + list(root.parents) + paths) or
            {str(p.relative_to(root)) for p in paths if not p.is_dir()} != set(pins)):
        raise ValueError('read-only SCADA project inventory changed')
    raw = {}
    for name, pin in pins.items():
        path = exact_file(root / name)
        if path.stat().st_size > 2 * 1024 * 1024:
            raise ValueError('read-only SCADA file exceeds bound')
        raw[name] = path.read_bytes()
        if not isinstance(pin, str) or hashlib.sha256(raw[name]).hexdigest() != pin:
            raise ValueError('read-only SCADA file pin changed: ' + name)
    if sum(len(value) for value in raw.values()) > 8 * 1024 * 1024:
        raise ValueError('read-only SCADA project exceeds bound')
    return raw


def scada_readonly_binding(home, approval, side, check_current=True):
    pins = approval.get('scadaReadOnlyProject')
    if (not isinstance(pins, dict) or set(pins) != {'oldTarget', 'oldFilesSha256', 'newTarget', 'newFilesSha256'} or
            pins['oldTarget'] == pins['newTarget'] or approval.get('mode') != 'standalone' or
            'qtDisplayEnvSha256' not in approval):
        raise ValueError('invalid A read-only SCADA approval')
    current = home / 'scada/current'
    if check_current and (not current.is_symlink() or os.readlink(str(current)) != pins[side + 'Target']):
        raise ValueError('read-only SCADA current binding changed')
    raw = scada_project_files(home, pins[side + 'Target'], pins[side + 'FilesSha256'])
    if side == 'old':
        if raw.get('runtime-map.json') != b'[]\n':
            raise ValueError('read-only SCADA requires the qualified old empty map')
        return
    if set(raw) != {'manifest.json', 'topology.json', 'nodes.json', 'tags.json',
                    'runtime-map.json', 'screens/overview.json'}:
        raise ValueError('unsupported read-only SCADA project files')
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate read-only SCADA JSON key')
            result[key] = value
        return result
    docs = {name: json.loads(value.decode('utf-8'), object_pairs_hook=unique_object) for name, value in raw.items()}
    manifest = docs['manifest.json']
    if (not isinstance(manifest, dict) or set(manifest) !=
            {'schemaVersion', 'projectId', 'projectName', 'packageVersion', 'entryScreen', 'packageRole'} or
            manifest['schemaVersion'] != '2.0' or manifest['entryScreen'] != 'overview' or
            manifest['packageRole'] != 'project' or
            any(not isinstance(manifest[k], str) or re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', manifest[k]) is None
                for k in ('projectId', 'packageVersion')) or not isinstance(manifest['projectName'], str)):
        raise ValueError('unsupported read-only SCADA manifest')
    if docs['topology.json'] != {'mode': 'integrated', 'scadaHost': 'edge', 'emsHost': 'edge',
                                 'dataTransport': 'sharedMemory', 'offlinePolicy': 'continueLocal'}:
        raise ValueError('unsupported read-only SCADA topology')
    nodes = docs['nodes.json']
    if (not isinstance(nodes, list) or len(nodes) != 1 or not isinstance(nodes[0], dict) or
            set(nodes[0]) != {'nodeId', 'machineCode', 'displayName', 'roles'} or
            nodes[0]['machineCode'] != approval['nodeId'] or nodes[0]['roles'] != [] or
            not isinstance(nodes[0]['displayName'], str) or not isinstance(nodes[0]['nodeId'], str) or
            re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', nodes[0]['nodeId']) is None):
        raise ValueError('unsupported read-only SCADA node')
    node = nodes[0]['nodeId']
    target = next(s['target'] for s in approval['segments'] if s['source'] == 'gateway_point_store_system_monitor')
    tags, mappings = docs['tags.json'], docs['runtime-map.json']
    if not isinstance(tags, list) or not isinstance(mappings, list) or not 1 <= len(tags) == len(mappings) <= 9:
        raise ValueError('read-only SCADA requires explicit monitor mappings')
    tag_ids, indexes = set(), set()
    for tag, mapping in zip(tags, mappings):
        if (not isinstance(tag, dict) or not isinstance(mapping, dict) or set(tag) !=
                {'nodeId', 'tagId', 'access', 'dataType', 'unit', 'indexFallback'} or set(mapping) !=
                {'nodeId', 'tagId', 'sharedMemoryName', 'index', 'writable', 'dataType', 'unit'} or
                not isinstance(tag['tagId'], str) or re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', tag['tagId']) is None or
                tag['tagId'] in tag_ids or tag['nodeId'] != node or tag['access'] != 'read' or
                type(tag['indexFallback']) is not int or tag['indexFallback'] != 0 or
                tag['dataType'] != 'float64' or not isinstance(tag['unit'], str) or
                any(mapping[k] != tag[k] for k in ('nodeId', 'tagId', 'dataType', 'unit')) or
                mapping['sharedMemoryName'] != target or mapping['writable'] is not False or
                type(mapping['index']) is not int or not 920000001 <= mapping['index'] <= 920000009 or
                mapping['index'] in indexes):
            raise ValueError('unsupported read-only SCADA monitor mapping')
        tag_ids.add(tag['tagId'])
        indexes.add(mapping['index'])
    screen = docs['screens/overview.json']
    if (not isinstance(screen, dict) or set(screen) != {'screenId', 'title', 'width', 'height', 'widgets'} or
            screen['screenId'] != 'overview' or not isinstance(screen['title'], str) or
            any(type(screen[k]) is not int or not 1 <= screen[k] <= 4096 for k in ('width', 'height')) or
            not isinstance(screen['widgets'], list) or len(screen['widgets']) != len(tags)):
        raise ValueError('unsupported read-only SCADA screen')
    widget_ids, bound = set(), set()
    for widget in screen['widgets']:
        if (not isinstance(widget, dict) or set(widget) !=
                {'widgetId', 'type', 'title', 'geometry', 'zIndex', 'visible', 'bindings', 'properties'} or
                widget['type'] != 'metricCard' or widget['visible'] is not True or widget['properties'] != {} or
                not isinstance(widget['title'], str) or type(widget['zIndex']) is not int or
                not isinstance(widget['widgetId'], str) or widget['widgetId'] in widget_ids or
                re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', widget['widgetId']) is None):
            raise ValueError('unsupported read-only SCADA widget')
        geometry, binding = widget['geometry'], widget['bindings']
        if (not isinstance(geometry, dict) or set(geometry) != {'x', 'y', 'width', 'height'} or
                any(type(v) is not int for v in geometry.values()) or
                min(geometry['x'], geometry['y']) < 0 or min(geometry['width'], geometry['height']) <= 0 or
                geometry['x'] + geometry['width'] > screen['width'] or geometry['y'] + geometry['height'] > screen['height'] or
                not isinstance(binding, list) or len(binding) != 1 or not isinstance(binding[0], dict) or
                set(binding[0]) != {'nodeId', 'tagId', 'slot'} or binding[0]['nodeId'] != node or
                binding[0]['slot'] != 'value' or binding[0]['tagId'] not in tag_ids or binding[0]['tagId'] in bound):
            raise ValueError('unsupported read-only SCADA widget binding')
        widget_ids.add(widget['widgetId'])
        bound.add(binding[0]['tagId'])


def active_context(home):
    marker = home / 'data/runtime-upgrade-stop'
    active_path = home / 'data' / ACTIVATION_FILE
    if (not marker.exists() and not marker.is_symlink()) or not active_path.exists():
        raise ValueError('persistent offline upgrade fence active; no approved activation')
    exact_file(marker)
    active = json.loads(exact_file(active_path).read_text())
    state_dir = Path(active['stateDir'])
    if (not state_dir.is_absolute() or any(part.is_symlink() for part in (state_dir,) + tuple(state_dir.parents)) or
            home in state_dir.parents):
        raise ValueError('invalid activation state directory')
    state_path = exact_file(state_dir / 'state.json')
    if sha(state_path) != active['stateSha256']:
        raise ValueError('activation state changed')
    state = json.loads(state_path.read_text())
    approval_path = exact_file(state_dir / 'approval.json')
    ready_path = exact_file(state_dir / 'activation-ready.json')
    if sha(approval_path) != active['approvalSha256'] or sha(ready_path) != active['readySha256']:
        raise ValueError('activation approval changed')
    approval = json.loads(approval_path.read_text())
    ready = json.loads(ready_path.read_text())
    readers = [MONITOR_UNIT] if state.get('activationProfile') == 'B_MONITOR' else [MONITOR_UNIT, QT_UNIT]
    watchdog = ('gateway-health-watchdog.service' in approval['units'] and readers == [MONITOR_UNIT])
    allowed = ['gateway-services.service'] + (['gateway-health-watchdog.service'] if watchdog else []) + readers
    if (state.get('phase') != 'ACTIVATED_CONTROL_DISABLED' or
            state.get('activationProfile') not in ('B_MONITOR', 'A_MONITOR_QT') or
            state.get('activationReadySha256') != active['readySha256'] or
            state.get('activatedUnits') != allowed or active.get('allowedUnits') != allowed or
            ready.get('schemaVersion') != 'offline-shm11-activate-1' or
            ready.get('startUnits') != readers or ready.get('controlEnabled') is not False or
            ready.get('approvalSha256') != active['approvalSha256'] or
            approval.get('controlEnabled') is not False or approval.get('gatewayHome') != str(home) or
            approval.get('transactionId') != active.get('transactionId') or
            marker.read_text().strip() != active['transactionId']):
        raise ValueError('activation scope or lineage changed')
    if (sha(exact_file(home / 'bin/runtime-upgrade-guard.py')) != state['activationGuardSha256'] or
            sha(exact_file(home / 'bin/gateway-services.sh')) != state['activationLauncherSha256']):
        raise ValueError('activation paired startup scripts changed')
    return active, state, approval, readers, allowed


def active_dropin(home, transaction, unit):
    path = Path('/etc/systemd/system') / (unit + '.d') / ('90-offline-' + transaction + '.conf')
    body = ('[Service]\nExecStartPre=/usr/bin/python3 ' + str(home / 'bin/runtime-upgrade-guard.py') +
            ' activated-unit ' + str(home) + ' ' + unit + '\n').encode()
    return path, body


def denied_dropin(home, transaction, unit):
    path = Path('/etc/systemd/system') / (unit + '.d') / ('90-offline-' + transaction + '.conf')
    body = ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n'
            '[Service]\nExecStartPre=/usr/bin/python3 ' + str(home / 'bin/runtime-upgrade-guard.py') +
            ' denied-unit ' + str(home) + ' ' + unit + '\n').encode()
    return path, body


def systemctl_value(unit, prop):
    result = subprocess.run(['systemctl', 'show', '--property=' + prop, '--value', unit],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=45)
    if result.returncode:
        raise ValueError('systemctl show failed: ' + unit + ' ' + prop)
    return result.stdout.strip()


def no_old_mappings(names):
    identities = set()
    for name in names:
        path = Path('/dev/shm') / name
        if not path.exists():
            continue
        value = exact_file(path).stat()
        identities.add((os.major(value.st_dev), os.minor(value.st_dev), value.st_ino))
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            for line in (entry / 'maps').read_text().splitlines():
                fields = line.split()
                dev = fields[3].split(':')
                mapped = ' '.join(fields[5:])
                if ((int(dev[0], 16), int(dev[1], 16), int(fields[4])) in identities or
                        any(mapped in ('/dev/shm/' + name, '/dev/shm/' + name + ' (deleted)') for name in names)):
                    raise ValueError('old SHM still mapped: ' + entry.name)
        except (FileNotFoundError, ProcessLookupError):
            continue


def activated_unit(home, unit):
    active, state, approval, readers, allowed = active_context(home)
    if unit not in allowed:
        raise ValueError('unit not approved for control-disabled activation: ' + unit)
    for selected in allowed:
        path, body = active_dropin(home, active['transactionId'], selected)
        if exact_file(path).read_bytes() != body:
            raise ValueError('activation unit gate changed: ' + selected)
        expected = {str(path)}
        if selected == MONITOR_UNIT:
            expected.add('/etc/systemd/system/' + MONITOR_UNIT + '.d/998-shm.conf')
        if set(systemctl_value(selected, 'DropInPaths').split()) != expected:
            raise ValueError('activation effective unit gate changed: ' + selected)
    for selected in set(approval['units']) - set(allowed):
        path, denied = denied_dropin(home, active['transactionId'], selected)
        if exact_file(path).read_bytes() != denied:
            raise ValueError('unapproved unit inhibition changed: ' + selected)
    fixed_units = {'gateway-services.service': LAUNCHER_UNIT_SHA,
                   MONITOR_UNIT: MONITOR_TEMPLATE,
                   QT_UNIT: QT_UNIT_SHA,
                   'gateway-health-watchdog.service': WATCHDOG_UNIT_SHA}
    for selected in allowed:
        fragment = ('system-monitor@.service' if selected == MONITOR_UNIT else selected)
        path = Path('/etc/systemd/system') / fragment
        if (sha(exact_file(path)) != fixed_units[selected] or
                systemctl_value(selected, 'FragmentPath') != str(path)):
            raise ValueError('activated unit fragment changed: ' + selected)
    if 'gateway-health-watchdog.service' in allowed:
        if (sha(exact_file(home / 'bin/gateway-health-watchdog.sh')) != WATCHDOG_SCRIPT_SHA or
                Path('/etc/default/gateway-health-watchdog').exists() or
                Path('/etc/default/gateway-health-watchdog').is_symlink()):
            raise ValueError('activated watchdog entry changed')
    config = home / 'config/runtime'
    actual = {str(p.relative_to(config)): sha(exact_file(p)) for p in config.rglob('*') if p.is_file()}
    if actual != state['configSha256']:
        raise ValueError('activated configuration changed')
    names = set()
    for path in config.rglob('*.json'):
        names.update(configured_names(json.loads(path.read_text())))
    sources = {segment['source'] for segment in state['segments']}
    targets = {segment['target'] for segment in state['segments']}
    if names & sources or not names <= targets or not state.get('unreferencedDefaultShm'):
        raise ValueError('activated SHM consumer binding changed')
    boot_changed = Path('/proc/sys/kernel/random/boot_id').read_text().strip() != state['activationBootId']
    source_metadata = state['activationSourceMetadata']
    if set(source_metadata) != sources:
        raise ValueError('activated source metadata scope changed')
    for segment in state['segments']:
        source = Path('/dev/shm') / segment['source']
        target = Path('/dev/shm') / segment['target']
        if source.exists() or source.is_symlink():
            if boot_changed:
                raise ValueError('old SHM recreated after activation boot')
            info = exact_file(source).stat()
            if source_metadata[segment['source']] != {'dev': info.st_dev, 'ino': info.st_ino,
                    'size': info.st_size, 'mtimeNs': info.st_mtime_ns, 'ctimeNs': info.st_ctime_ns}:
                raise ValueError('old SHM metadata changed')
            if segment['source'] == 'gateway_point_store' and sha(source) != segment['sourceSha256']:
                raise ValueError('old default SHM changed')
        elif not boot_changed:
            raise ValueError('old SHM disappeared in same boot')
        if target.exists() or target.is_symlink():
            with exact_file(target).open('rb') as stream:
                if stream.read(8) != struct.pack('<II', 0x4d505354, 11):
                    raise ValueError('activated target SHM ABI mismatch')
        elif not boot_changed:
            raise ValueError('target SHM disappeared in same boot')
    no_old_mappings(sources)
    if sha(home / 'bin/SystemMonitor') != MONITOR_BINARY or sha(Path('/etc/systemd/system/system-monitor@.service')) != MONITOR_TEMPLATE:
        raise ValueError('activated monitor binary/template changed')
    if state['activationProfile'] == 'A_MONITOR_QT':
        if (sha(home / 'ky-ems/KY-EMS') != QT_BINARY or
                sha(home / 'bin/gateway-qt-run.sh') != QT_WRAPPER_SHA or
                sha(Path('/etc/systemd/system/ky-ems.service')) != QT_UNIT_SHA or
                sha(Path('/etc/systemd/system/qt-display-bridge.service')) != BRIDGE_UNIT_SHA or
                systemctl_value('qt-display-bridge.service', 'ActiveState') not in ('inactive', 'failed') or
                systemctl_value('qt-display-bridge.service', 'MainPID') != '0'):
            raise ValueError('activated Qt/bridge binding changed')
    monitor_target = next(s['target'] for s in state['segments'] if s['source'] == state['activationMonitorSource'])
    monitor_dropin = exact_file(Path('/etc/systemd/system') / (MONITOR_UNIT + '.d/998-shm.conf'))
    if (monitor_dropin.read_bytes() != ('[Service]\nEnvironment=GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' +
            monitor_target + '\n').encode() or stat.S_IMODE(monitor_dropin.stat().st_mode) != 0o644):
        raise ValueError('activated monitor drop-in changed')
    if systemctl_value(MONITOR_UNIT, 'Environment') != 'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + monitor_target:
        raise ValueError('activated monitor environment changed')
    if state['activationProfile'] == 'A_MONITOR_QT':
        env = exact_file(config / 'qt-display.env').read_bytes()
        if b'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + monitor_target.encode() not in env:
            raise ValueError('activated Qt environment changed')
        app = json.loads(exact_file(config / 'apps/monitor-service.json').read_text())
        scada = app['localDisplay']['scada']
        current = home / 'scada/current'
        if 'scadaReadOnlyProject' in approval:
            if (state.get('scadaReadOnlyProject') != approval['scadaReadOnlyProject'] or
                    scada.get('enabled') is not True or scada.get('projectDirectory') != str(current) or
                    scada.get('autoReload') is not False):
                raise ValueError('activated read-only SCADA configuration changed')
            scada_readonly_binding(home, approval, 'new')
        elif (scada.get('enabled') is not True or scada.get('projectDirectory') != str(current) or
                not current.is_symlink() or re.fullmatch(r'releases/[A-Za-z0-9_.-]+', os.readlink(str(current))) is None or
                sha(exact_file(home / 'scada' / os.readlink(str(current)) / 'runtime-map.json')) != EMPTY_MAP_SHA):
            raise ValueError('activated SCADA map binding changed')
        bridge_dropin = Path('/etc/systemd/system/qt-display-bridge.service.d') / ('90-offline-' + active['transactionId'] + '.conf')
        _, body = denied_dropin(home, active['transactionId'], 'qt-display-bridge.service')
        if (exact_file(bridge_dropin).read_bytes() != body or
                systemctl_value('qt-display-bridge.service', 'DropInPaths').split() != [str(bridge_dropin)] or
                systemctl_value('graphical.target', 'ActiveState') != 'active'):
            raise ValueError('activated bridge inhibition changed')
    return readers


def compatibility(document):
    value = document.get("runtimeCompatibility")
    if value is None and "runtimeCompatibility" not in document:
        return None
    if not isinstance(value, dict) or set(value) != set(COMPATIBILITY):
        raise ValueError("runtimeCompatibility requires exactly pointStoreAbi, clusterProtocol, upgradeMode")
    for key, expected in COMPATIBILITY.items():
        if type(value[key]) is not type(expected) or value[key] != expected:
            raise ValueError("unsupported or mistyped runtimeCompatibility." + key)
    return value


def elf(path):
    if not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def runtime_file(source, destination):
    return destination.name in RUNTIME_BINARIES or elf(source) or elf(destination)


def ota(manifest_path):
    if Path('/opt/modbus-gateway/data/runtime-upgrade-stop').exists():
        raise ValueError('offline upgrade fence active; ordinary OTA cannot change this runtime')
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("manifest must be an object")
    compatibility(manifest)
    files = manifest.get('files')
    if not isinstance(files, list) or not files or any(not isinstance(item, dict) for item in files):
        raise ValueError("manifest files must be a non-empty object array")
    for item in files:
        source = (manifest_path.parent / str(item.get('path', ''))).resolve()
        if manifest_path.parent.resolve() not in source.parents or not source.is_file():
            raise ValueError("unsafe or missing OTA source")
        destination = Path(str(item.get('target', '')))
        if runtime_file(source, destination):
            raise ValueError("runtime ELF OTA requires the offline all-participant upgrade; declarations alone do not prove ABI compatibility")


def rollback(backup):
    if Path('/opt/modbus-gateway/data/runtime-upgrade-stop').exists():
        raise ValueError('offline upgrade fence active; use offline recovery')
    # Ordinary OTA cannot prove the old/new participant ABI from a backup filename.
    for top in ('opt', 'etc'):
        root = backup / top
        if not root.exists():
            continue
        for source in root.rglob('*'):
            if source.is_symlink():
                raise ValueError("symlink in rollback backup")
            if source.is_file() and runtime_file(source, Path('/') / source.relative_to(backup)):
                raise ValueError("runtime ELF rollback requires offline all-participant recovery; keep control stopped")


def factory(home):
    for path in (home / 'config/runtime', home / 'data', home / 'bin'):
        if path.exists() and any(path.iterdir()):
            raise ValueError("factory initialization is not an in-place runtime upgrade: " + str(path))
    for segment in Path('/dev/shm').iterdir():
        if segment.is_symlink() or not segment.is_file():
            continue
        with segment.open('rb') as stream:
            if stream.read(4) == struct.pack('<I', 0x4d505354):
                raise ValueError("cold initialization requires no existing point-store segments: " + segment.name)
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            executable = os.readlink(str(entry / 'exe'))
            if executable.endswith(' (deleted)'):
                executable = executable[:-10]
        except FileNotFoundError:
            continue
        except PermissionError:
            raise ValueError("cannot verify absence of old runtime processes")
        if Path(executable).name in RUNTIME_BINARIES:
            raise ValueError("old runtime participant is still running: " + entry.name)


def shm_name(value):
    if not isinstance(value, str):
        raise ValueError('SHM reference must be a string')
    if not value:
        return ''
    name = value[1:] if value.startswith('/') else value
    if re.fullmatch('[A-Za-z0-9_-]{1,63}', name) is None:
        raise ValueError('unsupported SHM reference: ' + value)
    return name


def configured_names(value, disabled_ems=False):
    names = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key in SINGLE_SHM_KEYS:
                name = shm_name(item)
                if name and not (disabled_ems and key == 'virtualSharedMemoryName'):
                    names.add(name)
            elif key in MULTI_SHM_KEYS:
                if not isinstance(item, list):
                    raise ValueError('SHM reference list required')
                names.update(shm_name(name) for name in item if shm_name(name))
            elif key.lower().endswith(('sharedmemoryname', 'sharedmemorynames')):
                raise ValueError('unsupported SHM reference key: ' + key)
            else:
                names.update(configured_names(item, key == 'emsCluster' and isinstance(item, dict)
                                              and item.get('enabled') is False))
    elif isinstance(value, list):
        for item in value:
            names.update(configured_names(item))
    return names


def startup(home):
    marker = home / 'data/runtime-upgrade-stop'
    if marker.exists() or marker.is_symlink():
        activated_unit(home, 'gateway-services.service')
        return
    names = {'gateway_point_store'}
    for config in (home / 'config/runtime').rglob('*.json'):
        names.update(configured_names(json.loads(config.read_text())))
    for name in names:
        if not name or any(ch not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for ch in name):
            raise ValueError("unsafe point-store name")
        path = Path('/dev/shm') / name
        if not path.exists():
            continue
        if path.is_symlink() or not path.is_file():
            raise ValueError("point-store must be a regular non-symlink segment: " + name)
        with path.open('rb') as stream:
            header = stream.read(8)
        if header != struct.pack('<II', 0x4d505354, 11):
            raise ValueError("point-store ABI mismatch; preserve it and use offline migration: " + name)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'denied-unit':
        raise ValueError('unit remains inhibited by offline activation: ' + sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == 'activated-unit':
        activated_unit(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == 'activated-list':
        _, _, _, readers, _ = active_context(Path(sys.argv[2]))
        print('\n'.join(readers))
    elif len(sys.argv) == 3 and sys.argv[1] in ('ota', 'rollback', 'factory', 'startup'):
        globals()[sys.argv[1]](Path(sys.argv[2]))
    else:
        raise ValueError('usage: runtime-upgrade-guard.py ota|rollback|factory|startup|activated-list|activated-unit PATH [UNIT]')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('runtime upgrade refused: ' + str(error), file=sys.stderr)
        sys.exit(2)
