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
CORE_PROFILE = 'CORE_READONLY_TEST'
CORE_READERS = {'RTU_MQTT_MONITOR': [A_RTU_UNIT, A_MQTT_UNIT, MONITOR_UNIT],
                'MONITOR_ONLY': [MONITOR_UNIT]}
CORE_NODES = {'RTU_MQTT_MONITOR': 'COMM202600999', 'MONITOR_ONLY': 'COMM202600998'}
CORE_PROGRAMS = frozenset(('ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver '
                          'MqttForwarder EventEngine ComputeEngine EmsClusterCoordinator '
                          'AgcAvcController EmsParityCheck SystemMonitor LocalDisplay '
                          'QtDisplayBridge CameraService pointctl stress_runner memory_point_store_migrate').split())
CORE_SCRIPTS = ('runtime-upgrade-guard.py', 'gateway-services.sh', 'ota-apply.sh',
                'ota-rollback.sh', 'ota-health-result.py', 'production-smoke-test.sh')
POINT_STORE_SIZES = {10: 15174272, 11: 15667032}
PRODUCTION_DIR = 'runtime-production'
PRODUCTION_UNIT = re.compile(r'(?:(?:modbus-rtu|dlt645-driver|dio-driver|can-driver|iec-driver|compute-engine|agc-avc|ems-cluster|event-engine|local-display|local-display-qt|local-kiosk|system-monitor|camera-service|mqtt-tls-tunnel|mqtt-driver|mqtt-forwarder)@[A-Za-z0-9_.:-]*|gateway-services|gateway-health-watchdog|qt-display-bridge|ky-ems)\.service\Z')
PRODUCTION_READERS = {A_RTU_UNIT: 'bin/ModbusRtu', A_MQTT_UNIT: 'bin/MqttDriver',
                      MONITOR_UNIT: 'bin/SystemMonitor'}
PRODUCTION_HELPERS = frozenset(('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py',
                                'gateway-services.sh', 'ota-health-result.py', 'ota-rollback.sh',
                                'factory-lifecycle-stage.py', 'production_role_policy.py'))
SYSTEMD_ROOTS = ('/etc/systemd/system', '/run/systemd/system', '/usr/local/lib/systemd/system',
                '/usr/lib/systemd/system', '/lib/systemd/system', '/etc/systemd/system.control',
                '/run/systemd/system.control', '/run/systemd/transient', '/run/systemd/generator',
                '/run/systemd/generator.early', '/run/systemd/generator.late')
PRODUCTION_RECOVERY_UNIT = 'gateway-ota-recover.service'


def production_json(path):
    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate production JSON field: ' + key)
            result[key] = value
        return result
    source = exact_file(path)
    if source.stat().st_uid != 0 or source.stat().st_mode & 0o022 or source.stat().st_size > 1024 * 1024:
        raise ValueError('untrusted production record: ' + str(path))
    return json.loads(source.read_text(), object_pairs_hook=unique)


def production_pin(path):
    source = exact_file(path)
    value = source.stat()
    if value.st_uid != 0 or value.st_mode & 0o022:
        raise ValueError('untrusted production file: ' + str(path))
    return dict(sha256=sha(source), mode=stat.S_IMODE(value.st_mode), bytes=value.st_size)


def production_files(home):
    result = {}
    for directory in (home / 'bin', home / 'ky-ems'):
        if directory.is_symlink():
            raise ValueError('symlink production runtime directory')
        if not directory.exists():
            continue
        for source in directory.rglob('*'):
            if source.is_symlink() or not source.is_dir() and not source.is_file():
                raise ValueError('nonregular production runtime file')
            if source.is_file():
                result[str(source.relative_to(home))] = production_pin(source)
    return result


def production_unit_files(units):
    names = set(units)
    names.update(u.split('@')[0] + '@.service' for u in units if '@' in u)
    directories = {'service.d'} | {u + '.d' for u in names}
    for unit in names:
        prefix = unit.split('@')[0]
        directories.update(prefix[:i + 1] + '.service.d' for i, c in enumerate(prefix) if c == '-')
    found = {}
    for root in SYSTEMD_ROOTS:
        base = Path(root)
        # Ubuntu's /lib -> /usr/lib alias is not an extra inventory. Resolve
        # ONLY this standard alias; unit/drop-in symlinks remain rejected.
        if root == '/lib/systemd/system' and base.resolve() == Path('/usr/lib/systemd/system').resolve():
            continue
        if not base.exists():
            continue
        for path in base.iterdir():
            if path.name == PRODUCTION_RECOVERY_UNIT:
                found[str(path)] = production_pin(path)
            elif PRODUCTION_UNIT.fullmatch(path.name):
                if path.name not in names:
                    raise ValueError('production unit inventory incomplete: ' + path.name)
                found[str(path)] = production_pin(path)
            elif path.name == PRODUCTION_RECOVERY_UNIT + '.d':
                raise ValueError('production recovery owner override refused')
            elif path.name in directories or (path.name.endswith('.service.d') and
                    PRODUCTION_UNIT.fullmatch(path.name[:-2])):
                if path.name not in directories or path.is_symlink() or not path.is_dir():
                    raise ValueError('unknown production override directory')
                for child in path.iterdir():
                    if not child.name.endswith('.conf'):
                        raise ValueError('uncovered production override file')
                    found[str(child)] = production_pin(child)
    return found


