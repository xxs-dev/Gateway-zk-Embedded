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
A_ACQUISITION_PROFILE = 'A_RTU_READ_MQTT_TX_FULL'
A_WINDOWS_PROFILE = 'A_RTU_READ_MQTT_WINDOWS_READONLY'
A_RTU_UNIT = 'modbus-rtu@device_modbusRTU_2_readonly.service'
A_MQTT_UNIT = 'mqtt-driver@mqtt-service.service'
A_ACQUISITION_READERS = [A_RTU_UNIT, A_MQTT_UNIT, MONITOR_UNIT, QT_UNIT]
A_RTU_TEMPLATE = '62c530751dd310a7612a5d3176f0549260b79a968e2e37582bfd7eb4c4524434'
A_MQTT_TEMPLATE = '83d021d45aa2120393248b97c1b84fd27cddf908aecdff124c4b00e508cca3ac'
A_SOURCE_CONFIGS = {
    'apps/camera-service.json': 'c4c61e0eee91bc88833a22bcb47cc7fff36c526e1afdfad54f519cc8ee22d0e8',
    'apps/monitor-service.json': '6a64c844c339a826986bfeea7de2c7af85526a439ae9d36c8fe1f1d2bcf1ba0b',
    'apps/mqtt-service.json': 'a656fe30d5a7220cad5efcde04f40b4211393fb56bfa867eaf3f9b097d8f542b',
    'devices/device_modbusRTU_2_readonly.json': '75e3740da48961bf928b0cdeeb9815ff6919f1e233ef8d0126679a2750abbdf0'}
A_RX_TOPICS = ('commandRequestTopic', 'otaRequestTopic', 'realtimeRequestTopic',
               'systemMonitorRequestTopic', 'diagRequestTopic', 'configPullRequestTopic',
               'configApplyRequestTopic', 'configDeleteRequestTopic', 'configRestoreRequestTopic',
               'recordingRequestTopic', 'recordingAckTopic')
A_WINDOWS_RX = {
    'apps/mqtt-service.json': {'realtimeRequestTopic': 'edge/telemetry/realtime/request'},
    'apps/monitor-service.json': {'systemMonitorRequestTopic': 'edge/system/monitor/request',
                                  'configPullRequestTopic': 'edge/config/pull/request'}}


def a_acquisition_profile(approval):
    return A_WINDOWS_PROFILE if approval.get('aReadonlyWindowsIntegration') is True else A_ACQUISITION_PROFILE


def a_acquisition_scope(home, approval):
    if (approval.get('aReadonlyAcquisition') is not True or home != Path('/opt/modbus-gateway') or
            ('aReadonlyWindowsIntegration' in approval and approval['aReadonlyWindowsIntegration'] is not True) or
            approval.get('mode') != 'standalone' or approval.get('controlEnabled') is not False or
            not approval.get('scadaReadOnlyProject') or not approval.get('qtDisplayEnvSha256') or
            approval.get('aInboundDisableSourceSha256') != A_SOURCE_CONFIGS['apps/monitor-service.json'] or
            not set(A_ACQUISITION_READERS + ['gateway-services.service', 'qt-display-bridge.service',
                    'modbus-rtu@.service', 'mqtt-driver@.service']) <= set(approval['units']) or
            any(approval['configSha256'].get(path) != pin for path, pin in A_SOURCE_CONFIGS.items())):
        raise ValueError('A acquisition requires exact original config pins and joint scope')


def switch_names(value, names):
    if isinstance(value, dict):
        return {key: (names.get(shm_name(item), item) if key in SINGLE_SHM_KEYS else
                      [names.get(shm_name(v), v) for v in item] if key in MULTI_SHM_KEYS
                      else switch_names(item, names)) for key, item in value.items()}
    if isinstance(value, list):
        return [switch_names(item, names) for item in value]
    return value


def a_acquisition_patch(relative, document, approval):
    # Only the approved MQTT RX fields change; defaults, full upload and auth stay intact.
    windows = a_acquisition_profile(approval) == A_WINDOWS_PROFILE
    if relative == 'apps/mqtt-service.json' or (windows and relative == 'apps/monitor-service.json'):
        allowed = A_WINDOWS_RX[relative] if windows else {}
        for key in A_RX_TOPICS:
            document['mqtt'][key] = allowed.get(key, '')
    if windows and relative == 'apps/monitor-service.json':
        document['systemMonitor']['recordingTransfer']['enabled'] = False
    return document


