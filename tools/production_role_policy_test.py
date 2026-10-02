#!/usr/bin/env python3
"""Focused, file-backed role-policy tests; no production service is started."""

import hashlib
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('production_role_policy',
        str(ROOT / 'deploy/production_role_policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
TARGET = '/opt/modbus-gateway/config/runtime/'


class ProductionRolePolicyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-role-policy-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'runtime'
        self.root.mkdir()
        self.templates = ROOT / 'deploy'
        self.trust = Path(self.temp.name) / 'ota.pub'
        self.trust.write_bytes(b'fixture-public-key\n')
        self.write('device_identity.json', {'machineCode': 'SIM_23'})
        self.write('apps/mqtt-service.json', {
            'runtimeMode': 'gateway', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [TARGET + 'devices/meter_1.json'],
            'mqtt': {'enabled': True},
            'mqttDriver': {'enabled': True, 'sharedMemoryNames': ['store_1']},
            'eventEngine': {'enabled': False}, 'computeEngine': {'enabled': False},
            'mqttForward': {'enabled': False}, 'emsCluster': {'enabled': False},
            'ota': {'enabled': False}, 'realtime': {'enabled': False}})
        self.write('apps/monitor-service.json', {
            'runtimeMode': 'gateway', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [TARGET + 'devices/meter_1.json'],
            'systemMonitor': {'enabled': True, 'cellular': {'routeFailover': {'enabled': False}}},
            'localDisplay': {'enabled': False}})
        self.write('devices/meter_1.json', {
            'enabled': True, 'protocol': {'type': 'modbus_tcp', 'tcp': {'host': '127.0.0.1', 'port': 1502}},
            'memoryStore': {'sharedMemoryName': 'store_1'}})
        self.bindings = {'meter_1': {'kind': 'tcp-simulator', 'endpoint': '127.0.0.1:1502'}}

    def write(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, sort_keys=True), encoding='utf-8')

    def read(self, name):
        return json.loads((self.root / name).read_text(encoding='utf-8'))

    def authority(self):
        return {'machineCode': 'SIM_23',
                'identitySha256': hashlib.sha256((self.root / 'device_identity.json').read_bytes()).hexdigest(),
                'networkSha256': {}, 'tlsSha256': {},
                'otaPublicKeySha256': hashlib.sha256(self.trust.read_bytes()).hexdigest(),
                'monitorShmName': 'monitor_store'}

    def resolve(self, **overrides):
        options = {'simulator_bindings': self.bindings}
        authority = overrides.pop('authority', self.authority())
        options.update(overrides)
        return policy.resolve(self.root, self.templates, authority, self.trust, **options)

    def rejects(self, phrase, **overrides):
        before = {p.relative_to(self.root).as_posix(): p.read_bytes()
                  for p in self.root.rglob('*') if p.is_file() and not p.is_symlink()}
        with self.assertRaisesRegex(policy.RolePolicyError, phrase):
            self.resolve(**overrides)
        after = {p.relative_to(self.root).as_posix(): p.read_bytes()
                 for p in self.root.rglob('*') if p.is_file() and not p.is_symlink()}
        self.assertEqual(before, after)

    def test_exact_roles_templates_order_and_tools_are_not_services(self):
        result = self.resolve(watchdog=True)
        self.assertEqual(result['format'], 'configured-role-policy-v2')
        roles = {r['unit']: r for r in result['roles']}
        self.assertEqual(set(roles), {'modbus-rtu@meter_1.service', 'mqtt-driver@mqtt-service.service',
                'system-monitor@monitor-service.service', 'gateway-services.service',
                'gateway-health-watchdog.service'})
        self.assertEqual(roles['modbus-rtu@meter_1.service']['shm'],
                         {'policy': 'required', 'names': ['store_1'],
                          'required': ['store_1'], 'optional': []})
        self.assertEqual(roles['system-monitor@monitor-service.service']['shm']['required'], ['monitor_store'])
        self.assertEqual(roles['system-monitor@monitor-service.service']['shm']['optional'],
                         ['gateway_point_store', 'store_1'])
        self.assertEqual(roles['mqtt-driver@mqtt-service.service']['executable'], 'bin/MqttDriver')
        self.assertEqual(roles['mqtt-driver@mqtt-service.service']['configPaths'],
                         ['apps/mqtt-service.json', 'device_identity.json', 'devices/meter_1.json'])
        self.assertEqual([r['startPhase'] for r in result['roles']], sorted(r['startPhase'] for r in result['roles']))
        self.assertEqual(result['tools']['pointctl'], 'bin/pointctl')
        self.assertFalse(any('pointctl' in name or 'EmsParityCheck' in name for name in roles))
        self.assertEqual(result['resourcePolicy']['unit'], 'MiB')
        self.assertEqual(roles['gateway-services.service']['runtimeFiles'], ['bin/gateway-services.sh'])

    def test_real_template_mismatch_rejected(self):
        private = Path(self.temp.name) / 'templates'
        shutil.copytree(self.templates, private, ignore=shutil.ignore_patterns('__pycache__'))
        path = private / 'mqtt-driver@.service'
        path.write_text(path.read_text().replace('bin/MqttDriver', 'bin/OtherDriver'))
        with self.assertRaisesRegex(policy.RolePolicyError, 'ExecStart differs'):
            policy.resolve(self.root, private, self.authority(), self.trust,
                           simulator_bindings=self.bindings)

    def test_entire_catalog_matches_real_unit_templates(self):
        for family in policy.CATALOG:
            with self.subTest(family=family):
                self.assertRegex(policy._template(self.templates, family), r'^[0-9a-f]{64}$')

    def test_duplicate_json_field_rejected(self):
        (self.root / 'apps/mqtt-service.json').write_text('{"enabled":true,"enabled":false}')
        self.rejects('duplicate JSON field')

    def test_unknown_top_level_role_and_protocol_rejected(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['newDriver'] = {'enabled': True}
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('unknown app field')
        mqtt.pop('newDriver')
        self.write('apps/mqtt-service.json', mqtt)
        device = self.read('devices/meter_1.json')
        device['protocol']['type'] = 'unknown_protocol'
        self.write('devices/meter_1.json', device)
        self.rejects('unknown device protocol')

    def test_runtime_modes_match_native_app_parser(self):
        for name in ('apps/mqtt-service.json', 'apps/monitor-service.json'):
            with self.subTest(name=name):
                app = self.read(name)
                app['runtimeMode'] = 'standard'
                self.write(name, app)
                self.rejects('unknown app runtime mode')
                app['runtimeMode'] = 'gateway'
                self.write(name, app)
        self.assertEqual(self.resolve()['format'], 'configured-role-policy-v2')

    def test_noncanonical_protocol_cannot_select_different_driver_transport(self):
        device = self.read('devices/meter_1.json')
        device['protocol']['type'] = 'MODBUS_TCP'
        self.write('devices/meter_1.json', device)
        self.rejects('noncanonical device protocol')

    def test_duplicate_and_unselected_inputs_rejected(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['deviceConfigFiles'].append(mqtt['deviceConfigFiles'][0])
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('duplicate/malformed device')
        mqtt['deviceConfigFiles'].pop()
        self.write('apps/mqtt-service.json', mqtt)
        self.write('apps/other-service.json', {'mqtt': {'enabled': True}})
        self.rejects('unselected app')

    def test_path_escape_and_symlink_rejected_before_roles(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['deviceConfigFiles'] = [TARGET + 'devices/../device_identity.json']
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('unsafe instance name')
        mqtt['deviceConfigFiles'] = [TARGET + 'devices/meter_1.json']
        self.write('apps/mqtt-service.json', mqtt)
        link = self.root / 'logic' / 'foreign.json'
        link.parent.mkdir()
        try:
            link.symlink_to(self.root / 'device_identity.json')
        except (OSError, NotImplementedError):
            self.skipTest('symlinks unavailable')
        self.rejects('symlink in configuration')

    def test_graph_and_tls_references_cannot_escape_pinned_configuration(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['computeEngine'] = {'enabled': False,
                                 'script': {'graphFile': 'runtime/logic/../outside.json'}}
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('graph reference outside pinned logic')
        mqtt['computeEngine']['script']['graphFile'] = 'runtime/logic/graph.json'
        self.write('logic/graph.json', {'nodes': []})
        self.write('apps/mqtt-service.json', mqtt)
        self.resolve()
        mqtt['mqtt']['tls'] = {'caFile': '/tmp/unpinned.crt'}
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('TLS reference outside pinned trust files')

    def test_physical_serial_and_unreviewed_binding_rejected(self):
        device = self.read('devices/meter_1.json')
        device['protocol'] = {'type': 'modbus_rtu', 'transport': {'serialPort': '/dev/ttySP1'}}
        self.write('devices/meter_1.json', device)
        self.rejects('serial role', simulator_bindings={
            'meter_1': {'kind': 'serial-simulator', 'endpoint': '/dev/ttySP1'}})
        device['protocol']['transport']['serialPort'] = '/dev/pts/7'
        self.write('devices/meter_1.json', device)
        roles = self.resolve(simulator_bindings={
            'meter_1': {'kind': 'serial-simulator', 'endpoint': '/dev/pts/7'}})['roles']
        self.assertIn('modbus-rtu@meter_1.service', [r['unit'] for r in roles])

    def test_unqualified_gpio_and_enabled_unreferenced_device_rejected(self):
        device = self.read('devices/meter_1.json')
        device['protocol'] = {'type': 'local_dio', 'backend': 'sysfs_gpio', 'gpioBasePath': '/sys/class/gpio'}
        self.write('devices/meter_1.json', device)
        self.rejects('no qualified simulator transport', simulator_bindings={
            'meter_1': {'kind': 'dio-simulator', 'endpoint': 'mock'}})
        self.write('devices/meter_1.json', {'enabled': False, 'protocol': device['protocol']})
        self.write('devices/other.json', {'enabled': True, 'protocol': {'type': 'modbus_tcp'}})
        self.rejects('enabled unreferenced device', simulator_bindings={})

    def test_identity_network_tls_and_trust_pins_rejected(self):
        authority = self.authority()
        self.write('device_identity.json', {'machineCode': 'OTHER'})
        with self.assertRaisesRegex(policy.RolePolicyError, 'protected identity changed'):
            policy.resolve(self.root, self.templates, authority, self.trust,
                           simulator_bindings=self.bindings)
        self.write('device_identity.json', {'machineCode': 'SIM_23'})
        self.write('network/route.json', {'enabled': True})
        self.rejects('protected network/trust configuration changed')
        (self.root / 'network/route.json').unlink()
        (self.root / 'tls').mkdir(exist_ok=True)
        (self.root / 'tls/ca.crt').write_bytes(b'candidate trust')
        self.rejects('protected network/trust configuration changed')
        (self.root / 'tls/ca.crt').unlink()
        authority = self.authority()
        self.trust.write_bytes(b'changed-public-key')
        self.rejects('OTA trust pin changed', authority=authority)

    def test_app_client_identity_change_rejected(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['mqtt']['clientId'] = 'OTHER_MACHINE'
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('app MQTT identity changed')

    def test_route_failover_and_unknown_capability_rejected(self):
        monitor = self.read('apps/monitor-service.json')
        monitor['systemMonitor']['cellular']['routeFailover']['enabled'] = True
        self.write('apps/monitor-service.json', monitor)
        self.rejects('network route/failover role is protected')
        self.rejects('unknown/duplicate capability', capabilities=['physical-actuator'])

    def test_missing_required_shm_and_implicit_shm_key_rejected(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['mqttDriver']['sharedMemoryNames'] = []
        mqtt['mqttDriver']['sharedMemoryName'] = ''
        mqtt['deviceConfigFiles'] = []
        self.write('apps/mqtt-service.json', mqtt)
        monitor = self.read('apps/monitor-service.json')
        monitor['deviceConfigFiles'] = []
        self.write('apps/monitor-service.json', monitor)
        device = self.read('devices/meter_1.json')
        device['enabled'] = False
        self.write('devices/meter_1.json', device)
        self.bindings = {}
        self.rejects('required SHM role lacks exact names')
        mqtt['mqttDriver']['mysterySharedMemoryName'] = 'secret_store'
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('unknown SHM key')

    def test_qt_dependency_real_executable_and_resource_floor(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['localDisplay'] = {'sharedMemoryNames': ['store_1']}
        self.write('apps/mqtt-service.json', mqtt)
        monitor = self.read('apps/monitor-service.json')
        monitor['localDisplay'] = {'enabled': True, 'renderer': 'nativeqt',
                                   'sharedMemoryNames': ['store_1']}
        self.write('apps/monitor-service.json', monitor)
        self.rejects('display requires', capabilities=[])
        result = self.resolve(capabilities=['display-simulator'])
        roles = {r['unit']: r for r in result['roles']}
        self.assertEqual(roles['ky-ems.service']['dependencies'], ['qt-display-bridge.service'])
        self.assertEqual(roles['ky-ems.service']['executable'], 'ky-ems/KY-EMS')
        self.assertIn('gateway-qt-run.sh', roles['ky-ems.service']['execStart'])
        self.assertEqual(roles['ky-ems.service']['runtimeFiles'],
                         ['bin/gateway-qt-run.sh', 'ky-ems/KY-EMS'])
        self.assertEqual(roles['qt-display-bridge.service']['reserveMiB'], 64)

    def test_agc_requires_simulated_control(self):
        self.write('apps/agc-avc-service.json', {
            'runtimeMode': 'agc_avc', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [], 'agcAvc': {'enabled': True,
                                                 'shadowMode': True, 'submitWrites': False,
                                                 'sharedMemoryNames': ['agc_store']}})
        self.rejects('AGC/AVC requires', capabilities=[])
        roles = self.resolve(capabilities=['control-simulator'])['roles']
        self.assertIn('agc-avc@agc-avc-service.service', [r['unit'] for r in roles])

    def test_agc_output_must_remain_shadow_only(self):
        self.write('apps/agc-avc-service.json', {
            'runtimeMode': 'agc_avc', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [], 'agcAvc': {'enabled': True, 'shadowMode': False,
                                                 'submitWrites': True,
                                                 'sharedMemoryNames': ['agc_store']}})
        self.rejects('physical output must remain disabled', capabilities=['control-simulator'])

    def test_event_forwarder_and_compute_open_exact_store_set(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['eventEngine'] = {'enabled': True, 'publishMode': 'mqtt_driver_outbox'}
        mqtt['mqttForward'] = {'enabled': True}
        mqtt['computeEngine'] = {'enabled': True, 'sharedMemoryNames': ['compute_store']}
        self.write('apps/mqtt-service.json', mqtt)
        roles = {r['unit']: r for r in self.resolve()['roles']}
        self.assertEqual(roles['event-engine@mqtt-service.service']['shm']['required'],
                         ['gateway_point_store', 'store_1'])
        self.assertEqual(roles['mqtt-forwarder@mqtt-service.service']['shm']['required'], ['store_1'])
        self.assertEqual(roles['compute-engine@mqtt-service.service']['shm']['required'],
                         ['compute_store', 'gateway_point_store',
                          'gateway_point_store_compute', 'store_1'])
        self.assertEqual(roles['system-monitor@monitor-service.service']['shm']['optional'],
                         ['compute_store', 'gateway_point_store',
                          'gateway_point_store_compute', 'store_1'])

    def test_role_specific_shm_defaults_and_explicit_overrides(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['eventEngine'] = {'enabled': True}
        mqtt['mqttForward'] = {'enabled': True}
        mqtt['computeEngine'] = {'enabled': True, 'sharedMemoryNames': ['compute_store'],
                                 'rules': [{'outputs': [{'sharedMemoryName': 'rule_store'}]}]}
        mqtt['cameraService'] = {'sharedMemoryName': 'camera_store'}
        mqtt['mqttDriver']['sharedMemoryName'] = 'primary_store'
        self.write('apps/mqtt-service.json', mqtt)
        roles = {r['unit']: r for r in self.resolve()['roles']}
        self.assertEqual(roles['event-engine@mqtt-service.service']['shm']['required'],
                         ['camera_store', 'store_1'])
        self.assertEqual(roles['mqtt-driver@mqtt-service.service']['shm']['required'], ['store_1'])
        self.assertEqual(roles['mqtt-forwarder@mqtt-service.service']['shm']['required'], ['store_1'])
        self.assertEqual(roles['compute-engine@mqtt-service.service']['shm']['required'],
                         ['camera_store', 'compute_store', 'gateway_point_store_compute',
                          'primary_store', 'rule_store', 'store_1'])
        mqtt['mqttDriver'].pop('sharedMemoryNames')
        self.write('apps/mqtt-service.json', mqtt)
        roles = {r['unit']: r for r in self.resolve()['roles']}
        self.assertEqual(roles['event-engine@mqtt-service.service']['shm']['required'],
                         ['camera_store', 'primary_store', 'store_1'])
        self.assertEqual(roles['mqtt-forwarder@mqtt-service.service']['shm']['required'],
                         ['primary_store', 'store_1'])

    def test_can_udp_test_cannot_claim_vcan_binding(self):
        device = self.read('devices/meter_1.json')
        device['protocol'] = {'type': 'can_socketcan', 'can': {
            'interfaceName': 'vcan0', 'manageInterface': False,
            'transportMode': 'udp_test', 'udpPeerAddress': '203.0.113.10'}}
        self.write('devices/meter_1.json', device)
        binding = {'meter_1': {'kind': 'can-simulator', 'endpoint': 'vcan0'}}
        self.rejects('vcan socketcan simulator', simulator_bindings=binding)
        device['protocol']['can']['transportMode'] = 'socketcan'
        self.write('devices/meter_1.json', device)
        self.assertIn('can-driver@meter_1.service',
                      [r['unit'] for r in self.resolve(simulator_bindings=binding)['roles']])

    def test_iec_tcp_uses_reviewed_loopback_binding(self):
        device = self.read('devices/meter_1.json')
        device['protocol'] = {'type': 'iec104', 'tcp': {'host': '127.0.0.1', 'port': 2404}}
        self.write('devices/meter_1.json', device)
        roles = self.resolve(simulator_bindings={
            'meter_1': {'kind': 'tcp-simulator', 'endpoint': '127.0.0.1:2404'}})['roles']
        self.assertIn('iec-driver@meter_1.service', [r['unit'] for r in roles])

    def test_camera_requires_test_source_not_physical_device(self):
        camera = {'runtimeMode': 'gateway', 'identityConfigFile': TARGET + 'device_identity.json',
                  'deviceConfigFiles': [], 'cameraService': {'enabled': True,
                  'cameras': [{'enabled': True, 'sourceType': 'rtsp', 'source': 'rtsp://camera.invalid'}]}}
        self.write('apps/camera-service.json', camera)
        self.rejects('camera requires reviewed simulator inputs', capabilities=['camera-simulator'])
        camera['cameraService']['cameras'][0] = {'enabled': True, 'sourceType': 'test',
                                                 'source': '', 'cameraCode': 'test_cam',
                                                 'sourceAuth': {'enabled': False},
                                                 'stream': {'publishUrl': 'rtsp://127.0.0.1:8554/test_cam'}}
        self.write('apps/camera-service.json', camera)
        binding = dict(self.bindings, **{'camera:test_cam': {
            'kind': 'rtsp-receiver', 'endpoint': 'rtsp://127.0.0.1:8554/test_cam'}})
        self.rejects('receiver binding missing', capabilities=['camera-simulator'])
        roles = {r['unit']: r for r in self.resolve(capabilities=['camera-simulator'],
                                                  simulator_bindings=binding)['roles']}
        self.assertEqual(roles['camera-service@camera-service.service']['shm']['required'],
                         ['gateway_point_store'])
        self.assertEqual(roles['camera-service@camera-service.service']['externalExecutables'],
                         ['/usr/bin/ffmpeg', '/bin/sh'])
        camera['cameraService']['allowCustomCommand'] = True
        self.write('apps/camera-service.json', camera)
        self.rejects('custom command', capabilities=['camera-simulator'], simulator_bindings=binding)
        camera['cameraService']['allowCustomCommand'] = False
        camera['cameraService']['cameras'][0]['command'] = 'true'
        self.write('apps/camera-service.json', camera)
        self.rejects('custom command', capabilities=['camera-simulator'], simulator_bindings=binding)
        camera['cameraService']['cameras'][0]['command'] = ''
        camera['cameraService']['cameras'][0]['stream']['publishUrl'] = 'rtsp://outside.invalid/live'
        self.write('apps/camera-service.json', camera)
        self.rejects('loopback RTSP', capabilities=['camera-simulator'], simulator_bindings=binding)

    def test_camera_derived_publish_url_requires_exact_local_receiver(self):
        self.write('apps/camera-service.json', {
            'runtimeMode': 'gateway', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [], 'cameraService': {
                'enabled': True, 'media': {'serverUrl': 'rtsp://127.0.0.1:8554',
                                            'auth': {'enabled': False}},
                'cameras': [{'enabled': True, 'cameraCode': 'test_cam',
                             'sourceType': 'test', 'source': '', 'command': '',
                             'sourceAuth': {'enabled': False}, 'stream': {'path': '/test_cam'}}]}})
        binding = dict(self.bindings, **{'camera:test_cam': {
            'kind': 'rtsp-receiver', 'endpoint': 'rtsp://127.0.0.1:8554/test_cam'}})
        self.assertIn('camera-service@camera-service.service',
                      [r['unit'] for r in self.resolve(capabilities=['camera-simulator'],
                                                        simulator_bindings=binding)['roles']])
        camera = self.read('apps/camera-service.json')
        camera['cameraService']['media']['serverUrl'] = 'rtsp://outside.invalid:8554'
        self.write('apps/camera-service.json', camera)
        self.rejects('loopback RTSP', capabilities=['camera-simulator'], simulator_bindings=binding)
        camera['cameraService']['media']['serverUrl'] = 'rtsp://127.0.0.1:8554'
        camera['cameraService']['media']['auth']['enabled'] = True
        self.write('apps/camera-service.json', camera)
        self.rejects('output authentication', capabilities=['camera-simulator'], simulator_bindings=binding)

    def test_camera_omitted_enabled_is_active_and_must_be_reviewed(self):
        self.write('apps/camera-service.json', {
            'runtimeMode': 'gateway', 'identityConfigFile': TARGET + 'device_identity.json',
            'deviceConfigFiles': [], 'cameraService': {'enabled': True,
                'cameras': [
                    {'enabled': True, 'cameraCode': 'safe', 'sourceType': 'test',
                     'source': '', 'stream': {'publishUrl': 'rtsp://127.0.0.1:8554/safe'}},
                    {'cameraCode': 'implicit', 'sourceType': 'rtsp',
                     'source': 'rtsp://outside.invalid/live',
                     'stream': {'publishUrl': 'rtsp://outside.invalid/publish'}}]}})
        binding = dict(self.bindings, **{'camera:safe': {
            'kind': 'rtsp-receiver', 'endpoint': 'rtsp://127.0.0.1:8554/safe'}})
        self.rejects('camera requires reviewed simulator inputs',
                     capabilities=['camera-simulator'], simulator_bindings=binding)
        camera = self.read('apps/camera-service.json')
        camera['cameraService']['cameras'][1]['sourceType'] = 'test'
        camera['cameraService']['cameras'][1]['source'] = ''
        self.write('apps/camera-service.json', camera)
        self.rejects('loopback RTSP', capabilities=['camera-simulator'], simulator_bindings=binding)
        camera['cameraService']['cameras'][1]['stream']['publishUrl'] = 'rtsp://127.0.0.1:8554/implicit'
        self.write('apps/camera-service.json', camera)
        self.rejects('receiver binding missing', capabilities=['camera-simulator'], simulator_bindings=binding)

    def test_unmodified_factory_profile_cannot_silently_admit_hardware(self):
        factory = Path(self.temp.name) / 'factory'
        shutil.copytree(ROOT / 'config/factory/runtime', factory)
        identity = json.loads((factory / 'device_identity.json').read_text(encoding='utf-8'))
        pins = {'machineCode': identity['machineCode'],
                'identitySha256': hashlib.sha256((factory / 'device_identity.json').read_bytes()).hexdigest(),
                'networkSha256': {}, 'tlsSha256': {
                    p.relative_to(factory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (factory / 'tls').rglob('*') if p.is_file()},
                'otaPublicKeySha256': hashlib.sha256(self.trust.read_bytes()).hexdigest(),
                'monitorShmName': 'gateway_point_store_system_monitor'}
        with self.assertRaises(policy.RolePolicyError) as failure:
            policy.resolve(factory, self.templates, pins, self.trust)
        self.assertRegex(str(failure.exception),
                         'hardware/simulator binding missing|network route/failover role is protected')

    def test_matched_tunnel_is_exact_dependency_and_duplicate_rejected(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['mqtt']['broker'] = 'tcp://127.0.0.1:18883'
        self.write('apps/mqtt-service.json', mqtt)
        path = self.root / 'tls/mqtt-service-stunnel.conf'
        path.parent.mkdir()
        path.write_text('foreground = yes\n[mqtts]\naccept = 127.0.0.1:18883\n'
                        'connect = broker.example.invalid:8883\n')
        authority = self.authority()
        authority['tlsSha256'] = {'tls/mqtt-service-stunnel.conf':
                                  hashlib.sha256(path.read_bytes()).hexdigest()}
        roles = {r['unit']: r for r in self.resolve(authority=authority)['roles']}
        tunnel = 'mqtt-tls-tunnel@mqtt-service-stunnel.service'
        self.assertIn(tunnel, roles)
        self.assertEqual(roles['mqtt-driver@mqtt-service.service']['dependencies'], [tunnel])
        self.assertEqual(roles[tunnel]['configPaths'], ['tls/mqtt-service-stunnel.conf'])
        other = self.root / 'tls/other-stunnel.conf'
        other.write_bytes(path.read_bytes())
        authority['tlsSha256']['tls/other-stunnel.conf'] = hashlib.sha256(other.read_bytes()).hexdigest()
        self.rejects('ambiguous matching stunnel', authority=authority)

    def test_storage_backend_is_explicit_and_eventstore_not_service(self):
        mqtt = self.read('apps/mqtt-service.json')
        mqtt['alarmStore'] = {'enabled': True,
                              'sqlitePath': '/opt/modbus-gateway/data/alarm_events.db'}
        self.write('apps/mqtt-service.json', mqtt)
        result = self.resolve()
        self.assertEqual(result['storage']['backend'], 'legacy-sqlite')
        self.assertFalse(result['storage']['eventStoreServiceAdmitted'])
        mqtt['eventStore'] = {'backend': 'ipc-lab'}
        self.write('apps/mqtt-service.json', mqtt)
        self.rejects('unqualified EventStore backend')

    def test_no_mutation_on_failure_and_no_runtime_authority(self):
        self.rejects('hardware/simulator binding missing', simulator_bindings={})
        self.assertFalse((self.root / 'data').exists())
        self.assertFalse((self.root / 'runtime-upgrade-stop').exists())


if __name__ == '__main__':
    unittest.main()
