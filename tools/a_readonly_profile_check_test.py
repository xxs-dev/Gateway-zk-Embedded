import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import a_readonly_profile_check as profile


def fixture():
    target = 'gateway_point_store_gw002_readonly_rtu2_v2_v11_a0925p2'
    point = {'index': 1234, 'pointCode': 'voltage', 'name': 'Voltage', 'enabled': True,
             'read': {'enable': True, 'function': 3, 'dataType': 'uint16'},
             'write': {'enable': False}}
    device = {'machineCode': 'SYNTHETIC_ONLY', 'meterCode': 'meter',
              'protocol': {'type': 'modbus_rtu', 'transport': {'serialPort': '/dev/ttySP2'}},
              'memoryStore': {'enabled': True, 'backend': 'memory', 'sharedMemoryName': target},
              'northboundServer': {'enabled': False},
              'meters': [{'meterCode': 'meter', 'enabled': True, 'points': [point]}]}
    documents = {profile.DEVICE: device}
    for name in profile.APPS:
        mqtt = {'enabled': name == 'mqtt-service.json', 'fullTelemetryTopic': 'test/full'}
        mqtt.update({key: '' for key in profile.RX_TOPICS})
        documents['apps/' + name] = {
            'deviceConfigFiles': [] if name == 'camera-service.json' else [profile.DEVICE_PATH],
            'mqtt': mqtt, 'mqttDriver': {'enabled': name == 'mqtt-service.json',
                'sharedMemoryName': target, 'sharedMemoryNames': [target], 'fullUploadIntervalMs': 1000,
                'publishAllOnFull': True, 'fullUploadIndexes': [1234],
                'fullUploadWorker': {'mode': 'inline', 'eventForwardingEnabled': False}},
            'computeEngine': {'enabled': False}, 'cameraService': {'enabled': False},
            'ota': {'enabled': False}, 'emsCluster': {'enabled': False, 'controlEnabled': False},
            'localDisplay': {'enabled': False, 'sharedMemoryNames': [target]},
            'systemMonitor': {'enabled': name == 'monitor-service.json',
                              'directMaintenance': {'enabled': False}}}
    return documents, [target]


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.documents, self.targets = fixture()
        self.device = self.documents[profile.DEVICE]
        self.point = self.device['meters'][0]['points'][0]
        self.mqtt = self.documents['apps/mqtt-service.json']

    def check(self):
        return profile.check_profile(self.documents, self.targets)

    def reject(self):
        with self.assertRaises(ValueError):
            self.check()

    def test_valid_config_is_never_activation_authority(self):
        result = self.check()
        self.assertEqual([1234], result['acquisitionIndexes'])
        for key in ('activationAuthorized', 'actualAQualified', 'realtimeSessionQualified'):
            self.assertIs(result[key], False)

    def test_every_rx_topic_requires_explicit_empty_string(self):
        for key in profile.RX_TOPICS:
            for value in ('test/request', None, False, 0):
                with self.subTest(key=key, value=value):
                    self.mqtt['mqtt'][key] = value
                    self.reject()
            del self.mqtt['mqtt'][key]
            self.reject()
            self.mqtt['mqtt'][key] = ''
        self.check()

    def test_disabled_point_write_still_refused(self):
        self.point.update(enabled=False)
        self.point['write']['enable'] = True
        self.reject()

    def test_disabled_meter_write_still_refused(self):
        self.device['meters'][0]['enabled'] = False
        self.point['write']['enable'] = True
        self.reject()

    def test_top_level_and_all_point_group_aliases(self):
        self.device['points'] = [copy.deepcopy(self.point)]
        self.device['points'][0]['index'] = 2000
        self.device['points'][0]['read']['enable'] = False
        self.check()
        self.device['points'][0]['write']['enable'] = True
        self.reject()
        del self.device['points']
        for owner in (self.device, self.device['meters'][0]):
            for key in profile.POINT_GROUPS:
                for wrapped in (False, True):
                    with self.subTest(key=key, wrapped=wrapped):
                        point = copy.deepcopy(self.point)
                        point['index'] = 2000
                        point['read']['enable'] = False
                        group = {'points': [point]} if wrapped else [point]
                        owner[key] = {'yk': group}
                        self.check()
                        point['write']['enable'] = True
                        self.reject()
                        del owner[key]

    def test_direct_array_group(self):
        point = copy.deepcopy(self.point)
        point['index'] = 2000
        point['read']['enable'] = False
        self.device['fourRemote'] = [point]
        self.check()
        point['initialValue'] = 0
        self.reject()

    def test_bad_function_even_on_disabled_point(self):
        for value in (5, 6, 15, 16, 0, True, '3'):
            with self.subTest(value=value):
                self.point['read']['function'] = value
                self.reject()

    def test_initial_zero_startup_zero_and_retention_refused(self):
        self.point['initialValue'] = 0
        self.reject()
        self.point['initialValue'] = None
        self.point['write']['startupValue'] = 0
        self.reject()
        self.point['write']['startupValue'] = None
        self.point['retain'] = True
        self.reject()

    def test_startup_writes_refused_even_disabled(self):
        self.device['startupWrites'] = [{'enabled': False, 'pointCode': 'voltage', 'value': 0}]
        self.reject()

    def test_virtual_protocol_refused(self):
        self.device['protocol']['type'] = 'ems_virtual'
        self.reject()

    def test_server_alias_cannot_override_disabled_northbound(self):
        self.device['server'] = {'enabled': True}
        self.reject()

    def test_forward_alias_refused(self):
        self.point['forward'] = {'enabled': True, 'writeEnabled': True}
        self.reject()

    def test_extra_sibling_or_device_file_refused(self):
        for path in ('apps/agc-avc-service.json', 'devices/virtual.json'):
            self.documents[path] = {}
            self.reject()
            del self.documents[path]
        self.mqtt['deviceConfigFiles'].append('/opt/modbus-gateway/config/runtime/devices/virtual.json')
        self.reject()

    def test_producer_control_or_ota_refused(self):
        for key in ('computeEngine', 'cameraService', 'ota', 'emsCluster'):
            self.mqtt[key]['enabled'] = True
            self.reject()
            self.mqtt[key]['enabled'] = False
        self.mqtt['emsCluster']['controlEnabled'] = True
        self.reject()

    def test_mqtt_inline_full_only(self):
        driver = self.mqtt['mqttDriver']
        driver['fullUploadWorker']['mode'] = 'isolated'
        self.reject()
        driver['fullUploadWorker']['mode'] = 'inline'
        driver['fullUploadWorker']['eventForwardingEnabled'] = True
        self.reject()
        driver['fullUploadWorker']['eventForwardingEnabled'] = False
        driver['fullUploadIntervalMs'] = 0
        self.reject()

    def test_full_index_coverage(self):
        driver = self.mqtt['mqttDriver']
        driver['fullUploadIndexes'] = []
        self.check()
        driver['publishAllOnFull'] = False
        self.check()
        driver['fullUploadIndexes'] = [1234]
        self.check()
        driver['fullUploadIndexes'] = [True]
        self.reject()

    def test_malformed_missing_and_duplicate_points(self):
        for bad in (None, 'points', {}):
            self.device['meters'][0]['points'] = bad
            self.reject()
        self.device['meters'][0]['points'] = [self.point, self.point]
        self.reject()
        self.device['meters'][0]['points'] = [self.point]
        self.point['index'] = True
        self.reject()

    def test_no_acquisition_point(self):
        self.point['read']['enable'] = False
        self.reject()

    def test_outside_target_and_bad_target(self):
        self.targets[:] = ['wrong_target']
        self.reject()
        self.targets[:] = [True]
        self.reject()

    def test_memory_store_enabled_backend_and_loader_defaults(self):
        memory = self.device['memoryStore']
        for value in (False, 0, 1, 'true'):
            with self.subTest(enabled=value):
                memory['enabled'] = value
                with self.assertRaisesRegex(ValueError, 'memoryStore.enabled'):
                    self.check()
        for value in (True, None):
            memory['enabled'] = value
            self.check()
        del memory['enabled']
        self.check()
        for value in (False, [], {}, 1):
            with self.subTest(backend=value):
                memory['backend'] = value
                with self.assertRaisesRegex(ValueError, 'memoryStore.backend'):
                    self.check()
        for value in ('memory', 'shared_memory', 'sqlite', 'private', '', None):
            memory['backend'] = value
            self.check()
        del memory['backend']
        self.check()

    def test_reader_store_sources_and_unproven_monitor_environment(self):
        for name in profile.APPS:
            app = self.documents['apps/' + name]
            display = app['localDisplay']
            for enabled in (False, True):
                display['enabled'] = enabled
                for names in (['unapproved_store'], [False], 'bad', {}):
                    with self.subTest(app=name, enabled=enabled, names=names):
                        display['sharedMemoryNames'] = names
                        with self.assertRaisesRegex(ValueError, 'localDisplay.sharedMemoryNames'):
                            self.check()
                for names in ([self.targets[0]], [], None, ['']):
                    display['sharedMemoryNames'] = names
                    self.check()
            del display['sharedMemoryNames']
            self.check()
            app['localDisplay'] = None
            self.check()
            del app['localDisplay']
            self.check()
            driver = app['mqttDriver']
            for key, bad in (('sharedMemoryName', 'unapproved_store'),
                             ('sharedMemoryNames', ['unapproved_store'])):
                original = driver[key]
                driver[key] = bad
                if name == 'camera-service.json':
                    self.check()  # Disabled sibling does not open its driver stores.
                else:
                    self.reject()
                driver[key] = original
        original = self.device['memoryStore']['sharedMemoryName']
        self.device['memoryStore']['sharedMemoryName'] = 'unapproved_store'
        self.reject()
        self.device['memoryStore']['sharedMemoryName'] = original
        monitor = self.documents['apps/monitor-service.json']
        monitor['systemMonitor']['sharedMemoryName'] = self.targets[0]
        result = self.check()
        self.assertIs(result['monitorEnvironmentQualified'], False)
        self.assertIn('system-monitor-effective-environment', result['pending'])

    def test_agc_mailbox_configuration_refused_even_disabled(self):
        self.mqtt['agcAvc'] = {'enabled': False}
        self.reject()

    def test_cli_pins_and_duplicate_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'synthetic.json'
            path.write_text(json.dumps({'documents': self.documents,
                                       'targetSharedMemoryNames': self.targets}))
            command = [sys.executable, '-B', str(Path(profile.__file__)), '--projection', str(path), '--sha256']
            good = subprocess.run(command + [hashlib.sha256(path.read_bytes()).hexdigest()],
                                  capture_output=True, text=True)
            self.assertEqual(0, good.returncode, good.stdout + good.stderr)
            self.assertFalse(json.loads(good.stdout)['activationAuthorized'])
            bad = subprocess.run(command + ['0' * 64], capture_output=True, text=True)
            self.assertEqual(1, bad.returncode)
            path.write_text('{"documents":{},"documents":{}}')
            bad = subprocess.run(command + [hashlib.sha256(path.read_bytes()).hexdigest()],
                                 capture_output=True, text=True)
            self.assertEqual(1, bad.returncode)
            self.assertIn('duplicate JSON key', bad.stdout)


