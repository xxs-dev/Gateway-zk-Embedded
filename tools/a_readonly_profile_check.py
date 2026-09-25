#!/usr/bin/env python3
"""Offline config-only predicate for A RTU acquisition + MQTT TX/full preparation.

This does not authorize activation, inspect devices, or replace the C++ loader.
"""
import argparse
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


def check_profile(documents, target_names):
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
    indexes, collected = set(), set()
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
            if owner_enabled and point.get('enabled', True) and read['enable']:
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
        require(mqtt.get('enabled') is (name == 'mqtt-service.json') and
                driver.get('enabled') is (name == 'mqtt-service.json'), path + ': MQTT role mismatch')
        for key in RX_TOPICS:
            require(mqtt.get(key) == '', path + '.mqtt.' + key + ': explicit empty string required')
        require(driver.get('sharedMemoryName') in names, path + ': primary store outside target set')
        stores = array(driver.get('sharedMemoryNames'), path + '.mqttDriver.sharedMemoryNames')
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
            require(memory['sharedMemoryName'] in stores, 'MQTT must include acquisition store')
            worker = obj(driver.get('fullUploadWorker'), 'mqttDriver.fullUploadWorker')
            require(worker.get('mode') == 'inline' and worker.get('eventForwardingEnabled') is False,
                    'first narrow profile requires inline full upload without event delegation')
            interval = driver.get('fullUploadIntervalMs')
            require(type(interval) is int and interval > 0, 'positive fullUploadIntervalMs required')
            require(isinstance(mqtt.get('fullTelemetryTopic'), str) and mqtt['fullTelemetryTopic'].strip(),
                    'nonempty fullTelemetryTopic required')
            selected = array(driver.get('fullUploadIndexes'), 'mqttDriver.fullUploadIndexes')
            require(all(type(i) is int and i in collected for i in selected), 'invalid fullUploadIndexes')
            # Per-point fullUpload flags can override publishAllOnFull in the C++ resolver.
            require(type(driver.get('publishAllOnFull')) is bool and collected <= set(selected),
                    'explicit fullUploadIndexes must cover acquisition indexes')
    return {'profile': 'A_RTU_READONLY_MQTT_TX_FULL_PREPARATION', 'configPredicatePassed': True,
            'pointCount': len(indexes), 'acquisitionIndexes': sorted(collected),
            'activationAuthorized': False, 'actualAQualified': False, 'realtimeSessionQualified': False,
            'monitorEnvironmentQualified': False,
            'pending': ['actual-config-projection', 'effective-unit-and-dependency-pins',
                        'system-monitor-effective-environment',
                        'existing-activation-gates-integration', 'stopped-state-and-runtime-acceptance']}


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
    args = parser.parse_args()
    try:
        raw = args.projection.read_bytes()
        require(hashlib.sha256(raw).hexdigest() == args.sha256, 'projection SHA256 mismatch')
        projection = json.loads(raw, object_pairs_hook=unique_object,
                                parse_constant=lambda value: require(False, 'non-finite JSON number'))
        obj(projection, 'projection')
        result = check_profile(projection.get('documents'), projection.get('targetSharedMemoryNames'))
        result['projectionSha256'] = args.sha256
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ValueError, OSError, TypeError) as error:
        print(json.dumps({'configPredicatePassed': False, 'activationAuthorized': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