def production_units_present(units):
    for action in ('list-units', 'list-unit-files'):
        result = subprocess.run(['systemctl', action, '--all', '--no-legend', '--no-pager', '--plain'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=45)
        if result.returncode:
            raise ValueError('cannot inventory production participants')
        for line in result.stdout.splitlines():
            fields = line.split()
            if fields and PRODUCTION_UNIT.fullmatch(fields[0]) and fields[0] not in units:
                raise ValueError('uncovered production participant: ' + fields[0])


def _production_schema(baseline, home):
    required = {'format', 'baselineId', 'generation', 'parentSha256', 'gatewayHome', 'machineCode',
                'architecture', 'pointStoreAbi', 'pointStoreSize', 'clusterProtocol', 'controlEnabled',
                'files', 'configSha256', 'units', 'allowedUnits', 'unitFiles', 'gates', 'segments',
                'retiredSources', 'historical', 'bootId', 'release', 'trust', 'healthPolicy'}
    version = baseline.get('format') if isinstance(baseline, dict) else None
    if version == 'runtime-baseline-v2':
        required |= {'roleAuthority'}
    if (not isinstance(baseline, dict) or set(baseline) != required or
            version not in ('runtime-baseline-v1', 'runtime-baseline-v2') or
            baseline['gatewayHome'] != str(home) or home != Path('/opt/modbus-gateway') or
            baseline['architecture'] != 'aarch64' or baseline['pointStoreAbi'] != 11 or
            baseline['pointStoreSize'] != POINT_STORE_SIZES[11] or baseline['clusterProtocol'] != 2 or
            baseline['controlEnabled'] is not False or type(baseline['generation']) is not int or
            baseline['generation'] < 1 or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', baseline['baselineId']) or
            baseline['parentSha256'] is not None and not re.fullmatch('[0-9a-f]{64}', baseline['parentSha256'])):
        raise ValueError('invalid production baseline schema/ABI')
    units, allowed = baseline['units'], baseline['allowedUnits']
    if (not isinstance(units, list) or len(set(units)) != len(units) or
            not all(PRODUCTION_UNIT.fullmatch(u) for u in units) or
            not isinstance(allowed, list) or len(set(allowed)) != len(allowed) or
            'gateway-services.service' not in allowed or not set(allowed) <= set(units) or
            version == 'runtime-baseline-v1' and (
                not set(allowed) <= set(PRODUCTION_READERS) |
                    {'gateway-services.service', 'gateway-health-watchdog.service'} or
                not set(allowed) & set(PRODUCTION_READERS))):
        raise ValueError('production reader-only participant scope invalid')
    if version == 'runtime-baseline-v2':
        authority = baseline['roleAuthority']
        if (not isinstance(authority, dict) or set(authority) != {'format', 'profileSha256', 'resolved'} or
                authority['format'] != 'configured-role-authority-v2' or
                not isinstance(authority['profileSha256'], str) or
                not re.fullmatch('[0-9a-f]{64}', authority['profileSha256']) or
                not isinstance(authority['resolved'], dict) or
                authority['resolved'].get('format') != 'configured-role-policy-v2' or
                authority['resolved'].get('configSha256') != baseline['configSha256'] or
                not isinstance(authority['resolved'].get('roles'), list) or
                [role.get('unit') for role in authority['resolved']['roles'] if isinstance(role, dict)] != allowed or
                len(authority['resolved']['roles']) != len(allowed)):
            raise ValueError('invalid configured production role authority')
    files = baseline['files']
    if (not isinstance(files, dict) or not {'bin/runtime-upgrade-guard.py', 'bin/gateway-services.sh'} <= set(files) or
            any(not isinstance(path, str) or not re.fullmatch(r'(?:bin|ky-ems)/[A-Za-z0-9_./-]+', path) or
                any(p in ('', '.', '..') for p in path.split('/')) for path in files)):
        raise ValueError('production paired runtime inventory invalid')
    for inventory in (files, baseline['unitFiles']):
        for pin in inventory.values():
            if (not isinstance(pin, dict) or set(pin) != {'sha256', 'mode', 'bytes'} or
                    not isinstance(pin['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', pin['sha256']) or
                    type(pin['mode']) is not int or pin['mode'] & ~0o755 or pin['mode'] & 0o022 or
                    type(pin['bytes']) is not int or not 0 <= pin['bytes'] <= 128 * 1024 * 1024):
                raise ValueError('invalid production file pin')
    if (not isinstance(baseline['configSha256'], dict) or not baseline['configSha256'] or
            any(not isinstance(p, str) or p.startswith('/') or any(v in ('', '.', '..') for v in p.split('/')) or
                not isinstance(pin, str) or not re.fullmatch('[0-9a-f]{64}', pin)
                for p, pin in baseline['configSha256'].items()) or
            not isinstance(baseline['historical'], dict) or not baseline['historical'] or
            not isinstance(baseline['retiredSources'], dict) or
            not isinstance(baseline['release'], str) or not re.fullmatch(r'[A-Za-z0-9._-]{1,128}', baseline['release']) or
            not isinstance(baseline['bootId'], str) or not baseline['bootId'] or
            not isinstance(baseline['machineCode'], str) or not baseline['machineCode']):
        raise ValueError('production config/provenance schema invalid')
    for path, pin in baseline['historical'].items():
        if not isinstance(path, str) or not Path(path).is_absolute() or not isinstance(pin, dict) or set(pin) != {'sha256', 'mode', 'bytes'}:
            raise ValueError('production historical pin invalid')
    for name, meta in baseline['retiredSources'].items():
        if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,127}', name) or
                not isinstance(meta, dict) or set(meta) != {'dev', 'ino'} or
                type(meta['dev']) is not int or type(meta['ino']) is not int or meta['ino'] <= 0):
            raise ValueError('production retired source identity invalid')
    if (set(baseline['gates']) != set(units) or
            len(set(baseline['gates'].values())) != len(units) or
            any(path not in baseline['unitFiles'] or
                not path.startswith(str(Path('/etc/systemd/system') / (unit + '.d')) + '/90-offline-') or
                not path.endswith('.conf') for unit, path in baseline['gates'].items())):
        raise ValueError('production gate ownership incomplete')
    segments = baseline['segments']
    if (not isinstance(segments, list) or not segments or
            any(set(s) != {'name', 'dev', 'ino'} or not re.fullmatch(r'[A-Za-z0-9_-]{1,127}', s['name']) or
                type(s['dev']) is not int or type(s['ino']) is not int or s['ino'] <= 0 for s in segments) or
            len({s['name'] for s in segments}) != len(segments)):
        raise ValueError('production SHM inventory invalid')
    if {s['name'] for s in segments} & set(baseline['retiredSources']):
        raise ValueError('production retired and current SHM roles overlap')
    trust = baseline['trust']
    if (set(trust) != {'requireSignature', 'checksumRequired', 'allowInsecureHttp', 'publicKeySha256', 'httpsOrigins'} or
            trust['requireSignature'] is not True or trust['checksumRequired'] is not True or
            trust['allowInsecureHttp'] is not False or not re.fullmatch('[0-9a-f]{64}', trust['publicKeySha256']) or
            not isinstance(trust['httpsOrigins'], list) or not trust['httpsOrigins'] or
            any(not re.fullmatch(r'https://[A-Za-z0-9.-]+(?::[0-9]+)?', o) for o in trust['httpsOrigins'])):
        raise ValueError('production trust policy invalid')
    policy = baseline['healthPolicy']
    if (not isinstance(policy, dict) or set(policy) != {'stableSeconds', 'url', 'timeoutSeconds'} or
            type(policy['stableSeconds']) is not int or not 2 <= policy['stableSeconds'] <= 120 or
            type(policy['timeoutSeconds']) is not int or not 1 <= policy['timeoutSeconds'] <= 10 or
            not isinstance(policy['url'], str) or
            not re.fullmatch(r'http://127\.0\.0\.1:[0-9]{1,5}/[A-Za-z0-9_/.-]*', policy['url'])):
        raise ValueError('production health endpoint policy invalid')