class ActualProjectionTest(unittest.TestCase):
    def setUp(self):
        self.evidence = json.loads((Path(__file__).parent / 'fixtures' /
                                   'a-profile-preflight-20260925-01.json').read_text(encoding='utf-8'))
        self.entries = self.evidence['documents']
        self.mqtt_name = 'apps/mqtt-service.json'
        self.monitor_name = 'apps/monitor-service.json'
        self.mqtt = self.entries[self.mqtt_name]['projection']['document']
        self.monitor = self.entries[self.monitor_name]['projection']['document']
        self.device = self.entries[profile.DEVICE]['projection']['document']
        # Observed SOURCE names are diagnostic only, not reviewed live remap targets.
        self.names = self.mqtt['mqttDriver']['sharedMemoryNames'] + [self.device['memoryStore']['sharedMemoryName']]
        self.expected_pending = [self.mqtt_name + '#/mqtt/telemetryTopic'] + [
            self.mqtt_name + '#/mqttDriver/fullUploadWorker/' + key for key in profile.WORKER_DEFAULTS]

    def repin(self):
        for entry in self.entries.values():
            raw = (json.dumps(entry['projection'], sort_keys=True, separators=(',', ':'),
                              allow_nan=False) + '\n').encode()
            entry['projectionSha256'] = hashlib.sha256(raw).hexdigest()

    def set_field(self, name, pointer, key, value=None, state='present'):
        projection = self.entries[name]['projection']
        parent = projection['document']
        for part in pointer.split('/')[1:]:
            parent = parent[int(part)] if isinstance(parent, list) else parent[part]
        parent.pop(key, None)
        for kind in ('missingFields', 'unknownKeys'):
            keys = projection['fieldMetadata'][kind].setdefault(pointer, [])
            if key in keys:
                keys.remove(key)
        if state == 'present':
            parent[key] = value
        else:
            projection['fieldMetadata'][state][pointer].append(key)

    def candidate(self):
        self.set_field(self.monitor_name, '/mqtt', 'enabled', False)
        self.set_field(self.monitor_name, '/systemMonitor/directMaintenance', 'enabled', False)
        for key in profile.RX_TOPICS:
            self.set_field(self.mqtt_name, '/mqtt', key, '')
        self.repin()

    def supplement(self):
        # Synthetic public values for boundary tests, never claimed as actual device evidence.
        self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', 'synthetic/telemetry')
        for key, value in profile.WORKER_DEFAULTS.items():
            self.set_field(self.mqtt_name, '/mqttDriver/fullUploadWorker', key, value)
        self.repin()

    def actual_supplement(self):
        supplement = json.loads((Path(__file__).parent / 'fixtures' /
                                 'a-unit-env-public-supplement-20260925-01.json').read_text(encoding='utf-8'))
        self.assertEqual(self.evidence['sourceReportSha256'], supplement['priorConfigReportSha256'])
        self.assertFalse(supplement['fieldsComplete'])
        for name, pin in supplement['configHashes'].items():
            self.assertEqual(pin['fullFileSha256'], self.entries[name]['fullFileSha256'])
            self.assertEqual(pin['fullFileBytes'], self.entries[name]['fullFileBytes'])
        public = supplement['mqttPublicSupplement']
        self.assertEqual(self.entries[self.mqtt_name]['fullFileSha256'], public['fullFileSha256'])
        raw = (json.dumps(public['projection'], sort_keys=True, separators=(',', ':'),
                          allow_nan=False) + '\n').encode()
        self.assertEqual(public['projectionSha256'], hashlib.sha256(raw).hexdigest())
        document = public['projection']['document']
        self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', document['mqtt']['telemetryTopic'])
        for key, value in document['mqttDriver']['fullUploadWorker'].items():
            self.set_field(self.mqtt_name, '/mqttDriver/fullUploadWorker', key, value)
        self.assertEqual(['realtimeTelemetryTopic'], public['projection']['fieldMetadata']['missingFields']['/mqtt'])
        self.set_field(self.mqtt_name, '/mqtt', 'realtimeTelemetryTopic', state='missingFields')
        self.repin()

    def check(self):
        return profile.check_projected_profile(self.entries, self.names)

    def test_exact_evidence_hashes_and_original_monitor_rx_refused(self):
        original = copy.deepcopy(self.entries)
        self.repin()
        self.assertEqual(original, self.entries)
        self.assertFalse(self.evidence['snapshotComplete'])
        with self.assertRaisesRegex(ValueError, 'directMaintenance.enabled'):
            self.check()

    def test_minimal_candidate_preserves_actual_values_and_five_unknowns(self):
        device = copy.deepcopy(self.device)
        camera = copy.deepcopy(self.entries['apps/camera-service.json'])
        driver = copy.deepcopy(self.mqtt['mqttDriver'])
        self.candidate()
        before = copy.deepcopy(self.entries)
        result = self.check()
        self.assertEqual(before, self.entries)
        self.assertEqual(camera, self.entries['apps/camera-service.json'])
        self.assertEqual(device, self.device)
        self.assertEqual(driver, self.mqtt['mqttDriver'])
        self.assertEqual(5000, driver['fullUploadIntervalMs'])
        self.assertEqual('inline', driver['fullUploadWorker']['mode'])
        self.assertEqual([], driver['fullUploadIndexes'])
        self.assertEqual('PENDING', result['configurationStatus'])
        self.assertFalse(result['configPredicatePassed'])
        self.assertEqual(sorted(self.expected_pending), result['pendingConfigFields'])
        self.assertEqual(151, result['pointCount'])
        self.assertEqual(list(range(4500, 4651)), result['acquisitionIndexes'])
        self.assertEqual(list(range(4500, 4651)), result['effectiveFullUploadIndexes'])

    def test_synthetic_supplement_pass_never_authorizes_activation(self):
        self.candidate()
        self.supplement()
        result = self.check()
        self.assertTrue(result['configPredicatePassed'])
        for key in ('activationAuthorized', 'actualAQualified', 'realtimeSessionQualified',
                    'monitorEnvironmentQualified', 'projectionIsFullConfiguration'):
            self.assertIs(result[key], False)

    def test_actual_public_supplement_only_proves_candidate_config(self):
        self.actual_supplement()
        with self.assertRaisesRegex(ValueError, 'directMaintenance.enabled'):
            self.check()
        self.candidate()
        result = self.check()
        self.assertTrue(result['configPredicatePassed'])
        self.assertEqual([], result['pendingConfigFields'])
        self.assertEqual('edge/telemetry/full', result['effectiveFullTelemetryTopic'])
        self.assertEqual(list(range(4500, 4651)), result['effectiveFullUploadIndexes'])
        self.assertEqual(5000, self.mqtt['mqttDriver']['fullUploadIntervalMs'])
        self.assertFalse(result['activationAuthorized'])
        self.assertFalse(result['actualAQualified'])
        self.assertFalse(result['monitorEnvironmentQualified'])
        self.assertIn('effective-unit-and-dependency-pins', result['pending'])
        self.assertIn('system-monitor-effective-environment', result['pending'])

    def test_legacy_null_key_is_presence_not_absence(self):
        self.candidate()
        self.set_field(self.mqtt_name, '/systemMonitor', 'listenHost', None)
        self.repin()
        with self.assertRaisesRegex(ValueError, 'legacy directMaintenance'):
            self.check()

    def test_proven_missing_worker_and_topic_defaults_only(self):
        self.candidate()
        for key in profile.WORKER_DEFAULTS:
            self.set_field(self.mqtt_name, '/mqttDriver/fullUploadWorker', key, state='missingFields')
        self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', state='missingFields')
        self.repin()
        self.assertTrue(self.check()['configPredicatePassed'])
        self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', state='unknownKeys')
        self.repin()
        self.assertEqual([self.expected_pending[0]], self.check()['pendingConfigFields'])

    def test_no_metadata_is_not_absence(self):
        self.candidate()
        meta = self.entries['apps/camera-service.json']['projection']['fieldMetadata']
        meta['missingFields'][''].remove('computeEngine')
        self.repin()
        self.assertEqual(['apps/camera-service.json#/computeEngine'], self.check()['pendingConfigFields'])

    def test_omitted_safety_fields_remain_pending(self):
        self.candidate()
        saved = copy.deepcopy(self.entries)
        for name, pointer, key in (
            ('apps/camera-service.json', '', 'computeEngine'),
            (profile.DEVICE, '/meters/0/points/0', 'initialValue'),
            (profile.DEVICE, '/meters/0/points/0', 'retain'),
            (profile.DEVICE, '/meters/0/points/0/write', 'startupValue'),
            (profile.DEVICE, '', 'server'),
            (self.mqtt_name, '/localDisplay', 'sharedMemoryNames'),
            (self.mqtt_name, '/systemMonitor', 'listenHost')):
            with self.subTest(pointer=pointer, key=key):
                self.entries = copy.deepcopy(saved)
                self.set_field(name, pointer, key, state='unknownKeys')
                self.repin()
                result = self.check()
                self.assertFalse(result['configPredicatePassed'])
                self.assertIn(name + '#' + pointer + '/' + key, result['pendingConfigFields'])

    def test_missing_or_nonempty_rx_refused_with_pending_supplement(self):
        self.candidate()
        for key in profile.RX_TOPICS:
            for value, state in ((None, 'missingFields'), (None, 'present'), ('request', 'present')):
                with self.subTest(key=key, state=state, value=value):
                    self.set_field(self.mqtt_name, '/mqtt', key, value, state)
                    self.repin()
                    with self.assertRaisesRegex(ValueError, 'explicit empty string'):
                        self.check()
            self.set_field(self.mqtt_name, '/mqtt', key, '')

    def test_worker_bounds_even_inline(self):
        self.candidate()
        self.supplement()
        for key, values in (('healthHeartbeatMs', [99, 60001, True, '1000']),
                            ('failoverTimeoutMs', [-1, 1999, 300001]),
                            ('retryMinMs', [99, 60001]), ('retryMaxMs', [-1, 499, 300001])):
            for value in values:
                with self.subTest(key=key, value=value):
                    self.set_field(self.mqtt_name, '/mqttDriver/fullUploadWorker', key, value)
                    self.repin()
                    with self.assertRaisesRegex(ValueError, 'full worker timing'):
                        self.check()
            self.set_field(self.mqtt_name, '/mqttDriver/fullUploadWorker', key, profile.WORKER_DEFAULTS[key])

    def test_empty_or_invalid_derived_full_topic_refused(self):
        self.candidate()
        self.supplement()
        for value in ('', False, 7):
            self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', value)
            self.repin()
            with self.assertRaises(ValueError):
                self.check()
        for value in ('synthetic/full', 'synthetic', None):
            self.set_field(self.mqtt_name, '/mqtt', 'telemetryTopic', value)
            self.repin()
            self.assertTrue(self.check()['configPredicatePassed'])

    def test_partial_point_flags_require_full_coverage(self):
        self.candidate()
        self.device['meters'][0]['points'][0]['fullUpload'] = False
        self.repin()
        with self.assertRaisesRegex(ValueError, 'effective full upload'):
            self.check()
        self.mqtt['mqttDriver']['publishAllOnFull'] = True
        self.repin()
        with self.assertRaisesRegex(ValueError, 'effective full upload'):
            self.check()
        self.mqtt['mqttDriver']['fullUploadIndexes'] = [4500]
        self.repin()
        self.assertEqual(151, len(self.check()['effectiveFullUploadIndexes']))

    def test_projection_hash_metadata_conflicts_and_invalid_types_refused(self):
        self.candidate()
        entry = self.entries[self.mqtt_name]
        saved = copy.deepcopy(entry)
        self.mqtt['mqtt']['enabled'] = False
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            self.check()
        self.entries[self.mqtt_name] = copy.deepcopy(saved)
        self.entries[self.mqtt_name]['projection']['fieldMetadata']['invalidTypes'] = {'/mqtt/enabled': 'string'}
        self.repin()
        with self.assertRaisesRegex(ValueError, 'invalid projected types'):
            self.check()
        self.entries[self.mqtt_name] = copy.deepcopy(saved)
        self.entries[self.mqtt_name]['projection']['fieldMetadata']['unknownKeys']['/mqtt'].append('enabled')
        self.repin()
        with self.assertRaisesRegex(ValueError, 'inconsistent metadata'):
            self.check()

    def test_redacted_cli_pending_exit_code(self):
        self.candidate()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'simulation-only.json'
            path.write_text(json.dumps({'documents': self.entries, 'targetSharedMemoryNames': self.names}))
            result = subprocess.run([sys.executable, '-B', str(Path(profile.__file__)), '--redacted',
                                     '--projection', str(path), '--sha256',
                                     hashlib.sha256(path.read_bytes()).hexdigest()], capture_output=True, text=True)
            self.assertEqual(2, result.returncode, result.stdout + result.stderr)
            self.assertEqual('PENDING', json.loads(result.stdout)['configurationStatus'])


if __name__ == '__main__':
    unittest.main()
