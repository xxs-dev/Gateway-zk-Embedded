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
              'memoryStore': {'sharedMemoryName': target}, 'northboundServer': {'enabled': False},
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
        self.reject()
        driver['publishAllOnFull'] = False
        self.reject()
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


if __name__ == '__main__':
    unittest.main()