def production_schema(baseline, home):
    try:
        _production_schema(baseline, home)
    except (KeyError, TypeError, AttributeError) as error:
        raise ValueError('malformed production baseline field/type') from error


def production_roles(home, baseline, config_root=None, helper=None):
    if baseline['format'] != 'runtime-baseline-v2':
        raise ValueError('configured role authority requires a v2 baseline')
    authority = baseline['roleAuthority']
    profile_sha = authority['profileSha256']
    profile_path = home / 'data' / PRODUCTION_DIR / 'profiles' / (profile_sha + '.json')
    if sha(exact_file(profile_path)) != profile_sha:
        raise ValueError('reviewed board profile digest changed')
    profile = production_json(profile_path)
    if (not isinstance(profile, dict) or set(profile) != {'format', 'machineCode', 'ramMiB',
            'capabilities', 'simulatorBindings', 'allowedUnits', 'segmentNames'} or
            profile['format'] != 'board-simulator-profile-v1' or
            profile['machineCode'] != baseline['machineCode'] or
            profile['ramMiB'] not in (2048, 4096) or
            not isinstance(profile['capabilities'], list) or
            not isinstance(profile['simulatorBindings'], dict) or
            profile['allowedUnits'] != baseline['allowedUnits'] or
            profile['segmentNames'] != sorted(s['name'] for s in baseline['segments'])):
        raise ValueError('reviewed board profile disagrees with baseline')
    helper = helper if helper is not None else home / 'bin/production_role_policy.py'
    pin = baseline['files'].get('bin/production_role_policy.py')
    if pin is None or production_pin(helper) != pin or pin['bytes'] > 1024 * 1024:
        raise ValueError('configured role parser is not paired with baseline')
    source = exact_file(helper)
    before = source.stat()
    fd = os.open(str(source), os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        raw = stream.read(pin['bytes'] + 1)
        after = os.fstat(stream.fileno())
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
    if (identity(before) != identity(opened) or identity(opened) != identity(after) or
            identity(after) != identity(source.stat()) or len(raw) != pin['bytes'] or
            hashlib.sha256(raw).hexdigest() != pin['sha256']):
        raise ValueError('configured role parser changed while loading')
    policy = {'__name__': 'production_role_policy', '__file__': str(source)}
    exec(compile(raw, str(source), 'exec'), policy)
    resolved = authority['resolved']
    protected = resolved.get('protected') if isinstance(resolved, dict) else None
    if (not isinstance(protected, dict) or protected.get('machineCode') != baseline['machineCode'] or
            protected.get('identitySha256') != baseline['configSha256'].get('device_identity.json') or
            protected.get('otaPublicKeySha256') != baseline['trust']['publicKeySha256']):
        raise ValueError('configured role protected identity changed')
    actual = policy['resolve'](config_root if config_root is not None else home / 'config/runtime',
                            Path('/etc/systemd/system'), protected, Path('/etc/modbus-gateway/ota.pub'),
                            simulator_bindings=profile['simulatorBindings'],
                            capabilities=profile['capabilities'],
                            watchdog='gateway-health-watchdog.service' in baseline['allowedUnits'])
    if actual != resolved:
        raise ValueError('configured role result differs from approved config/templates')
    for role in actual['roles']:
        unit = role['unit']
        template = unit.split('@')[0] + '@.service' if '@' in unit else unit
        template_path = str(Path('/etc/systemd/system') / template)
        if (role['unit'] not in baseline['units'] or
                any(path not in baseline['files'] for path in role['runtimeFiles']) or
                not set(role['shm']['names']) <= {s['name'] for s in baseline['segments']} or
                baseline['unitFiles'].get(template_path, {}).get('sha256') != role['templateSha256']):
            raise ValueError('configured role runtime/unit/SHM inventory incomplete')
    return actual['roles']


def production_effective(baseline):
    for unit in baseline['units']:
        if unit.endswith('@.service'):
            continue
        fragment = systemctl_value(unit, 'FragmentPath')
        if fragment.startswith('/lib/systemd/system/') and Path('/lib/systemd/system').resolve() == Path('/usr/lib/systemd/system').resolve():
            fragment = str(Path(fragment).resolve())
        if not fragment:
            if (unit in baseline['allowedUnits'] or systemctl_value(unit, 'LoadState') != 'not-found' or
                    systemctl_value(unit, 'ActiveState') != 'inactive' or systemctl_value(unit, 'MainPID') != '0'):
                raise ValueError('production missing participant is not safely absent')
            continue
        if (fragment not in baseline['unitFiles'] or systemctl_value(unit, 'LoadState') != 'loaded' or
                systemctl_value(unit, 'NeedDaemonReload') != 'no'):
            raise ValueError('production effective unit fragment differs from inventory')
        names = {unit, unit.split('@')[0] + '@.service'} if '@' in unit else {unit}
        dirs = {u + '.d' for u in names} | {'service.d'}
        prefix = unit.split('@')[0]
        dirs.update(prefix[:i + 1] + '.service.d' for i, c in enumerate(prefix) if c == '-')
        expected = {p for p in baseline['unitFiles'] if Path(p).parent.name in dirs}
        drops = set()
        for p in systemctl_value(unit, 'DropInPaths').split():
            if p.startswith('/lib/systemd/system/') and Path('/lib/systemd/system').resolve() == Path('/usr/lib/systemd/system').resolve():
                p = str(Path(p).resolve())
            drops.add(p)
        if drops != expected:
            raise ValueError('production effective overrides differ from inventory')


def production_architecture(path):
    with exact_file(path).open('rb') as stream:
        header = stream.read(20)
    if header[:4] == b'\x7fELF' and (len(header) < 20 or header[4:6] != b'\x02\x01' or
            struct.unpack_from('<H', header, 18)[0] != 183):
        raise ValueError('production runtime ELF architecture mismatch')


def production_roles_sha(baseline):
    # Boot-local inode identities are not durable logical roles. Same-ABI installs
    # preserve these fields; the native creation proof can outlive file promotion.
    roles = {k: baseline[k] for k in ('gatewayHome', 'machineCode', 'pointStoreAbi',
             'pointStoreSize', 'configSha256', 'retiredSources')}
    roles['names'] = sorted(s['name'] for s in baseline['segments'])
    if baseline['format'] == 'runtime-baseline-v2':
        roles['roleAuthority'] = baseline['roleAuthority']
        roles['allowedUnits'] = baseline['allowedUnits']
    return hashlib.sha256((json.dumps(roles, sort_keys=True) + '\n').encode()).hexdigest()


def production_boot_lineage(home, baseline, original, proof_path, proof_boot):
    root = home / 'data' / PRODUCTION_DIR
    current = baseline
    seen = set()
    transaction_hint = None
    for depth in range(129):
        if production_roles_sha(current) == production_roles_sha(original):
            return depth
        if len(seen) == 128:
            break
        candidate_id = current['baselineId']
        if (current['format'] != 'runtime-baseline-v2' or
                not isinstance(candidate_id, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', candidate_id)):
            break
        candidate_sha = hashlib.sha256((json.dumps(current, sort_keys=True) + '\n').encode()).hexdigest()
        if candidate_sha in seen:
            break
        seen.add(candidate_sha)
        pending_path = root / 'pending.json'
        pending = production_json(pending_path) if transaction_hint is None and (
            pending_path.exists() or pending_path.is_symlink()) else None
        provisional = False
        if (isinstance(pending, dict) and set(pending) == {'transaction', 'approvalSha256'} and
                isinstance(pending['transaction'], str) and
                re.fullmatch(r'[A-Za-z0-9_-]{1,63}', pending['transaction'])):
            pending_transaction = root / 'transactions' / pending['transaction']
            pending_state = production_json(pending_transaction / 'state.json')
            provisional = (pending_state.get('candidateSha256') == candidate_sha and
                           pending_state.get('phase') in ('INSTALLING', 'STARTING'))
        if provisional:
            transaction_id = pending['transaction']
        elif transaction_hint is not None:
            transaction_id = transaction_hint
        else:
            pointer = production_json(root / 'current.json')
            if pointer.get('sha256') != candidate_sha:
                break
            transaction_id = pointer.get('transaction')
        if not isinstance(transaction_id, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', transaction_id):
            break
        transaction = root / 'transactions' / transaction_id
        receipt = root / 'boot-lineage' / (candidate_sha + '-' + transaction_id + '.json')
        legacy = root / 'boot-lineage' / (candidate_sha + '.json')
        if not provisional and not receipt.exists() and not receipt.is_symlink():
            receipt = legacy
        if not provisional:
            link = production_json(receipt)
            base_keys = {'transaction', 'approvalSha256', 'candidateSha256', 'previousSha256'}
            checkpoint_keys = base_keys | {'format', 'bootId', 'proofSha256', 'rolesSha256'}
            checkpoint = isinstance(link, dict) and set(link) == checkpoint_keys
            if (not isinstance(link, dict) or set(link) not in (base_keys, checkpoint_keys) or
                    link['candidateSha256'] != candidate_sha or
                    link['transaction'] != transaction_id or
                    not isinstance(link['approvalSha256'], str) or
                    not re.fullmatch(r'[0-9a-f]{64}', link['approvalSha256'])):
                break
            state = production_json(transaction / 'state.json')
            if (state.get('transaction') != link['transaction'] or
                    state.get('approvalSha256') != link['approvalSha256'] or
                    state.get('candidateSha256') != candidate_sha or
                    state.get('previousSha256') != link['previousSha256'] or
                    state.get('phase') not in ('PROMOTING', 'COMPLETED')):
                break
            health = production_json(transaction / 'health.json')
            if health.get('status') != 'healthy' or health.get('baselineSha256') != candidate_sha:
                break
            if state['phase'] == 'PROMOTING':
                pointer = production_json(root / 'current.json')
                if pointer.get('sha256') != candidate_sha or pointer.get('transaction') != link['transaction']:
                    break
            record = root / 'baselines' / (candidate_id + '.json')
            if sha(exact_file(record)) != candidate_sha or production_json(record) != current:
                break
        else:
            if (not isinstance(pending, dict) or set(pending) != {'transaction', 'approvalSha256'} or
                    pending['transaction'] != transaction_id):
                break
            state = pending_state
            pointer = production_json(root / 'current.json')
            if (state.get('transaction') != pending['transaction'] or
                    state.get('phase') not in ('INSTALLING', 'STARTING') or
                    state.get('approvalSha256') != pending['approvalSha256'] or
                    state.get('candidateSha256') != candidate_sha or
                    pointer.get('sha256') != state.get('previousSha256')):
                break
            link = dict(pending, candidateSha256=candidate_sha,
                        previousSha256=state['previousSha256'])
            checkpoint = False
        approval_path = transaction / 'approval.json'
        if sha(exact_file(approval_path)) != link['approvalSha256']:
            break
        approval = production_json(approval_path)
        if (approval.get('format') != 'runtime-provisioning-v2' or
                approval.get('transaction') != link['transaction']):
            break
        previous_path = transaction / 'previous.json'
        candidate_path = transaction / 'candidate.json'
        if (sha(exact_file(previous_path)) != link['previousSha256'] or
                sha(exact_file(candidate_path)) != candidate_sha or
                production_json(previous_path) != approval.get('previous') or
                production_json(candidate_path) != approval.get('candidate') or
                approval['candidate'] != current):
            break
        previous = approval['previous']
        if (current['parentSha256'] != link['previousSha256'] or
                current['generation'] != previous['generation'] + 1 or
                any(current[key] != previous[key] for key in ('gatewayHome', 'machineCode',
                     'architecture', 'pointStoreAbi', 'pointStoreSize', 'clusterProtocol',
                     'controlEnabled', 'units', 'gates', 'segments', 'retiredSources',
                     'historical', 'bootId', 'trust', 'healthPolicy'))):
            break
        prior_pointer = production_json(transaction / 'previous-pointer.json')
        if (not isinstance(prior_pointer, dict) or
                set(prior_pointer) != {'baselineId', 'sha256', 'healthSha256', 'transaction'} or
                prior_pointer['baselineId'] != previous['baselineId'] or
                prior_pointer['sha256'] != link['previousSha256'] or
                not isinstance(prior_pointer['transaction'], str) or
                not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', prior_pointer['transaction'])):
            break
        if checkpoint:
            if (link['format'] != 'runtime-boot-lineage-checkpoint-v1' or
                    link['bootId'] != proof_boot or
                    link['proofSha256'] != sha(exact_file(proof_path)) or
                    link['rolesSha256'] != production_roles_sha(current)):
                break
            return depth + 1
        current = previous
        transaction_hint = prior_pointer['transaction']
    raise ValueError('invalid native boot authority lineage')


def production_boot_record(home, baseline, path, intent=False, historical_boot=None):
    record = production_json(path)
    boot = historical_boot if historical_boot is not None else Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if not isinstance(boot, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', boot):
        raise ValueError('invalid native boot ID')
    keys = {'format', 'bootId', 'rolesSha256', 'sourceRecord', 'sourceSha256', 'creatorPin', 'segments'}
    if intent:
        keys |= {'phase', 'inProgress'}
    if (not isinstance(record, dict) or set(record) != keys or record['format'] != 'runtime-native-boot-v1' or
            record['bootId'] != boot or
            not isinstance(record['sourceRecord'], str) or
            not re.fullmatch(r'(?:baselines/[A-Za-z0-9_-]{1,63}\.json|transactions/[A-Za-z0-9_-]{1,63}/(?:previous|candidate)\.json)', record['sourceRecord']) or
            not isinstance(record['sourceSha256'], str) or not re.fullmatch('[0-9a-f]{64}', record['sourceSha256'])):
        raise ValueError('invalid native boot authority')
    source = home / 'data' / PRODUCTION_DIR / record['sourceRecord']
    if sha(exact_file(source)) != record['sourceSha256']:
        raise ValueError('native boot source record drift')
    original = production_json(source)
    production_schema(original, home)
    if (production_roles_sha(original) != record['rolesSha256'] or
            baseline['segments'] != original['segments'] or baseline['bootId'] != original['bootId'] or
            original['files'].get('bin/pointctl') != record['creatorPin']):
        raise ValueError('native boot creator/role lineage drift')
    production_boot_lineage(home, baseline, original, path, boot)
    allowed = {s['name'] for s in baseline['segments']}
    rows = record['segments']
    if (not isinstance(rows, list) or any(not isinstance(s, dict) or set(s) != {'name', 'dev', 'ino'} or
            not isinstance(s['name'], str) or s['name'] not in allowed or type(s['dev']) is not int or s['dev'] < 0 or
            type(s['ino']) is not int or s['ino'] <= 0 for s in rows) or
            len({s['name'] for s in rows}) != len(rows)):
        raise ValueError('invalid native boot segment identities')
    if intent:
        if (record['phase'] not in ('CREATING', 'READY') or record['inProgress'] is not None and
                (not isinstance(record['inProgress'], str) or
                 record['inProgress'] not in allowed - {s['name'] for s in rows}) or
                record['phase'] == 'READY' and
                (record['inProgress'] is not None or {s['name'] for s in rows} != allowed)):
            raise ValueError('invalid native boot progress')
    elif {s['name'] for s in rows} != allowed:
        raise ValueError('incomplete native boot proof')
    return record


def production_segment_identities(home, baseline):
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if boot == baseline['bootId']:
        return baseline['segments']
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', boot):
        raise ValueError('invalid native boot ID')
    record = production_boot_record(home, baseline, home / 'data' / PRODUCTION_DIR / 'boots' / (boot + '.json'))
    return record['segments']


def production_boot_lineage_depth(home, baseline):
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if boot == baseline['bootId']:
        return 0
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', boot):
        raise ValueError('invalid native boot ID')
    proof = home / 'data' / PRODUCTION_DIR / 'boots' / (boot + '.json')
    record = production_boot_record(home, baseline, proof)
    source = home / 'data' / PRODUCTION_DIR / record['sourceRecord']
    return production_boot_lineage(home, baseline, production_json(source), proof, boot)


def production_verify(home, baseline, files=True, units=True, segments=True, config=True, roles=True):
    production_schema(baseline, home)
    if production_pin(Path('/etc/modbus-gateway/ota.pub'))['sha256'] != baseline['trust']['publicKeySha256']:
        raise ValueError('production signature public-key fingerprint drift')
    config_root = home / 'config/runtime'
    if production_json(config_root / 'device_identity.json').get('machineCode') != baseline['machineCode']:
        raise ValueError('production configuration/identity drift')
    if config:
        actual = {}
        names = set()
        for p in config_root.rglob('*'):
            if p.is_symlink():
                raise ValueError('symlink production configuration')
            if p.is_file():
                actual[str(p.relative_to(config_root))] = sha(exact_file(p))
                if p.suffix == '.json':
                    names.update(configured_names(production_json(p)))
        if actual != baseline['configSha256']:
            raise ValueError('production configuration/identity drift')
        if not names <= {s['name'] for s in baseline['segments']} or names & set(baseline['retiredSources']):
            raise ValueError('production implicit/custom SHM coverage incomplete')
        if roles and baseline['format'] == 'runtime-baseline-v2':
            production_roles(home, baseline)
    # Historical records remain immutable proof, never a second live authority.
    for path, pin in baseline['historical'].items():
        if production_pin(Path(path)) != pin:
            raise ValueError('production historical lineage drift: ' + path)
    for name, meta in baseline['retiredSources'].items():
        path = Path('/dev/shm') / name
        if path.exists() or path.is_symlink():
            s = exact_file(path).stat()
            if (baseline['bootId'] != Path('/proc/sys/kernel/random/boot_id').read_text().strip() or
                    {'dev': s.st_dev, 'ino': s.st_ino} != meta):
                raise ValueError('retired source recreated or replaced')
    no_old_mappings(baseline['retiredSources'], include_fds=True)
    if files and production_files(home) != baseline['files']:
        raise ValueError('production paired runtime drift')
    if files:
        for relative in baseline['files']:
            production_architecture(home / relative)
    if units:
        production_units_present(baseline['units'])
        if production_unit_files(baseline['units']) != baseline['unitFiles']:
            raise ValueError('production unit/override drift')
        production_effective(baseline)
    if segments:
        for segment in production_segment_identities(home, baseline):
            path = Path('/dev/shm') / segment['name']
            point_store_abi(path, 11)
            value = path.stat()
            if (value.st_dev, value.st_ino) != (segment['dev'], segment['ino']):
                raise ValueError('production v11 segment identity changed')


def production_context(home, segments=True, units=True):
    root = home / 'data' / PRODUCTION_DIR
    pending = root / 'pending.json'
    pointer = production_json(root / 'current.json')
    if not isinstance(pointer, dict) or set(pointer) != {'baselineId', 'sha256', 'healthSha256', 'transaction'}:
        raise ValueError('invalid production baseline pointer')
    if (not isinstance(pointer['baselineId'], str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', pointer['baselineId']) or
            not isinstance(pointer['transaction'], str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', pointer['transaction']) or
            any(not isinstance(pointer[k], str) or not re.fullmatch('[0-9a-f]{64}', pointer[k]) for k in ('sha256', 'healthSha256'))):
        raise ValueError('invalid production baseline ID')
    record = root / 'baselines' / (pointer['baselineId'] + '.json')
    if sha(exact_file(record)) != pointer['sha256']:
        raise ValueError('production baseline digest changed')
    baseline = production_json(record)
    if baseline.get('baselineId') != pointer['baselineId']:
        raise ValueError('production baseline name mismatch')
    transaction = root / 'transactions' / pointer['transaction']
    decision = production_json(transaction / 'health.json')
    if sha(transaction / 'health.json') != pointer['healthSha256'] or decision.get('baselineSha256') != pointer['sha256'] or decision.get('status') != 'healthy':
        raise ValueError('production promotion health proof missing')
    if pending.exists() or pending.is_symlink():
        state = production_json(transaction / 'state.json')
        ref = production_json(pending)
        if ref != {'transaction': pointer['transaction'], 'approvalSha256': state['approvalSha256']} or state['phase'] not in ('PROMOTING', 'COMPLETED', 'RECOVERED'):
            raise ValueError('unresolved production install inhibits startup')
    production_verify(home, baseline, segments=segments, units=units)
    return baseline


def production_unit(home, unit):
    root = home / 'data' / PRODUCTION_DIR
    pending = root / 'pending.json'
    if pending.exists() or pending.is_symlink():
        ref = production_json(pending)
        if (not isinstance(ref, dict) or set(ref) != {'transaction', 'approvalSha256'} or
                not isinstance(ref['transaction'], str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', ref['transaction']) or
                not isinstance(ref['approvalSha256'], str) or not re.fullmatch('[0-9a-f]{64}', ref['approvalSha256'])):
            raise ValueError('invalid production transaction')
        transaction = root / 'transactions' / ref['transaction']
        state = production_json(transaction / 'state.json')
        if state['approvalSha256'] != ref['approvalSha256']:
            raise ValueError('production pending approval changed')
        approval_path = transaction / 'approval.json'
        if sha(exact_file(approval_path)) != ref['approvalSha256']:
            raise ValueError('production provisional approval digest changed')
        approval = production_json(approval_path)
        if state['phase'] in ('STARTING', 'RECOVERING_START'):
            baseline = production_json(transaction / ('candidate.json' if state['phase'] == 'STARTING' else 'previous.json'))
            if sha(transaction / ('candidate.json' if state['phase'] == 'STARTING' else 'previous.json')) != state['startingBaselineSha256']:
                raise ValueError('production provisional baseline changed')
            if baseline != approval['candidate' if state['phase'] == 'STARTING' else 'previous']:
                raise ValueError('production provisional baseline is not approved')
            production_verify(home, baseline)
        else:
            baseline = production_context(home)
    else:
        baseline = production_context(home)
    if unit not in baseline['allowedUnits']:
        raise ValueError('production controller/unapproved participant inhibited: ' + unit)
    return baseline


def production_present(home):
    root = home / 'data' / PRODUCTION_DIR
    return any((root / name).exists() or (root / name).is_symlink() for name in ('current.json', 'pending.json'))


def core_absent_qt(home):
    for relative in ('bin/KY-EMS', 'bin/LocalDisplayQtEms', 'ky-ems/KY-EMS', 'config/runtime/qt-display.env'):
        path = home / relative
        if path.exists() or path.is_symlink():
            raise ValueError('core monitor-only omitted Qt installation exists: ' + relative)
    for unit in (QT_UNIT, 'qt-display-bridge.service'):
        expected = {'LoadState': 'not-found', 'ActiveState': 'inactive', 'MainPID': '0',
                    'FragmentPath': '', 'DropInPaths': '', 'NeedDaemonReload': 'no'}
        if any(systemctl_value(unit, prop) != value for prop, value in expected.items()):
            raise ValueError('core monitor-only omitted Qt unit is not absent: ' + unit)
        directory_names = {unit + '.d', 'service.d'}
        prefix = unit[:-len('.service')]
        for i, character in enumerate(prefix):
            if character == '-':
                directory_names.add(prefix[:i + 1] + '.service.d')
        for base in ('/etc/systemd/system', '/run/systemd/system', '/usr/local/lib/systemd/system',
                     '/usr/lib/systemd/system', '/lib/systemd/system', '/etc/systemd/system.control',
                     '/run/systemd/system.control', '/run/systemd/transient', '/run/systemd/generator',
                     '/run/systemd/generator.early', '/run/systemd/generator.late'):
            path = Path(base) / unit
            if path.exists() or path.is_symlink():
                raise ValueError('core monitor-only omitted Qt unit file exists: ' + unit)
            for dirname in directory_names:
                directory = Path(base) / dirname
                if directory.is_symlink() or (directory.exists() and (
                        not directory.is_dir() or any(directory.iterdir()))):
                    raise ValueError('core monitor-only omitted Qt unit coverage exists: ' + unit)


def core_scope(home, approval, manifest=None):
    profile = approval.get('coreReadOnlyTest')
    if (not isinstance(profile, dict) or set(profile) != {
            'schemaVersion', 'profile', 'sourceCommit', 'programManifestSha256', 'preservedRuntimeSha256'} or
            profile['schemaVersion'] != 'core-readonly-test-1' or
            not isinstance(profile['profile'], str) or profile['profile'] not in CORE_READERS or
            approval.get('nodeId') != CORE_NODES[profile['profile']] or
            home != Path('/opt/modbus-gateway') or approval.get('mode') != 'standalone' or
            approval.get('controlEnabled') is not False or
            any(k in approval for k in ('aReadonlyAcquisition', 'aReadonlyWindowsIntegration')) or
            re.fullmatch('[0-9a-f]{40}', str(profile['sourceCommit'])) is None or
            re.fullmatch('[0-9a-f]{64}', str(profile['programManifestSha256'])) is None or
            profile['programManifestSha256'] != approval.get('programManifestSha256')):
        raise ValueError('core read-only requires exact independent approval and reader scope')
    missing = set(CORE_READERS[profile['profile']] + ['gateway-services.service', QT_UNIT,
                  'qt-display-bridge.service']) - set(approval.get('units', []))
    if missing:
        # Only the no-Qt Monitor-only device may preserve its smaller predecessor scope.
        if profile['profile'] != 'MONITOR_ONLY' or missing != {QT_UNIT, 'qt-display-bridge.service'}:
            raise ValueError('core read-only requires exact independent approval and reader scope')
        core_absent_qt(home)
    # Recovered A metadata is preservation-only, never a request for A rewrites/Qt activation.
    for key, relative in (('qtDisplayEnvSha256', 'qt-display.env'),
                          ('aInboundDisableSourceSha256', 'apps/monitor-service.json')):
        if key in approval and ('recoveredFrom' not in approval or
                approval[key] != approval.get('configSha256', {}).get(relative)):
            raise ValueError('core inherited metadata must preserve the approved source')
    if 'scadaReadOnlyProject' in approval:
        if 'recoveredFrom' not in approval:
            raise ValueError('core SCADA metadata is only a predecessor preservation proof')
        scada_readonly_binding(home, approval, 'old')
    preserved = profile['preservedRuntimeSha256']
    if not isinstance(preserved, dict) or any(
            path not in {'bin/' + name for name in RUNTIME_BINARIES | {'stress_runner'}} | {'ky-ems/KY-EMS'} or
            re.fullmatch('[0-9a-f]{64}', str(pin)) is None or
            approval.get('installedRuntimeSha256', {}).get(path) != pin or
            path in approval.get('installPaths', {}).values() for path, pin in preserved.items()):
        raise ValueError('core preserved runtime inventory must be exact and disjoint')
    if manifest is not None:
        components = manifest.get('components', [])
        if (manifest.get('sourceCommit') != profile['sourceCommit'] or not isinstance(components, list) or
                len(components) != len(CORE_PROGRAMS) or
                {c.get('target') for c in components if isinstance(c, dict)} != CORE_PROGRAMS or
                any(not isinstance(c, dict) or c.get('kind') != 'product' or
                    c.get('archivePath') != 'bin/' + c.get('target', '') or
                    approval.get('installPaths', {}).get(c['target']) != 'bin/' + c['target'] or
                    re.fullmatch('[0-9a-f]{64}', str(c.get('sha256'))) is None or
                    type(c.get('bytes')) is not int or c['bytes'] < 64 for c in components)):
            raise ValueError('core requires complete exact approved AArch64 program manifest')
    return list(CORE_READERS[profile['profile']])


def point_store_abi(path, version):
    with exact_file(path).open('rb') as stream:
        header = stream.read(8)
    if path.stat().st_size != POINT_STORE_SIZES[version] or header != struct.pack('<II', 0x4d505354, version):
        raise ValueError('point-store native layout/version/size mismatch: ' + path.name)


def core_documents_safe(home, approval, documents):
    readers = core_scope(home, approval)
    def safe(value):
        if isinstance(value, dict):
            for key in ('emsCluster', 'agcAvc'):
                if key in value and (not isinstance(value[key], dict) or
                        value[key].get('enabled') is not False or
                        value[key].get('controlEnabled', False) is not False):
                    raise ValueError('core read-only forbids control authority')
            if value.get('startupWrites') not in (None, []):
                raise ValueError('core read-only forbids startup writes')
            if 'write' in value and (not isinstance(value['write'], dict) or
                    value['write'].get('enable', False) is not False):
                raise ValueError('core read-only forbids writable points')
            if 'northboundServer' in value and (not isinstance(value['northboundServer'], dict) or
                    value['northboundServer'].get('enabled') is not False):
                raise ValueError('core read-only forbids northbound writes')
            for item in value.values(): safe(item)
        elif isinstance(value, list):
            for item in value: safe(item)
    for doc in documents.values(): safe(doc)
    monitor = documents.get('apps/monitor-service.json', {})
    if monitor.get('systemMonitor', {}).get('enabled') is not True:
        raise ValueError('core monitor must be explicitly enabled')
    if readers == [MONITOR_UNIT]:
        if (any(name.startswith('devices/') for name in documents) or
                any(doc.get('deviceConfigFiles') not in (None, []) or
                    doc.get('mqtt', {}).get('enabled') is not False
                    for name, doc in documents.items() if name.startswith('apps/'))):
            raise ValueError('monitor-only requires empty device inventory and disabled MQTT')
        if QT_UNIT not in approval['units'] and monitor.get('localDisplay', {}).get('enabled', False) is not False:
            raise ValueError('core monitor-only omitted Qt requires disabled local display')
    else:
        device = 'devices/device_modbusRTU_2_readonly.json'
        mqtt = documents.get('apps/mqtt-service.json', {})
        if (device not in documents or mqtt.get('mqtt', {}).get('enabled') is not True or
                any(doc.get('deviceConfigFiles') != [str(home / 'config/runtime' / device)]
                    for doc in (monitor, mqtt))):
            raise ValueError('core RTU/MQTT requires the exact read-only device binding')


def core_config(home, state_dir, approval, state):
    core_scope(home, approval)
    names = {s['source']: s['target'] for s in state['segments']}
    if names != {s['source']: s['target'] for s in approval['segments']}:
        raise ValueError('core SHM lineage changed')
    root = home / 'config/runtime'
    actual = {str(p.relative_to(root)): sha(exact_file(p)) for p in root.rglob('*') if p.is_file()}
    if set(actual) != set(approval['configSha256']) or actual != state['configSha256']:
        raise ValueError('core configuration inventory changed')
    for relative, pin in approval['configSha256'].items():
        rows = [(i, row) for i, row in enumerate(state['files']) if row['path'] == 'config/runtime/' + relative]
        if len(rows) > 1 or rows and rows[0][1]['oldSha256'] != pin:
            raise ValueError('core config backup binding changed')
        current = exact_file(root / relative)
        original = exact_file(state_dir / 'files' / str(rows[0][0])) if rows else current
        if sha(original) != pin:
            raise ValueError('core original config backup changed')
        if current.suffix == '.json':
            before = json.loads(original.read_text())
            expected = switch_names(before, names)
            if relative == 'device_identity.json' and expected != before:
                raise ValueError('core cannot remap device identity')
            if json.loads(current.read_text()) != expected:
                raise ValueError('core configuration exceeds internal SHM remap')
        elif current.read_bytes() != original.read_bytes():
            raise ValueError('core cannot change non-JSON configuration')
    manifest_path = exact_file(state_dir / 'program-manifest.json')
    if sha(manifest_path) != approval['programManifestSha256']:
        raise ValueError('core manifest approval changed')
    manifest = json.loads(manifest_path.read_text())
    core_scope(home, approval, manifest)
    expected_runtime = dict(approval['coreReadOnlyTest']['preservedRuntimeSha256'])
    expected_runtime.update({'bin/' + c['target']: c['sha256'] for c in manifest['components']})
    installed = {str(p.relative_to(home)): sha(exact_file(p)) for d in ('bin', 'ky-ems')
                 for p in (home / d).rglob('*') if p.is_file() and runtime_file(p, p)}
    if installed != expected_runtime:
        raise ValueError('core complete installed runtime inventory changed')
    for name, pin in approval['activationStartupScripts'].items():
        if sha(exact_file(home / 'bin' / name)) != pin['newSha256']:
            raise ValueError('core paired runtime script changed')
    core_documents_safe(home, approval, {str(p.relative_to(root)): json.loads(p.read_text()) for p in root.rglob('*.json')})


def core_denied_units(approval, allowed, inhibited=None):
    for unit in set(approval['units']) - set(allowed):
        template = unit.endswith('@.service')
        if not template and (systemctl_value(unit, 'ActiveState') not in ('inactive', 'failed') or
                systemctl_value(unit, 'MainPID') != '0'):
            raise ValueError('core excluded participant is not stopped: ' + unit)
        if inhibited is not None:
            home = Path(approval['gatewayHome'])
            path, body = denied_dropin(home, approval['transactionId'], unit)
            if inhibited:
                body = ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n').encode()
            if (exact_file(path).read_bytes() != body or stat.S_IMODE(path.stat().st_mode) != 0o644):
                raise ValueError('core excluded participant fence coverage changed: ' + unit)
            if template:
                # Bare templates have no systemctl show object; retain the existing fixed-directory coverage gate.
                directory_names = {unit + '.d', 'service.d'}
                prefix = unit.split('@')[0]
                for i, character in enumerate(prefix):
                    if character == '-':
                        directory_names.add(prefix[:i + 1] + '.service.d')
                for base in ('/etc/systemd/system', '/run/systemd/system', '/usr/local/lib/systemd/system',
                             '/usr/lib/systemd/system', '/lib/systemd/system', '/etc/systemd/system.control',
                             '/run/systemd/system.control', '/run/systemd/transient', '/run/systemd/generator',
                             '/run/systemd/generator.early', '/run/systemd/generator.late'):
                    for dirname in directory_names:
                        directory = Path(base) / dirname
                        wanted = {path.name} if directory == path.parent else set()
                        if directory.is_symlink() or (directory.exists() and (
                                not directory.is_dir() or {p.name for p in directory.iterdir()} != wanted)) or (
                                wanted and not directory.exists()):
                            raise ValueError('core excluded participant fence coverage changed: ' + unit)
            elif set(systemctl_value(unit, 'DropInPaths').split()) != {str(path)}:
                raise ValueError('core excluded participant fence coverage changed: ' + unit)


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
    core = state.get('activationProfile') == CORE_PROFILE
    if core != ('coreReadOnlyTest' in approval):
        raise ValueError('core cannot fall back to a historical profile')
    readers = (core_scope(home, approval) if core else A_ACQUISITION_READERS if acquisition else
               [MONITOR_UNIT] if state.get('activationProfile') == 'B_MONITOR' else [MONITOR_UNIT, QT_UNIT])
    watchdog = (not core and 'gateway-health-watchdog.service' in approval['units'] and readers == [MONITOR_UNIT])
    allowed = ['gateway-services.service'] + (['gateway-health-watchdog.service'] if watchdog else []) + readers
    if (state.get('phase') != 'ACTIVATED_CONTROL_DISABLED' or
            state.get('activationProfile') not in ('B_MONITOR', 'A_MONITOR_QT', A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE, CORE_PROFILE) or
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


def a_acquisition_units(home, approval, monitor_target, inhibited, core=False):
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
    if core:
        readers = core_scope(home, approval)
        specs = tuple(s for s in specs if s[0] in readers + ['gateway-services.service'])
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


def no_old_mappings(names, include_fds=False):
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
            if include_fds:
                for fd in (entry / 'fd').iterdir():
                    try:
                        value = fd.stat()
                        if (os.major(value.st_dev), os.minor(value.st_dev), value.st_ino) in identities:
                            raise ValueError('old SHM still open: ' + entry.name)
                    except (FileNotFoundError, ProcessLookupError):
                        continue
        except (FileNotFoundError, ProcessLookupError):
            continue


def activated_unit(home, unit):
    if production_present(home):
        production_unit(home, unit)
        return
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
    core = state['activationProfile'] == CORE_PROFILE
    if core or state['activationProfile'] in (A_ACQUISITION_PROFILE, A_WINDOWS_PROFILE):
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
            if core:
                point_store_abi(target, 11)
        elif not boot_changed:
            raise ValueError('target SHM disappeared in same boot')
    no_old_mappings(sources, include_fds=core)
    if (not core and sha(home / 'bin/SystemMonitor') != MONITOR_BINARY or
            sha(Path('/etc/systemd/system/system-monitor@.service')) != MONITOR_TEMPLATE):
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
    if core:
        core_config(home, Path(active['stateDir']), approval, state)
        core_denied_units(approval, allowed, inhibited=False)
        a_acquisition_units(home, approval, monitor_target, inhibited=False, core=True)
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
    if production_present(home):
        production_unit(home, 'gateway-services.service')
        return
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
    if len(sys.argv) == 4 and sys.argv[1] == 'production-unit':
        production_unit(Path(sys.argv[2]), sys.argv[3])
        return
    if len(sys.argv) == 4 and sys.argv[1] == 'denied-unit':
        raise ValueError('unit remains inhibited by offline activation: ' + sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == 'activated-unit':
        activated_unit(Path(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == 'activated-list':
        home = Path(sys.argv[2])
        if production_present(home):
            baseline = production_unit(home, 'gateway-services.service')
            if baseline['format'] == 'runtime-baseline-v2':
                print('\n'.join(role['unit'] for role in baseline['roleAuthority']['resolved']['roles']
                                if role['kind'] == 'service'))
            else:
                print('\n'.join(u for u in baseline['allowedUnits'] if u in PRODUCTION_READERS))
            return
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
