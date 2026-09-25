#!/usr/bin/env python3
"""Offline config-only predicate for A RTU acquisition + MQTT TX/full preparation.

This does not authorize activation, inspect devices, or replace the C++ loader.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re

APPS = ('camera-service.json', 'monitor-service.json', 'mqtt-service.json')
DEVICE = 'devices/device_modbusRTU_2_readonly.json'
DEVICE_PATH = '/opt/modbus-gateway/config/runtime/' + DEVICE
RX_TOPICS = ('commandRequestTopic', 'otaRequestTopic', 'realtimeRequestTopic',
             'systemMonitorRequestTopic', 'diagRequestTopic', 'configPullRequestTopic',
             'configApplyRequestTopic', 'configDeleteRequestTopic', 'configRestoreRequestTopic',
             'recordingRequestTopic', 'recordingAckTopic')
POINT_GROUPS = ('pointGroups', 'iecPointGroups', 'fourRemote', 'fourRemotePoints',
                'pointsByType', 'pointsByCategory')
WORKER_DEFAULTS = {'healthHeartbeatMs': 1000, 'failoverTimeoutMs': 3000,
                   'retryMinMs': 500, 'retryMaxMs': 5000}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def obj(value, path):
    require(isinstance(value, dict), path + ': explicit object required')
    return value


def array(value, path):
    require(isinstance(value, list), path + ': array required')
    return value


def disabled(parent, key, path):
    require(obj(parent.get(key), path + '.' + key).get('enabled') is False,
            path + '.' + key + '.enabled must be explicitly false')


def points(owner, path):
    for n, point in enumerate(array(owner.get('points', []), path + '.points')):
        yield obj(point, path + '.points'), path + '.points[' + str(n) + ']'
    # ConfigLoader::appendPointGroups accepts each alias independently, including disabled meters.
    for key in POINT_GROUPS:
        if key not in owner or owner[key] is None:
            continue
        groups = owner[key]
        if isinstance(groups, list):
            groups = {key: groups}
        for category, entries in obj(groups, path + '.' + key).items():
            group_path = path + '.' + key + '.' + category
            if isinstance(entries, dict):
                entries = entries.get('points')
            for n, point in enumerate(array(entries, group_path)):
                yield obj(point, group_path), group_path + '[' + str(n) + ']'


def check_profile(documents, target_names, *, _pending_fields=()):
    obj(documents, 'documents')
    names = array(target_names, 'targetSharedMemoryNames')
    require(names and all(isinstance(n, str) and re.fullmatch(r'[A-Za-z0-9_-]{1,63}', n)
                          for n in names) and len(set(names)) == len(names), 'invalid target names')
    expected = {'apps/' + name for name in APPS} | {DEVICE}
    require(set(documents) == expected, 'exact three apps and one RTU device required')
    device = obj(documents[DEVICE], DEVICE)
    protocol = obj(device.get('protocol'), DEVICE + '.protocol')
    require(protocol.get('type') == 'modbus_rtu', 'only explicit modbus_rtu protocol allowed')
    require(protocol.get('standardPointsFile', '') == '', 'external standard points refused')
    transport = obj(protocol.get('transport'), 'protocol.transport')
    require(isinstance(transport.get('serialPort'), str) and
            re.fullmatch(r'/dev/[A-Za-z0-9_-]+', transport['serialPort']), 'explicit serial port required')
    require(device.get('startupWrites', []) == [], 'startupWrites must be empty')
    disabled(device, 'northboundServer', DEVICE)
    if 'server' in device:
        disabled(device, 'server', DEVICE)
    memory = obj(device.get('memoryStore'), DEVICE + '.memoryStore')
    # ConfigLoader uses MemoryStoreConfig defaults for absent/null fields.
    enabled = True if memory.get('enabled') is None else memory['enabled']
    backend = 'memory' if memory.get('backend') is None else memory['backend']
    require(enabled is True, DEVICE + '.memoryStore.enabled must resolve to true')
    # main.cpp constructs the SHM store regardless of backend; do not invent an enum.
    require(isinstance(backend, str), DEVICE + '.memoryStore.backend must resolve to a string')
    require(memory.get('sharedMemoryName') in names, 'RTU store outside target set')
    meters = array(device.get('meters'), DEVICE + '.meters')
    owners = [(device, DEVICE, True)]
    for n, meter in enumerate(meters):
        path = DEVICE + '.meters[' + str(n) + ']'
        obj(meter, path)
        require(type(meter.get('enabled', True)) is bool, path + '.enabled must be boolean')
        owners.append((meter, path, meter.get('enabled', True)))
    indexes, collected, flagged_full = set(), set(), set()
    for owner, path, owner_enabled in owners:
        for point, location in points(owner, path):
            index = point.get('index')
            require(type(index) is int and 0 < index <= 0xffffffff and index not in indexes,
                    location + ': invalid/duplicate index')
            indexes.add(index)
            require(type(point.get('enabled', True)) is bool, location + ': invalid enabled')
            write = obj(point.get('write'), location + '.write')
            require(write.get('enable') is False, location + ': write.enable must be explicit false')
            require(point.get('initialValue') is None and write.get('startupValue') is None and
                    point.get('retain', False) is False, location + ': initialization/retention refused')
            read = obj(point.get('read'), location + '.read')
            require(type(read.get('enable')) is bool and type(read.get('function')) is int and
                    read['function'] in (1, 2, 3, 4), location + ': only read functions 1..4 allowed')
            for key in ('northbound', 'forward'):
                if key in point:
                    disabled(point, key, location)
            full = point.get('fullUpload', False)
            require(type(full) is bool, location + ': invalid fullUpload')
            effective = owner is not device or not meters
            # Router also installs top-level routes when logical meters exist.
            if full:
                flagged_full.add(index)
            if effective and owner_enabled and point.get('enabled', True) and read['enable']:
                collected.add(index)
    require(collected, 'at least one enabled acquisition point required')

    for name in APPS:
        path = 'apps/' + name
        app = obj(documents[path], path)
        require(app.get('deviceConfigFiles') == ([] if name == 'camera-service.json' else [DEVICE_PATH]),
                path + ': exact deviceConfigFiles required')
        # Prevent sibling discovery and virtual route creation, not just physical dispatch.
        require('agcAvc' not in app, path + ': AGC mailbox configuration refused')
        for key in ('computeEngine', 'cameraService', 'ota'):
            disabled(app, key, path)
        disabled(app, 'emsCluster', path)
        require(app['emsCluster'].get('controlEnabled') is False, path + ': cluster control disabled required')
        monitor = obj(app.get('systemMonitor'), path + '.systemMonitor')
        require(monitor.get('enabled') is (name == 'monitor-service.json'), path + ': Monitor role mismatch')
        disabled(monitor, 'directMaintenance', path + '.systemMonitor')
        mqtt = obj(app.get('mqtt'), path + '.mqtt')
        driver = obj(app.get('mqttDriver'), path + '.mqttDriver')
        require(type(mqtt.get('enabled')) is bool and
                (name == 'camera-service.json' or mqtt['enabled'] is (name == 'mqtt-service.json')) and
                driver.get('enabled') is (name == 'mqtt-service.json'), path + ': MQTT role mismatch')
        if name == 'mqtt-service.json':
            for key in RX_TOPICS:
                require(mqtt.get(key) == '', path + '.mqtt.' + key + ': explicit empty string required')
        if name != 'camera-service.json':
            stores = array(driver.get('sharedMemoryNames'), path + '.mqttDriver.sharedMemoryNames')
            require(driver.get('sharedMemoryName') in names, path + ': primary store outside target set')
            require(stores and all(isinstance(n, str) and n in names for n in stores), path + ': invalid stores')
        # Qt merges this list even when localDisplay.enabled=false. Missing/null means [].
        display = app.get('localDisplay')
        if display is not None:
            display = obj(display, path + '.localDisplay')
            display_names = display.get('sharedMemoryNames')
            if display_names is not None:
                display_names = array(display_names, path + '.localDisplay.sharedMemoryNames')
                require(all(isinstance(n, str) and (not n or n in names) for n in display_names),
                        path + '.localDisplay.sharedMemoryNames outside target set')
        if name == 'mqtt-service.json':
            # mqtt_driver_main adds device stores even when not in the app's explicit list.
            worker = obj(driver.get('fullUploadWorker'), 'mqttDriver.fullUploadWorker')
            require(worker.get('mode') == 'inline' and worker.get('eventForwardingEnabled') is False,
                    'first narrow profile requires inline full upload without event delegation')
            prefix = path + '#/mqttDriver/fullUploadWorker/'
            timing = {k: worker[k] if worker.get(k) is not None else default
                      for k, default in WORKER_DEFAULTS.items() if prefix + k not in _pending_fields}
            require(all(type(v) is int for v in timing.values()), 'full worker timings must be integers')
            for key in ('healthHeartbeatMs', 'retryMinMs'):
                require(key not in timing or 100 <= timing[key] <= 60000, 'full worker timing bounds: ' + key)
            for key, minimum, factor in (('failoverTimeoutMs', 'healthHeartbeatMs', 2),
                                         ('retryMaxMs', 'retryMinMs', 1)):
                require(key not in timing or timing[key] <= 300000, 'full worker timing maximum: ' + key)
                require(key not in timing or minimum not in timing or timing[key] >= timing[minimum] * factor,
                        'full worker timing minimum: ' + key)
            interval = driver.get('fullUploadIntervalMs')
            require(type(interval) is int and interval > 0, 'positive fullUploadIntervalMs required')
            if path + '#/mqtt/telemetryTopic' not in _pending_fields and path + '#/mqtt/fullTelemetryTopic' not in _pending_fields:
                require(isinstance(mqtt.get('fullTelemetryTopic'), str) and mqtt['fullTelemetryTopic'].strip(),
                        'nonempty fullTelemetryTopic required')
            selected = array(driver.get('fullUploadIndexes'), 'mqttDriver.fullUploadIndexes')
            require(all(type(i) is int and i in collected for i in selected), 'invalid fullUploadIndexes')
            require(type(driver.get('publishAllOnFull')) is bool, 'publishAllOnFull must be boolean')
            selected = set(selected) | flagged_full
            # resolveFullUploadIndexes falls back to all routes for empty effective selection.
            effective_full = indexes if not selected or (driver['publishAllOnFull'] and not flagged_full) else selected
            require(collected <= effective_full, 'effective full upload must cover acquisition indexes')
    return {'profile': 'A_RTU_READONLY_MQTT_TX_FULL_PREPARATION',
            'configPredicatePassed': not _pending_fields,
            'configurationStatus': 'PENDING' if _pending_fields else 'PASS',
            'pendingConfigFields': sorted(_pending_fields),
            'pointCount': len(indexes), 'acquisitionIndexes': sorted(collected),
            'effectiveFullUploadIndexes': sorted(effective_full),
            'effectiveFullTelemetryTopic': mqtt.get('fullTelemetryTopic'),
            'activationAuthorized': False, 'actualAQualified': False, 'realtimeSessionQualified': False,
            'monitorEnvironmentQualified': False,
            'pending': ['actual-config-projection', 'effective-unit-and-dependency-pins',
                        'system-monitor-effective-environment',
                        'existing-activation-gates-integration', 'stopped-state-and-runtime-acceptance']}


class ProjectionPending(Exception):
    """A safety-relevant projected value cannot be evaluated without more evidence."""


def check_projected_profile(entries, target_names):
    """Check redacted preflight entries; hashes bind bytes, not authority or live state."""
    documents, pending = {}, []
    expected = {'apps/' + name for name in APPS} | {DEVICE}
    require(set(obj(entries, 'documents')) == expected, 'exact projected inventory required')
    # Validate every projection before interpreting any defaults.
    for name, entry in entries.items():
        obj(entry, name)
        projection = obj(entry.get('projection'), name + '.projection')
        encoded = (json.dumps(projection, sort_keys=True, separators=(',', ':'),
                              allow_nan=False) + '\n').encode()
        require(hashlib.sha256(encoded).hexdigest() == entry.get('projectionSha256'),
                name + ': projection SHA256 mismatch')
        metadata = obj(projection.get('fieldMetadata'), name + '.fieldMetadata')
        require(not metadata.get('invalidTypes'), name + ': invalid projected types')
        documents[name] = copy.deepcopy(obj(projection.get('document'), name + '.document'))

    try:
        for name, document in documents.items():
            metadata = entries[name]['projection']['fieldMetadata']
            missing = obj(metadata.get('missingFields'), name + '.missingFields')
            unknown = obj(metadata.get('unknownKeys'), name + '.unknownKeys')
            defaulted = set()

            def field(parent, pointer, key, default=None, *, inventory=False):
                obj(parent, name + '#' + pointer)
                location = pointer + '/' + key
                absent = array(missing.get(pointer, []), 'missingFields' + pointer)
                omitted = array(unknown.get(pointer, []), 'unknownKeys' + pointer)
                require(not (key in absent and (key in omitted or key in parent)) and
                        not (key in omitted and key in parent), name + '#' + location + ': inconsistent metadata')
                if key in parent:
                    return parent[key]
                # Non-schema legacy/timing keys use the complete original-key inventory.
                proven = key in absent or pointer in defaulted or (
                    inventory and pointer in missing and pointer in unknown and key not in omitted)
                if key in omitted or not proven:
                    raise ProjectionPending(name + '#' + location)
                parent[key] = copy.deepcopy(default)
                defaulted.add(location)
                return parent[key]

            def section(parent, pointer, key, defaults):
                value = field(parent, pointer, key, defaults)
                if value is None:
                    parent[key] = value = copy.deepcopy(defaults)
                    defaulted.add(pointer + '/' + key)
                obj(value, name + '#' + pointer + '/' + key)
                for setting, default in defaults.items():
                    field(value, pointer + '/' + key, setting, default)
                return value

            def optional_points(owner, pointer):
                entries_at = [(field(owner, pointer, 'points', []), pointer + '/points')]
                for key in POINT_GROUPS:
                    groups = field(owner, pointer, key)
                    if groups is None:
                        continue
                    if isinstance(groups, list):
                        entries_at.append((groups, pointer + '/' + key))
                    else:
                        for category, group in obj(groups, pointer + '/' + key).items():
                            group_path = pointer + '/' + key + '/' + category.replace('~', '~0').replace('/', '~1')
                            if isinstance(group, dict):
                                group = field(group, group_path, 'points')
                                group_path += '/points'
                            entries_at.append((group, group_path))
                for point_list, list_path in entries_at:
                    for i, point in enumerate(array(point_list, list_path)):
                        p = list_path + '/' + str(i)
                        for key, default in (('index', None), ('enabled', True), ('fullUpload', False),
                                             ('initialValue', None), ('retain', False)):
                            field(point, p, key, default)
                        read = field(point, p, 'read')
                        for key in ('enable', 'function'):
                            field(read, p + '/read', key)
                        write = field(point, p, 'write')
                        field(write, p + '/write', 'enable')
                        field(write, p + '/write', 'startupValue')
                        for key in ('northbound', 'forward'):
                            value = field(point, p, key)
                            if value is None:
                                point.pop(key)
                            else:
                                field(value, p + '/' + key, 'enabled', False)

            if name == DEVICE:
                protocol = field(document, '', 'protocol')
                field(protocol, '/protocol', 'type')
                field(protocol, '/protocol', 'standardPointsFile', '')
                transport = field(protocol, '/protocol', 'transport')
                field(transport, '/protocol/transport', 'serialPort')
                field(document, '', 'startupWrites', [])
                section(document, '', 'northboundServer', {'enabled': False})
                server = field(document, '', 'server')
                if server is None:
                    document.pop('server')
                else:
                    field(server, '/server', 'enabled', False)
                section(document, '', 'memoryStore', {'enabled': True, 'backend': 'memory',
                                                     'sharedMemoryName': 'gateway_point_store'})
                optional_points(document, '')
                for i, meter in enumerate(array(field(document, '', 'meters', []), 'meters')):
                    pointer = '/meters/' + str(i)
                    field(meter, pointer, 'enabled', True)
                    optional_points(meter, pointer)
                continue

            field(document, '', 'deviceConfigFiles')
            agc = field(document, '', 'agcAvc')
            require(agc is None and 'agcAvc' in missing.get('', []), name + ': AGC config refused')
            document.pop('agcAvc')
            for key in ('computeEngine', 'cameraService', 'ota'):
                section(document, '', key, {'enabled': False})
            section(document, '', 'emsCluster', {'enabled': False, 'controlEnabled': False})
            monitor = section(document, '', 'systemMonitor', {'enabled': False})
            maintenance = field(monitor, '/systemMonitor', 'directMaintenance')
            if maintenance is None:
                for key in ('listenHost', 'listenHosts', 'listenPort', 'allowedClientCidrs', 'maxRealtimePoints'):
                    present = key in monitor
                    field(monitor, '/systemMonitor', key, inventory=True)
                    require(not present, name + ': legacy directMaintenance configuration refused')
                monitor['directMaintenance'] = {'enabled': False}
            else:
                field(maintenance, '/systemMonitor/directMaintenance', 'enabled', False)
            mqtt = section(document, '', 'mqtt', {'enabled': False})
            driver = section(document, '', 'mqttDriver', {'enabled': False})
            if name != 'apps/camera-service.json':
                field(driver, '/mqttDriver', 'sharedMemoryName', 'gateway_point_store')
                field(driver, '/mqttDriver', 'sharedMemoryNames', [])
            display = field(document, '', 'localDisplay')
            if display is not None:
                field(display, '/localDisplay', 'sharedMemoryNames', [])
            if name != 'apps/mqtt-service.json':
                continue
            for key in RX_TOPICS:
                # Missing/null RX must fail, never receive an empty-string default.
                field(mqtt, '/mqtt', key)
            for key in ('fullUploadIntervalMs', 'fullUploadIndexes', 'publishAllOnFull'):
                field(driver, '/mqttDriver', key)
            worker = section(driver, '/mqttDriver', 'fullUploadWorker',
                             {'mode': 'inline', 'eventForwardingEnabled': False})
            for key, default in WORKER_DEFAULTS.items():
                try:
                    field(worker, '/mqttDriver/fullUploadWorker', key, default, inventory=True)
                except ProjectionPending as error:
                    pending.append(str(error))
            full_topic = field(mqtt, '/mqtt', 'fullTelemetryTopic')
            if full_topic is None:
                try:
                    base = field(mqtt, '/mqtt', 'telemetryTopic', 'edge/telemetry', inventory=True)
                    base = 'edge/telemetry' if base is None else base
                    require(isinstance(base, str), 'telemetryTopic must be a string')
                    mqtt['fullTelemetryTopic'] = base if not base or base.endswith('/full') else base + '/full'
                except ProjectionPending as error:
                    pending.append(str(error))
    except ProjectionPending as error:
        # Never manufacture a value for an omitted gate to obtain a simulated PASS.
        return {'configPredicatePassed': False, 'configurationStatus': 'PENDING',
                'pendingConfigFields': sorted(set(pending + [str(error)])),
                'activationAuthorized': False, 'actualAQualified': False,
                'realtimeSessionQualified': False, 'monitorEnvironmentQualified': False}
    result = check_profile(documents, target_names, _pending_fields=pending)
    result['projectionIsFullConfiguration'] = False
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--projection', required=True, type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--redacted', action='store_true', help='documents contain preflight projection entries')
    args = parser.parse_args()
    try:
        raw = args.projection.read_bytes()
        require(hashlib.sha256(raw).hexdigest() == args.sha256, 'projection SHA256 mismatch')
        projection = json.loads(raw, object_pairs_hook=unique_object,
                                parse_constant=lambda value: require(False, 'non-finite JSON number'))
        obj(projection, 'projection')
        check = check_projected_profile if args.redacted else check_profile
        result = check(projection.get('documents'), projection.get('targetSharedMemoryNames'))
        result['projectionSha256'] = args.sha256
        print(json.dumps(result, sort_keys=True))
        return 0 if result['configPredicatePassed'] else 2
    except (ValueError, OSError, TypeError) as error:
        print(json.dumps({'configPredicatePassed': False, 'activationAuthorized': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
