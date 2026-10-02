#!/usr/bin/env python3
"""Strict, read-only role resolution for a future configured production baseline.

This module does not grant production-unit admission. The v1 guard and launcher
remain authoritative until a journaled v2 provisioning transaction uses this
result and verifies live effective systemd units.
"""

import configparser
import hashlib
import json
import re
import stat
from pathlib import Path, PurePosixPath
from urllib.parse import urlparse


HOME = PurePosixPath('/opt/modbus-gateway')
FORMAT = 'configured-role-policy-v2'
NAME = re.compile(r'[A-Za-z0-9_-]{1,63}\Z')
SHM = re.compile(r'[A-Za-z0-9_-]{1,127}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
APP_KEYS = frozenset(('runtimeMode', 'identityConfigFile', 'deviceConfigFiles',
                      'pointHistory', 'mqtt', 'mqttForward', 'mqttDriver', 'alarmStore',
                      'eventEngine', 'computeEngine', 'emsCluster', 'realtime',
                      'systemMonitor', 'localDisplay', 'ota', 'cameraService', 'agcAvc',
                      'eventStore'))
SHM_SINGLE = frozenset(('sharedMemoryName', 'virtualSharedMemoryName',
                       'outputSharedMemoryName', 'outputDefaultSharedMemoryName'))
SHM_MULTI = frozenset(('sharedMemoryNames',))
DEFAULT_APPS = {'mqtt': 'mqtt-service', 'monitor': 'monitor-service',
                'camera': 'camera-service', 'agc': 'agc-avc-service'}
TOOLS = {'pointctl': 'bin/pointctl', 'ems-parity-check': 'bin/EmsParityCheck'}
EXTRA_RUNTIME_FILES = {'ky-ems': ('bin/gateway-qt-run.sh',),
                       'local-kiosk': ('bin/local-kiosk.py',),
                       'qt-display-bridge': ('ky-ems/VarList.xml',)}

# reserveMiB values are conservative admission floors, not measured RSS peaks.
# The later capacity gate must add dependencies once and separately budget new
# SHM/tmpfs growth; these numbers cannot by themselves qualify a 2 GiB board.
CATALOG = {
    'modbus-rtu': ('bin/ModbusRtu', '/opt/modbus-gateway/bin/ModbusRtu --config /opt/modbus-gateway/config/runtime/devices/%i.json', 'required', 128, 20),
    'dlt645-driver': ('bin/Dlt645Driver', '/opt/modbus-gateway/bin/Dlt645Driver --config /opt/modbus-gateway/config/runtime/devices/%i.json', 'required', 128, 20),
    'dio-driver': ('bin/DioDriver', '/opt/modbus-gateway/bin/DioDriver --config /opt/modbus-gateway/config/runtime/devices/%i.json --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json', 'required', 128, 20),
    'can-driver': ('bin/CanDriver', '/opt/modbus-gateway/bin/CanDriver --config /opt/modbus-gateway/config/runtime/devices/%i.json --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json', 'required', 128, 20),
    'iec-driver': ('bin/IecDriver', '/opt/modbus-gateway/bin/IecDriver --config /opt/modbus-gateway/config/runtime/devices/%i.json', 'required', 128, 20),
    'compute-engine': ('bin/ComputeEngine', '/opt/modbus-gateway/bin/ComputeEngine --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 30),
    'event-engine': ('bin/EventEngine', '/opt/modbus-gateway/bin/EventEngine --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 128, 40),
    'mqtt-driver': ('bin/MqttDriver', '/opt/modbus-gateway/bin/MqttDriver --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 50),
    'mqtt-forwarder': ('bin/MqttForwarder', '/opt/modbus-gateway/bin/MqttForwarder --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 128, 60),
    'mqtt-tls-tunnel': ('/usr/bin/stunnel', '/usr/bin/stunnel /opt/modbus-gateway/config/runtime/tls/%i.conf', 'none', 64, 10),
    'system-monitor': ('bin/SystemMonitor', '/opt/modbus-gateway/bin/SystemMonitor --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 70),
    'ems-cluster': ('bin/EmsClusterCoordinator', '/opt/modbus-gateway/bin/EmsClusterCoordinator --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 75),
    'agc-avc': ('bin/AgcAvcController', '/opt/modbus-gateway/bin/AgcAvcController --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 80),
    'local-display': ('bin/LocalDisplay', '/opt/modbus-gateway/bin/LocalDisplay --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 256, 90),
    'camera-service': ('bin/CameraService', '/opt/modbus-gateway/bin/CameraService --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'required', 192, 90),
    'qt-display-bridge': ('bin/QtDisplayBridge', '/opt/modbus-gateway/bin/QtDisplayBridge --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json --var-list /opt/modbus-gateway/ky-ems/VarList.xml --interval-ms 500', 'required', 64, 90),
    'ky-ems': ('ky-ems/KY-EMS', '/bin/sh /opt/modbus-gateway/bin/gateway-qt-run.sh', 'optional', 256, 95),
    'local-kiosk': ('/usr/bin/python3', '/usr/bin/python3 /opt/modbus-gateway/bin/local-kiosk.py --app-config /opt/modbus-gateway/config/runtime/apps/%i.json', 'none', 256, 100),
    'gateway-services': ('bin/gateway-services.sh', '/opt/modbus-gateway/bin/gateway-services.sh apply', 'none', 32, 110),
    'gateway-health-watchdog': ('bin/gateway-health-watchdog.sh', '/opt/modbus-gateway/bin/gateway-health-watchdog.sh run', 'none', 32, 120),
}
DEVICE_FAMILIES = {'modbus_rtu': 'modbus-rtu', 'modbus_tcp': 'modbus-rtu',
                   'dlt645_2007': 'dlt645-driver', 'dlt645': 'dlt645-driver',
                   'local_dio': 'dio-driver', 'dio': 'dio-driver', 'dido': 'dio-driver',
                   'can_socketcan': 'can-driver', 'can': 'can-driver',
                   'iec104': 'iec-driver', 'iec101': 'iec-driver',
                   'iec103': 'iec-driver', 'iec103_tcp': 'iec-driver',
                   'iec103_serial': 'iec-driver',
                   'ems_virtual': None, 'agc_avc_virtual': None}


class RolePolicyError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise RolePolicyError(message)


def _pairs(items):
    value = {}
    for key, item in items:
        require(key not in value, 'duplicate JSON field: ' + key)
        value[key] = item
    return value


def _regular(root, path):
    require(root == path or root in path.parents, 'path escapes policy root')
    for part in (path,) + tuple(path.parents):
        if part == root.parent:
            break
        require(not part.is_symlink(), 'symlink in policy input: ' + str(part))
    require(path.is_file() and stat.S_ISREG(path.stat().st_mode) and path.stat().st_nlink == 1,
            'nonregular policy input: ' + str(path))
    require(path.stat().st_size <= 2 * 1024 * 1024, 'policy input too large')
    return path.read_bytes()


def _json(root, relative):
    data = _regular(root, root / relative)
    try:
        return json.loads(data.decode('utf-8'), object_pairs_hook=_pairs)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise RolePolicyError('invalid JSON: ' + relative) from error


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def _name(value):
    require(isinstance(value, str) and NAME.fullmatch(value), 'unsafe instance name')
    return value


def _section(value, key):
    section = value.get(key, {})
    require(isinstance(section, dict), 'invalid section: ' + key)
    enabled = section.get('enabled', False)
    require(type(enabled) is bool, 'nonboolean enabled: ' + key)
    return section, enabled


def _shm_names(value):
    names = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key in SHM_SINGLE:
                require(isinstance(item, str), 'invalid SHM name')
                if item:
                    require(SHM.fullmatch(item), 'unsafe SHM name')
                    names.add(item)
            elif key in SHM_MULTI:
                require(isinstance(item, list), 'invalid SHM names')
                require(all(isinstance(name, str) for name in item), 'invalid SHM name')
                require(len(set(item)) == len(item), 'duplicate SHM name')
                for name in item:
                    require(isinstance(name, str) and SHM.fullmatch(name), 'unsafe SHM name')
                    names.add(name)
            elif key.lower().endswith(('sharedmemoryname', 'sharedmemorynames')):
                raise RolePolicyError('unknown SHM key: ' + key)
            else:
                names.update(_shm_names(item))
    elif isinstance(value, list):
        for item in value:
            names.update(_shm_names(item))
    return names


def _shm_single(section, key, default=''):
    value = section.get(key, default)
    require(isinstance(value, str), 'invalid SHM name')
    require(not value or SHM.fullmatch(value), 'unsafe SHM name')
    return {value} if value else set()


def _shm_list(section, key):
    value = section.get(key, [])
    require(isinstance(value, list) and all(isinstance(v, str) and SHM.fullmatch(v)
            for v in value) and len(value) == len(set(value)), 'invalid SHM names')
    return set(value)


def _driver_stores(section):
    # ConfigLoader fills the plural list from the singular default only when
    # the plural list is empty. The mains do not all read the singular field.
    plural = _shm_list(section, 'sharedMemoryNames')
    singular = _shm_single(section, 'sharedMemoryName', 'gateway_point_store')
    return plural or singular, singular


def _ref(value, directory):
    require(isinstance(value, str), 'configuration reference must be a string')
    prefix = str(HOME / 'config/runtime' / directory) + '/'
    require(value.startswith(prefix), 'configuration reference outside approved directory')
    stem = value[len(prefix):]
    require(stem.endswith('.json') and _name(stem[:-5]) == stem[:-5], 'unsafe configuration reference')
    return directory + '/' + stem


def _config_references(value, pins, app_paths):
    if isinstance(value, dict):
        for key, item in value.items():
            if key == 'identityConfigFile':
                require(item == str(HOME / 'config/runtime/device_identity.json'),
                        'nested identity reference changed')
            elif key in ('appConfigFile', 'otaAppConfigFile'):
                require(_ref(item, 'apps') in app_paths, 'unselected app reference')
            elif key == 'graphFile' and item:
                require(isinstance(item, str) and
                        re.fullmatch(r'runtime/logic/[A-Za-z0-9_-]+\.json', item) and
                        item[len('runtime/'):] in pins, 'graph reference outside pinned logic')
            elif key in ('caFile', 'certFile', 'keyFile') and item:
                require(isinstance(item, str) and item.startswith(str(HOME / 'config/runtime/tls') + '/') and
                        'tls/' + item.rsplit('/', 1)[-1] in pins and
                        re.fullmatch(r'[A-Za-z0-9_.-]+', item.rsplit('/', 1)[-1]),
                        'TLS reference outside pinned trust files')
            else:
                _config_references(item, pins, app_paths)
    elif isinstance(value, list):
        for item in value:
            _config_references(item, pins, app_paths)


def _template(template_root, family):
    name = family + ('@.service' if family not in ('qt-display-bridge', 'ky-ems',
            'gateway-services', 'gateway-health-watchdog') else '.service')
    data = _regular(template_root, template_root / name)
    parser = configparser.RawConfigParser(strict=False, interpolation=None)
    parser.optionxform = str
    try:
        parser.read_string(data.decode('utf-8'))
        command = parser.get('Service', 'ExecStart')
    except (UnicodeError, configparser.Error) as error:
        raise RolePolicyError('invalid unit template: ' + name) from error
    require(command == CATALOG[family][1], 'unit ExecStart differs from catalog: ' + name)
    requires = set(parser.get('Unit', 'Requires', fallback='').split())
    expected = {'ky-ems': {'qt-display-bridge.service'},
                'local-kiosk': {'local-display@%i.service'}}.get(family, set())
    require(requires == expected, 'unit Requires differs from catalog: ' + name)
    return _hash(data)


def _files(root):
    pins = {}
    require(root.is_dir() and not root.is_symlink(), 'invalid configuration root')
    for path in root.rglob('*'):
        require(not path.is_symlink(), 'symlink in configuration inventory')
        if path.is_dir():
            continue
        relative = path.relative_to(root).as_posix()
        require(relative.split('/')[0] in ('apps', 'devices', 'logic', 'tls', 'network') or
                relative in ('device_identity.json', 'qt-display.env'),
                'unknown configuration location: ' + relative)
        pins[relative] = _hash(_regular(root, path))
    return dict(sorted(pins.items()))


def _protected(config_root, pins, authority, trust_file):
    keys = {'machineCode', 'identitySha256', 'networkSha256', 'tlsSha256',
            'otaPublicKeySha256', 'monitorShmName'}
    require(isinstance(authority, dict) and set(authority) == keys, 'protected authority schema invalid')
    require(isinstance(authority['machineCode'], str) and authority['machineCode'], 'machine identity missing')
    for key in ('identitySha256', 'otaPublicKeySha256'):
        require(isinstance(authority[key], str) and SHA.fullmatch(authority[key]), 'invalid protected pin: ' + key)
    for key, prefix in (('networkSha256', 'network/'), ('tlsSha256', 'tls/')):
        expected = authority[key]
        require(isinstance(expected, dict) and all(isinstance(k, str) and k.startswith(prefix) and
                isinstance(v, str) and SHA.fullmatch(v) for k, v in expected.items()), 'invalid protected inventory')
        require({k: v for k, v in pins.items() if k.startswith(prefix)} == expected,
                'protected network/trust configuration changed')
    identity = _json(config_root, 'device_identity.json')
    require(pins['device_identity.json'] == authority['identitySha256'] and
            identity.get('machineCode') == authority['machineCode'], 'protected identity changed')
    monitor = authority['monitorShmName']
    require(isinstance(monitor, str) and SHM.fullmatch(monitor), 'invalid Monitor SHM binding')
    require(trust_file.is_absolute() and all(not part.is_symlink() for part in (trust_file,) + tuple(trust_file.parents)) and
            trust_file.is_file() and
            stat.S_ISREG(trust_file.stat().st_mode) and trust_file.stat().st_nlink == 1 and
            trust_file.stat().st_size <= 1024 * 1024, 'untrusted OTA public key')
    require(_hash(trust_file.read_bytes()) == authority['otaPublicKeySha256'], 'OTA trust pin changed')


def _device_endpoint(stem, value, protocol, bindings):
    if protocol in ('ems_virtual', 'agc_avc_virtual'):
        return
    require(stem in bindings, 'hardware/simulator binding missing: ' + stem)
    binding = bindings[stem]
    require(isinstance(binding, dict) and set(binding) == {'kind', 'endpoint'}, 'invalid simulator binding')
    kind, endpoint = binding['kind'], binding['endpoint']
    proto = value['protocol']
    if protocol in ('modbus_tcp', 'iec104', 'iec103_tcp'):
        tcp = proto.get('tcp', {})
        require(kind == 'tcp-simulator' and isinstance(tcp, dict) and
                tcp.get('host') in ('127.0.0.1', 'localhost') and
                endpoint == '{}:{}'.format(tcp.get('host'), tcp.get('port')),
                'TCP role must bind the reviewed loopback simulator')
    elif protocol in ('modbus_rtu', 'dlt645_2007', 'dlt645', 'iec101', 'iec103_serial'):
        transport = proto.get('transport', {})
        serial = transport.get('serialPort') if isinstance(transport, dict) else None
        require(kind == 'serial-simulator' and isinstance(serial, str) and
                re.fullmatch(r'/dev/pts/[0-9]+', serial) and endpoint == serial,
                'serial role must bind a reviewed PTY')
    elif protocol in ('can_socketcan', 'can'):
        can = proto.get('can', {})
        iface = can.get('interfaceName') if isinstance(can, dict) else None
        require(kind == 'can-simulator' and isinstance(iface, str) and
                re.fullmatch(r'vcan[0-9]+', iface) and endpoint == iface and
                can.get('manageInterface') is False and
                can.get('transportMode', 'socketcan') == 'socketcan',
                'CAN role must bind an unmanaged vcan socketcan simulator')
    else:
        # The current DIO/sysfs and IEC variants need a reviewed simulator
        # transport adapter before they can be admitted as active services.
        raise RolePolicyError('role has no qualified simulator transport: ' + protocol)


def _camera_receiver(camera, section, bindings):
    code = _name(camera.get('cameraCode'))
    stream = camera.get('stream', {})
    media = section.get('media', {})
    require(isinstance(stream, dict) and isinstance(media, dict), 'camera output invalid')
    auth = media.get('auth', {})
    require(isinstance(auth, dict) and auth.get('enabled', False) is False,
            'camera output authentication is not qualified')
    publish = stream.get('publishUrl', '')
    if not publish:
        base, suffix = media.get('serverUrl', ''), stream.get('path', '')
        require(isinstance(base, str) and isinstance(suffix, str) and base and suffix and
                suffix.startswith('/') and not suffix.startswith('//'), 'camera output missing')
        publish = base.rstrip('/') + suffix
    require(isinstance(publish, str), 'camera output invalid')
    parsed = urlparse(publish)
    try:
        port = parsed.port
    except ValueError as error:
        raise RolePolicyError('camera output invalid') from error
    require(parsed.scheme == 'rtsp' and parsed.hostname == '127.0.0.1' and port and
            not parsed.username and not parsed.password and not parsed.query and not parsed.fragment and
            re.fullmatch(r'/[A-Za-z0-9_/-]+', parsed.path) and '..' not in parsed.path.split('/'),
            'camera output must be an exact loopback RTSP receiver')
    binding = bindings.get('camera:' + code)
    require(isinstance(binding, dict) and binding == {'kind': 'rtsp-receiver', 'endpoint': publish},
            'camera output receiver binding missing')
    return 'camera:' + code


def _tunnel_for_broker(config_root, pins, broker):
    require(isinstance(broker, str), 'broker endpoint invalid')
    if not broker:
        return None
    parsed = urlparse(broker)
    try:
        host, port = parsed.hostname, parsed.port
    except ValueError as error:
        raise RolePolicyError('broker endpoint invalid') from error
    require(parsed.scheme in ('tcp', 'mqtt', 'mqtts') and host and port and
            not parsed.username and not parsed.password and not parsed.path,
            'broker endpoint invalid')
    if host not in ('127.0.0.1', 'localhost'):
        return None
    matches = []
    for relative in pins:
        if not re.fullmatch(r'tls/[A-Za-z0-9_-]+-stunnel\.conf', relative):
            continue
        data = _regular(config_root, config_root / relative)
        parser = configparser.RawConfigParser(strict=True, interpolation=None)
        try:
            parser.read_string('[global]\n' + data.decode('utf-8'))
            accept = parser.get('mqtts', 'accept', fallback='')
        except (UnicodeError, configparser.Error) as error:
            raise RolePolicyError('invalid protected stunnel configuration') from error
        if accept in ('127.0.0.1:' + str(port), 'localhost:' + str(port)):
            matches.append(relative)
    require(len(matches) <= 1, 'ambiguous matching stunnel configuration')
    return matches[0] if matches else None


def _storage(mqtt):
    event = mqtt.get('eventStore', {})
    require(isinstance(event, dict) and event.get('backend', 'legacy') == 'legacy',
            'unqualified EventStore backend')
    alarm, enabled = _section(mqtt, 'alarmStore')
    sqlite_path = alarm.get('sqlitePath', '') if enabled else ''
    if enabled:
        prefix = str(HOME / 'data') + '/'
        require(isinstance(sqlite_path, str) and sqlite_path.startswith(prefix) and
                all(part not in ('', '.', '..') for part in sqlite_path[len(prefix):].split('/')),
                'alarm SQLite path outside gateway data')
    return {'backend': 'legacy-sqlite', 'alarmSqlitePath': sqlite_path,
            'eventStoreServiceAdmitted': False}


def resolve(config_root, template_root, authority, trust_file, *, app_names=None,
            simulator_bindings=None, capabilities=(), watchdog=False):
    """Return deterministic v2 policy data; never start or mutate a service."""
    config_root, template_root, trust_file = Path(config_root), Path(template_root), Path(trust_file)
    names = dict(DEFAULT_APPS if app_names is None else app_names)
    require(set(names) == set(DEFAULT_APPS), 'app entrypoint set invalid')
    require(len(set(names.values())) == len(names), 'duplicate app entrypoint')
    for name in names.values():
        _name(name)
    bindings = {} if simulator_bindings is None else simulator_bindings
    require(isinstance(bindings, dict), 'simulator bindings invalid')
    allowed_caps = {'control-simulator', 'display-simulator', 'camera-simulator'}
    capabilities = tuple(capabilities)
    caps = set(capabilities)
    require(caps <= allowed_caps and len(caps) == len(capabilities), 'unknown/duplicate capability')
    require(type(watchdog) is bool, 'watchdog flag invalid')
    pins = _files(config_root)
    _protected(config_root, pins, authority, trust_file)
    apps = {}
    for key, stem in names.items():
        relative = 'apps/' + stem + '.json'
        if relative not in pins:
            require(key in ('camera', 'agc'), 'required app missing: ' + relative)
            continue
        value = _json(config_root, relative)
        require(isinstance(value, dict) and set(value) <= APP_KEYS, 'unknown app field: ' + relative)
        require(value.get('identityConfigFile') == str(HOME / 'config/runtime/device_identity.json'),
                'app identity path changed')
        mqtt_config = value.get('mqtt', {})
        require(isinstance(mqtt_config, dict) and
                mqtt_config.get('clientId', authority['machineCode']) == authority['machineCode'],
                'app MQTT identity changed')
        _shm_names(value)
        apps[key] = (relative, value)
    require({p for p in pins if p.startswith('apps/') and p.endswith('.json')} ==
            {v[0] for v in apps.values()}, 'unselected app configuration')
    for _, value in apps.values():
        _config_references(value, pins, {v[0] for v in apps.values()})
    device_refs = set()
    app_device_refs = {}
    for key, (_, value) in apps.items():
        refs = value.get('deviceConfigFiles', [])
        require(isinstance(refs, list) and all(isinstance(ref, str) for ref in refs) and
                len(refs) == len(set(refs)), 'duplicate/malformed device references')
        app_device_refs[key] = {_ref(v, 'devices') for v in refs}
        device_refs.update(app_device_refs[key])

    def role_configs(key):
        return [apps[key][0]] + sorted(app_device_refs[key])
    roles = {}
    data_inputs = []

    def add(family, instance, configs, shm_names=(), deps=(), optional_shm=()):
        _name(instance) if instance is not None else None
        unit = family + ('@' + instance if instance is not None else '') + '.service'
        require(unit not in roles, 'duplicate service role: ' + unit)
        executable, command, policy, reserve, phase = CATALOG[family]
        require(family not in ('dio-driver', 'can-driver', 'qt-display-bridge') or
                names['mqtt'] == 'mqtt-service', 'unit template binds canonical MQTT app')
        mapped = sorted(set(shm_names))
        optional = sorted(set(optional_shm) - set(mapped))
        if policy == 'optional':
            optional = sorted(set(mapped) | set(optional))
            mapped = []
        require(policy != 'required' or mapped, 'required SHM role lacks exact names: ' + unit)
        require(policy != 'none' or not mapped and not optional, 'non-SHM role has a mapping: ' + unit)
        if family not in ('mqtt-tls-tunnel', 'gateway-services', 'gateway-health-watchdog'):
            configs = list(configs) + ['device_identity.json']
        for relative in configs:
            require(relative in pins, 'role config missing: ' + relative)
        roles[unit] = {'unit': unit, 'family': family, 'kind': 'service' if family not in
                       ('gateway-services', 'gateway-health-watchdog') else 'supervisor',
                       'executable': executable, 'execStart': command.replace('%i', instance or ''),
                       'runtimeFiles': sorted(([executable] if not executable.startswith('/') else []) +
                                              list(EXTRA_RUNTIME_FILES.get(family, ()))),
                       'templateSha256': _template(template_root, family),
                       'configPaths': sorted(set(configs)), 'dependencies': sorted(set(deps)),
                       'shm': {'policy': policy, 'names': sorted(mapped + optional),
                               'required': mapped, 'optional': optional}, 'startPhase': phase,
                       'reserveMiB': reserve}
        if family == 'camera-service':
            roles[unit]['externalExecutables'] = ['/usr/bin/ffmpeg', '/bin/sh']

    devices = {}
    for relative in sorted(device_refs):
        require(relative in pins, 'referenced device missing: ' + relative)
        value = _json(config_root, relative)
        require(isinstance(value, dict), 'device must be object')
        protocol_data = value.get('protocol')
        require(isinstance(protocol_data, dict) and isinstance(protocol_data.get('type'), str),
                'device protocol missing')
        protocol = protocol_data['type']
        require(protocol == protocol.lower(), 'noncanonical device protocol')
        require(protocol in DEVICE_FAMILIES, 'unknown device protocol: ' + protocol)
        enabled = value.get('enabled', True)
        require(type(enabled) is bool, 'nonboolean device enabled')
        shm_names = _shm_names(value.get('memoryStore', {}))
        stem = relative[8:-5]
        devices[stem] = (protocol, enabled, shm_names)
        if enabled:
            family = DEVICE_FAMILIES[protocol]
            if family is None:
                require(len(shm_names) == 1, 'virtual device requires one exact SHM')
                data_inputs.append({'path': relative, 'protocol': protocol,
                                    'shmNames': sorted(shm_names)})
            else:
                _device_endpoint(stem, value, protocol, bindings)
                require(len(shm_names) == 1, 'device requires one exact SHM')
                configs = [relative]
                if family in ('dio-driver', 'can-driver'):
                    configs.append(apps['mqtt'][0])
                add(family, stem, configs, shm_names)
    required_bindings = {name for name, (protocol, enabled, _) in devices.items()
                         if enabled and DEVICE_FAMILIES[protocol] is not None}
    for relative in pins:
        if relative.startswith('devices/') and relative.endswith('.json') and relative not in device_refs:
            value = _json(config_root, relative)
            protocol = value.get('protocol', {}) if isinstance(value, dict) else {}
            require(isinstance(protocol, dict) and protocol.get('type') in DEVICE_FAMILIES,
                    'unknown unreferenced device protocol')
            _shm_names(value)
            require(value.get('enabled') is False,
                    'enabled unreferenced device: ' + relative)

    def device_shm(app_key):
        return set().union(*(devices[relative[8:-5]][2] for relative in app_device_refs[app_key]))

    mqtt_path, mqtt = apps['mqtt']
    monitor_path, monitor = apps['monitor']
    storage = _storage(mqtt)
    mqtt_settings, mqtt_enabled = _section(mqtt, 'mqtt')
    _, driver_enabled = _section(mqtt, 'mqttDriver')
    event, event_enabled = _section(mqtt, 'eventEngine')
    compute, compute_enabled = _section(mqtt, 'computeEngine')
    forward, forward_enabled = _section(mqtt, 'mqttForward')
    cluster, cluster_enabled = _section(mqtt, 'emsCluster')
    ota, ota_enabled = _section(mqtt, 'ota')
    realtime, realtime_enabled = _section(mqtt, 'realtime')
    require(mqtt_enabled or not (driver_enabled or event_enabled or forward_enabled or ota_enabled or realtime_enabled),
            'MQTT-dependent role enabled without MQTT')
    mode = mqtt.get('runtimeMode')
    require(mode in ('ems', 'agc_avc', 'standard'), 'unknown MQTT runtime mode')
    require(not cluster_enabled or mode == 'ems' and 'control-simulator' in caps,
            'EMS cluster requires reviewed simulated control')
    driver_section = mqtt.get('mqttDriver', {})
    mqtt_primary_names, mqtt_singular_name = _driver_stores(driver_section)
    mqtt_base_names = mqtt_primary_names | device_shm('mqtt')
    agc_device_names = device_shm('agc') if 'agc' in apps else set()
    camera_section, camera_enabled = _section(mqtt, 'cameraService')
    camera_names = _shm_single(camera_section, 'sharedMemoryName', 'gateway_point_store')
    driver_names = mqtt_base_names | agc_device_names
    if camera_enabled:
        driver_names.update(camera_names)
    if cluster_enabled:
        driver_names.update(_shm_single(cluster, 'virtualSharedMemoryName', 'ems_cluster_store'))
    tunnel_by_app = {}
    for app_key, (relative, value) in apps.items():
        settings, enabled = _section(value, 'mqtt')
        if not enabled:
            continue
        tunnel_path = _tunnel_for_broker(config_root, pins, settings.get('broker', ''))
        if tunnel_path:
            stem = tunnel_path[len('tls/'):-len('.conf')]
            unit = 'mqtt-tls-tunnel@' + stem + '.service'
            if unit not in roles:
                add('mqtt-tls-tunnel', stem, [tunnel_path])
            tunnel_by_app[app_key] = unit
    if compute_enabled:
        output_names = set()
        rules = compute.get('rules', [])
        require(isinstance(rules, list), 'invalid compute rules')
        for rule in rules:
            require(isinstance(rule, dict) and isinstance(rule.get('outputs', []), list),
                    'invalid compute outputs')
            for output in rule.get('outputs', []):
                require(isinstance(output, dict), 'invalid compute output')
                output_names.update(_shm_single(output, 'sharedMemoryName'))
        compute_names = (_shm_list(compute, 'sharedMemoryNames') | mqtt_primary_names |
                         mqtt_singular_name | device_shm('mqtt') | camera_names | output_names |
                         _shm_single(compute, 'outputDefaultSharedMemoryName',
                                     'gateway_point_store_compute'))
        if cluster_enabled:
            compute_names.update(_shm_single(cluster, 'virtualSharedMemoryName', 'ems_cluster_store'))
        add('compute-engine', names['mqtt'], role_configs('mqtt'),
            compute_names)
    if event_enabled:
        add('event-engine', names['mqtt'], role_configs('mqtt'), mqtt_base_names | camera_names)
    driver_needed = mqtt_enabled and (driver_enabled or ota_enabled or realtime_enabled or
            event_enabled and event.get('publishMode') == 'mqtt_driver_outbox' or
            any(mqtt_settings.get(k) for k in ('commandRequestTopic', 'otaRequestTopic', 'realtimeRequestTopic')))
    require(not event_enabled or event.get('publishMode') != 'mqtt_driver_outbox' or driver_needed,
            'event outbox requires MQTT driver')
    if driver_needed:
        configs = role_configs('mqtt') + (role_configs('agc') if 'agc' in apps else [])
        add('mqtt-driver', names['mqtt'], configs, driver_names,
            [tunnel_by_app['mqtt']] if 'mqtt' in tunnel_by_app else [])
    worker = mqtt.get('mqttDriver', {}).get('fullUploadWorker', {})
    require(isinstance(worker, dict), 'invalid full upload worker')
    worker_mode = worker.get('mode', 'inline')
    require(worker_mode in ('inline', 'isolated'), 'unknown full upload worker mode')
    if forward_enabled or driver_needed and worker_mode == 'isolated':
        configs = role_configs('mqtt') + (role_configs('agc') if 'agc' in apps else [])
        add('mqtt-forwarder', names['mqtt'], configs, driver_names,
            deps=(('mqtt-driver@' + names['mqtt'] + '.service',)
            if driver_needed and worker_mode == 'isolated' else ()))
    if cluster_enabled:
        add('ems-cluster', names['mqtt'], role_configs('mqtt'),
            _shm_single(cluster, 'virtualSharedMemoryName', 'ems_cluster_store'))

    monitor_section, monitor_enabled = _section(monitor, 'systemMonitor')
    cellular = monitor_section.get('cellular', {})
    require(isinstance(cellular, dict), 'invalid cellular section')
    route = cellular.get('routeFailover', {})
    require(isinstance(route, dict) and route.get('enabled', False) is False,
            'network route/failover role is protected')
    if monitor_enabled:
        # system_monitor_runtime_discovery.cpp reads every sibling app JSON,
        # including defaults, before system_monitor_main.cpp opens stores.
        monitor_optional = set()
        monitor_configs = []
        for app_key, (_, app) in apps.items():
            monitor_configs.extend(role_configs(app_key))
            monitor_optional.update(device_shm(app_key))
            app_driver, _ = _driver_stores(app.get('mqttDriver', {}))
            _, app_driver_enabled = _section(app, 'mqttDriver')
            if app_key == 'monitor' or app_driver_enabled:
                monitor_optional.update(app_driver)
            app_compute, app_compute_enabled = _section(app, 'computeEngine')
            if app_compute_enabled:
                monitor_optional.update(_shm_list(app_compute, 'sharedMemoryNames'))
                monitor_optional.update(_shm_single(app_compute, 'outputDefaultSharedMemoryName',
                                                   'gateway_point_store_compute'))
            app_display, app_display_enabled = _section(app, 'localDisplay')
            if app_display_enabled:
                monitor_optional.update(_shm_list(app_display, 'sharedMemoryNames'))
            app_agc, app_agc_enabled = _section(app, 'agcAvc')
            if app_agc_enabled:
                monitor_optional.update(_shm_list(app_agc, 'sharedMemoryNames'))
                monitor_optional.update(_shm_single(app_agc, 'outputSharedMemoryName',
                                                   'gateway_point_store_agc_avc'))
            app_camera, app_camera_enabled = _section(app, 'cameraService')
            if app_camera_enabled:
                monitor_optional.update(_shm_single(app_camera, 'sharedMemoryName',
                                                   'gateway_point_store'))
        add('system-monitor', names['monitor'], monitor_configs, [authority['monitorShmName']],
            [tunnel_by_app['monitor']] if 'monitor' in tunnel_by_app else [],
            optional_shm=monitor_optional)
    display, display_enabled = _section(monitor, 'localDisplay')
    if display_enabled:
        require('display-simulator' in caps, 'display requires reviewed simulated output')
        renderer = display.get('renderer') or display.get('mode') or 'webkit'
        if renderer in ('nativeqt', 'qt', 'ky-ems', 'kyems'):
            add('qt-display-bridge', None, role_configs('mqtt'),
                mqtt_base_names | camera_names | _shm_names(mqtt.get('localDisplay', {})))
            add('ky-ems', None, role_configs('monitor'), _shm_names(display), ['qt-display-bridge.service'])
        elif renderer == 'webkit':
            add('local-display', names['monitor'], role_configs('monitor'),
                _shm_names(display) | _shm_names(monitor.get('mqttDriver', {})) | device_shm('monitor'))
            kiosk = display.get('kiosk', {})
            require(isinstance(kiosk, dict), 'invalid kiosk section')
            if kiosk.get('enabled', True):
                add('local-kiosk', names['monitor'], role_configs('monitor'),
                    deps=['local-display@' + names['monitor'] + '.service'])
        else:
            raise RolePolicyError('unknown display renderer')
    if 'agc' in apps:
        agc_path, agc = apps['agc']
        section, enabled = _section(agc, 'agcAvc')
        if enabled:
            require(agc.get('runtimeMode') == 'agc_avc' and 'control-simulator' in caps,
                    'AGC/AVC requires reviewed simulated control')
            require(section.get('shadowMode') is True and section.get('submitWrites') is False,
                    'AGC/AVC physical output must remain disabled')
            add('agc-avc', names['agc'], role_configs('agc'), _shm_names(section))
    if 'camera' in apps:
        camera_path, camera = apps['camera']
        section, enabled = _section(camera, 'cameraService')
        if enabled:
            cameras = section.get('cameras')
            require(section.get('allowCustomCommand', False) is False,
                    'camera custom command is not qualified')
            require('camera-simulator' in caps and isinstance(cameras, list) and
                    all(isinstance(item, dict) and type(item.get('enabled', True)) is bool
                        for item in cameras) and
                    any(item.get('enabled', True) for item in cameras) and
                    all(not item.get('enabled', True) or
                        item.get('sourceType') == 'test' and not item.get('source') and
                        isinstance(item.get('sourceAuth', {}), dict) and
                        item.get('sourceAuth', {}).get('enabled', False) is False
                        for item in cameras),
                    'camera requires reviewed simulator inputs')
            for item in cameras:
                if item.get('enabled', True):
                    require(not item.get('command'), 'camera custom command is not qualified')
                    required_bindings.add(_camera_receiver(item, section, bindings))
            camera_shm = _shm_single(section, 'sharedMemoryName', 'gateway_point_store')
            if not camera_shm:
                camera_shm = {'gateway_point_store'}
            add('camera-service', names['camera'], role_configs('camera'), camera_shm)

    require(set(bindings) == required_bindings, 'unused or missing simulator binding')

    add('gateway-services', None, [])
    if watchdog:
        add('gateway-health-watchdog', None, [], deps=['gateway-services.service'])
    for role in roles.values():
        require(all(dep in roles for dep in role['dependencies']), 'role dependency missing: ' + role['unit'])
        require(all(roles[dep]['startPhase'] < role['startPhase'] for dep in role['dependencies']),
                'role dependency order invalid')
    ordered = sorted(roles.values(), key=lambda item: (item['startPhase'], item['unit']))
    return {'format': FORMAT, 'catalogVersion': 1, 'configSha256': pins,
            'protected': authority, 'roles': ordered, 'dataInputs': data_inputs,
            'tools': dict(TOOLS), 'storage': storage,
            'resourcePolicy': {'unit': 'MiB', 'systemReserveMiB': 64,
                    'basis': 'admission floors only; not measured RSS or cgroup limits'}}