def a_acquisition_config(home, state_dir, approval, state):
    a_acquisition_scope(home, approval)
    names = {item['source']: item['target'] for item in state['segments']}
    if names != {item['source']: item['target'] for item in approval['segments']}:
        raise ValueError('A acquisition remap approval changed')
    config = home / 'config/runtime'
    inventory = {str(p.relative_to(config)) for folder in ('apps', 'devices')
                 for p in (config / folder).rglob('*.json')}
    if inventory != set(A_SOURCE_CONFIGS):
        raise ValueError('A acquisition config inventory changed')
    for relative, pin in A_SOURCE_CONFIGS.items():
        rows = [(i, row) for i, row in enumerate(state['files'])
                if row['path'] == 'config/runtime/' + relative]
        if len(rows) > 1 or rows and rows[0][1]['oldSha256'] != pin:
            raise ValueError('A acquisition original backup binding changed')
        current = exact_file(config / relative)
        # A disabled camera with no remapped fields may remain byte-for-byte original.
        original = exact_file(state_dir / 'files' / str(rows[0][0])) if rows else current
        if sha(original) != pin:
            raise ValueError('A acquisition original backup changed')
        expected = switch_names(json.loads(original.read_text()), names)
        if relative == 'apps/monitor-service.json':
            expected['mqtt']['enabled'] = a_acquisition_profile(approval) == A_WINDOWS_PROFILE
            expected['systemMonitor']['directMaintenance']['enabled'] = False
            expected['localDisplay']['scada']['autoReload'] = False
        a_acquisition_patch(relative, expected, approval)
        expected_pin = rows[0][1]['newSha256'] if rows else pin
        if (sha(current) != expected_pin or state['configSha256'].get(relative) != expected_pin or
                json.dumps(json.loads(current.read_text()), sort_keys=True) != json.dumps(expected, sort_keys=True)):
            raise ValueError('A acquisition exceeds the approved minimal config patch')
    manifest_path = exact_file(state_dir / 'program-manifest.json')
    if sha(manifest_path) != approval['programManifestSha256']:
        raise ValueError('A acquisition manifest changed')
    components = json.loads(manifest_path.read_text())['components']
    for name in ('ModbusRtu', 'MqttDriver'):
        selected = [c for c in components if c.get('kind') == 'product' and c.get('target') == name]
        if (len(selected) != 1 or approval.get('installPaths', {}).get(name) != 'bin/' + name or
                sha(exact_file(home / 'bin' / name)) != selected[0]['sha256']):
            raise ValueError('A acquisition installed binary not pinned: ' + name)


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
    acquisition = state.get('activationProfile') in (A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE)
    if acquisition or 'aReadonlyAcquisition' in approval or 'aReadonlyWindowsIntegration' in approval:
        a_acquisition_scope(home, approval)
        if not acquisition or state['activationProfile'] != a_acquisition_profile(approval):
            raise ValueError('A acquisition cannot fall back to an observer-only profile')
    readers = (A_ACQUISITION_READERS if acquisition else
               [MONITOR_UNIT] if state.get('activationProfile') == 'B_MONITOR' else [MONITOR_UNIT, QT_UNIT])
    watchdog = ('gateway-health-watchdog.service' in approval['units'] and readers == [MONITOR_UNIT])
    allowed = ['gateway-services.service'] + (['gateway-health-watchdog.service'] if watchdog else []) + readers
    if (state.get('phase') != 'ACTIVATED_CONTROL_DISABLED' or
            state.get('activationProfile') not in ('B_MONITOR', 'A_MONITOR_QT', A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE) or
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


def a_acquisition_unit_properties(unit):
    fields = ('Id', 'LoadState', 'NeedDaemonReload', 'FragmentPath', 'DropInPaths', 'WorkingDirectory',
              'Environment', 'EnvironmentFiles', 'PassEnvironment', 'UnsetEnvironment',
              'ExecStart', 'ExecStartPre', 'ExecStartPost', 'ExecStop', 'ExecStopPost',
              'Requires', 'Wants', 'BindsTo', 'Triggers', 'TriggeredBy')
    result = subprocess.run(['systemctl', 'show', '--all', '--property=' + ','.join(fields), unit],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=45)
    if result.returncode:
        raise ValueError('A acquisition effective unit query failed: ' + unit)
    values = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition('=')
        if not separator or key not in fields or key in values:
            raise ValueError('A acquisition effective unit output invalid: ' + unit)
        values[key] = value
    return values


def a_acquisition_units(home, approval, monitor_target, inhibited):
    """Fixed-file proof for this one profile, never a generic systemd parser."""
    root = Path('/etc/systemd/system')
    fence_name = '90-offline-' + approval['transactionId'] + '.conf'
    fence = ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n').encode()
    specs = (
        (A_RTU_UNIT, 'modbus-rtu@.service', A_RTU_TEMPLATE, str(home / 'bin/ModbusRtu'),
         ' --config ' + str(home / 'config/runtime/devices/device_modbusRTU_2_readonly.json'), r'system-modbus\x2drtu.slice'),
        (A_MQTT_UNIT, 'mqtt-driver@.service', A_MQTT_TEMPLATE, str(home / 'bin/MqttDriver'),
         ' --app-config ' + str(home / 'config/runtime/apps/mqtt-service.json'), r'system-mqtt\x2ddriver.slice'),
        (MONITOR_UNIT, 'system-monitor@.service', MONITOR_TEMPLATE, str(home / 'bin/SystemMonitor'),
         ' --app-config ' + str(home / 'config/runtime/apps/monitor-service.json'), r'system-system\x2dmonitor.slice'),
        (QT_UNIT, QT_UNIT, QT_UNIT_SHA, '/bin/sh', ' ' + str(home / 'bin/gateway-qt-run.sh'), 'system.slice'),
        ('gateway-services.service', 'gateway-services.service', LAUNCHER_UNIT_SHA,
         str(home / 'bin/gateway-services.sh'), ' apply', 'system.slice'))
    derived = {}
    for unit, template, pin, executable, arguments, slice_unit in specs:
        fragment = root / template
        if sha(exact_file(fragment)) != pin:
            raise ValueError('A acquisition unit fragment not pinned: ' + unit)
        expected = {root / (unit + '.d') / fence_name:
                    fence if inhibited else active_dropin(home, approval['transactionId'], unit)[1]}
        if unit == MONITOR_UNIT:
            expected[root / (unit + '.d') / '998-shm.conf'] = (
                '[Service]\nEnvironment=GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + monitor_target + '\n').encode()
        effective_paths = {str(p) for p in expected}
        if template != unit and template in approval['units']:
            expected[root / (template + '.d') / fence_name] = (
                fence if inhibited else denied_dropin(home, approval['transactionId'], template)[1])
        # Check shadowed template files too; effective DropInPaths alone cannot prove their content.
        directory_names = {unit + '.d', template + '.d', 'service.d'}
        prefix = template.split('@')[0]
        if prefix.endswith('.service'):
            prefix = prefix[:-8]
        for i, character in enumerate(prefix):
            if character == '-':
                directory_names.add(prefix[:i + 1] + '.service.d')
        for base in ('/etc/systemd/system', '/run/systemd/system', '/usr/local/lib/systemd/system',
                     '/usr/lib/systemd/system', '/lib/systemd/system', '/etc/systemd/system.control',
                     '/run/systemd/system.control', '/run/systemd/transient', '/run/systemd/generator',
                     '/run/systemd/generator.early', '/run/systemd/generator.late'):
            for dirname in directory_names:
                directory = Path(base) / dirname
                wanted = {p.name for p in expected if p.parent == directory}
                if directory.is_symlink() or (directory.exists() and (
                        not directory.is_dir() or {p.name for p in directory.iterdir()} != wanted)) or (
                        wanted and not directory.exists()):
                    raise ValueError('A acquisition unknown drop-in coverage: ' + unit)
        for path, body in expected.items():
            if exact_file(path).read_bytes() != body:
                raise ValueError('A acquisition drop-in bytes changed: ' + unit)
        values = a_acquisition_unit_properties(unit)
        complex_fields = {'EnvironmentFiles', 'ExecStartPre', 'ExecStartPost', 'ExecStop', 'ExecStopPost'}
        mandatory = {'Id', 'LoadState', 'NeedDaemonReload', 'FragmentPath', 'DropInPaths', 'WorkingDirectory',
                     'Environment', 'PassEnvironment', 'UnsetEnvironment', 'ExecStart',
                     'Requires', 'Wants', 'BindsTo', 'Triggers', 'TriggeredBy'}
        required = {'sysinit.target', slice_unit} | ({'qt-display-bridge.service'} if unit == QT_UNIT else set())
        wants = {'graphical.target'} if unit == QT_UNIT else set()
        environment = ('GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME=' + monitor_target if unit == MONITOR_UNIT else
                       'DISPLAY=:0 QT_QPA_PLATFORM=xcb XDG_RUNTIME_DIR=/run/ky-ems' if unit == QT_UNIT else '')
        if (not mandatory <= set(values) or values['Id'] != unit or values['LoadState'] != 'loaded' or
                values['NeedDaemonReload'] != 'no' or values['FragmentPath'] != str(fragment) or
                set(values['DropInPaths'].split()) != effective_paths or
                values['WorkingDirectory'] != str(home / 'ky-ems' if unit == QT_UNIT else home) or
                set(values['Environment'].split()) != set(environment.split()) or
                values['PassEnvironment'] or values['UnsetEnvironment'] or
                set(values['Requires'].split()) != required or set(values['Wants'].split()) != wants or
                any(values[key] for key in ('BindsTo', 'Triggers', 'TriggeredBy'))):
            raise ValueError('A acquisition effective unit/env/dependency mismatch: ' + unit)

        def command_matches(value, path, args):
            prefix = '{ path=' + path + ' ; argv[]=' + path + args + ' ; ignore_errors=no ;'
            return re.fullmatch(re.escape(prefix) + r'[^{}]*\}', value.strip()) is not None

        if not command_matches(values['ExecStart'], executable, arguments):
            raise ValueError('A acquisition effective ExecStart mismatch: ' + unit)
        expected_complex = dict.fromkeys(complex_fields, '')
        if unit == QT_UNIT:
            expected_complex['EnvironmentFiles'] = str(home / 'config/runtime/qt-display.env') + ' (ignore_errors=yes)'
        if unit == 'gateway-services.service':
            expected_complex['ExecStop'] = (executable, ' stop')
        if not inhibited:
            expected_complex['ExecStartPre'] = ('/usr/bin/python3', ' ' + str(home / 'bin/runtime-upgrade-guard.py') +
                                               ' activated-unit ' + str(home) + ' ' + unit)
        derived[unit] = []
        for key, expected_value in expected_complex.items():
            if key not in values:
                if expected_value:
                    raise ValueError('A acquisition nonempty effective field missing: ' + unit + ' ' + key)
                # Absence follows from exact files + coverage + loaded/no-reload state above,
                # not from an empty --value response or the old WITH_GAPS report.
                derived[unit].append(key)
            elif (isinstance(expected_value, tuple) and not command_matches(values[key], *expected_value) or
                  isinstance(expected_value, str) and values[key] != expected_value):
                raise ValueError('A acquisition unexpected effective field: ' + unit + ' ' + key)
    return {'source': 'STATIC_PINNED_FILES', 'derivedEmptyFields': {u: sorted(v) for u, v in derived.items()}}


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
    if state['activationProfile'] in (A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE):
        fixed_units.update({A_RTU_UNIT: A_RTU_TEMPLATE, A_MQTT_UNIT: A_MQTT_TEMPLATE})
    for selected in allowed:
        fragment = (selected.split('@')[0] + '@.service' if selected in (MONITOR_UNIT, A_RTU_UNIT, A_MQTT_UNIT)
                    else selected)
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
    if state['activationProfile'] in ('A_MONITOR_QT', A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE):
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
    if state['activationProfile'] in ('A_MONITOR_QT', A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE):
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
    if state['activationProfile'] in (A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE):
        a_acquisition_config(home, Path(active['stateDir']), approval, state)
        a_acquisition_units(home, approval, monitor_target, inhibited=False)
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
