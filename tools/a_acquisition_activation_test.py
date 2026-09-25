"""Local Linux file transactions; systemctl, migration CLI and processes are simulated.

No compiler, network, real service, SHM or system directory is used. Only synthetic
config/binary pins are substituted; apply/activate/guard/refence/recover run as written.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parent.parent


def load():
    spec = importlib.util.spec_from_file_location('acquisition_offline_test', REPO / 'deploy/offline-runtime-upgrade.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content if isinstance(content, bytes) else content.encode())


class AcquisitionTest(unittest.TestCase):
    def patch(self, *args, **kwargs):
        patcher = mock.patch.object(*args, **kwargs)
        value = patcher.start()
        self.addCleanup(patcher.stop)
        return value

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='a-acquisition-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.m = load()
        self.g = self.m.guard
        self.real_pins = self.g.A_SOURCE_CONFIGS.copy()
        self.calls, self.live, self.overrides = [], set(), {}
        self.fail_start, self.fail_stop = None, False

        def mapped(*parts):
            path = Path(*parts)
            if path.is_absolute() and path.parts[1] in ('opt', 'etc', 'dev', 'proc', 'run', 'lib', 'usr'):
                return self.root / 'system' / str(path).lstrip('/')
            return path

        self.path = mapped
        self.patch(self.m, 'Path', mapped)
        self.patch(self.g, 'Path', mapped)
        self.home = mapped('/opt/modbus-gateway')
        self.config = self.home / 'config/runtime'
        self.state_dir = self.root / 'transaction'
        self.patch(self.m, 'MONITOR_DROPIN', mapped('/etc/systemd/system/' + self.g.MONITOR_UNIT + '.d/998-shm.conf'))
        write(mapped('/proc/sys/kernel/random/boot_id'), 'synthetic-boot\n')
        source = json.loads((REPO / 'tools/fixtures/a-profile-preflight-20260925-01.json').read_text())
        self.documents = {name: row['projection']['document'] for name, row in source['documents'].items()}
        # Reconstructed redacted documents are SYNTHETIC, never passed off as full actual configs.
        for name in ('apps/monitor-service.json', 'apps/mqtt-service.json'):
            self.documents[name]['deviceConfigFiles'] = [str(self.config / 'devices/device_modbusRTU_2_readonly.json')]
        monitor = self.documents['apps/monitor-service.json']
        monitor['localDisplay']['scada'].update(autoReload=True, projectDirectory=str(self.home / 'scada/current'))
        mqtt = self.documents['apps/mqtt-service.json']
        mqtt['mqtt'].update(telemetryTopic='edge/telemetry', username='synthetic', password='test-only', tls={'enabled': True})
        mqtt['mqttDriver']['fullUploadWorker'].update(healthHeartbeatMs=1000, failoverTimeoutMs=3000,
                                                     retryMinMs=500, retryMaxMs=5000)
        for name, doc in self.documents.items():
            write(self.config / name, json.dumps(doc, indent=3) + '\n')
        write(self.config / 'device_identity.json', '{"machineCode":"SYNTHETIC_A"}\n')
        write(self.config / 'qt-display.env', 'DISPLAY=:0\n')
        write(self.home / 'scada/releases/old/runtime-map.json', '[]\n')
        write(self.home / 'scada/releases/new/runtime-map.json', '[]\n')
        (self.home / 'scada/current').symlink_to('releases/old')
        for name in ('gateway-qt-run.sh', 'gateway-services.sh'):
            write(self.home / 'bin' / name, (REPO / 'deploy' / name).read_bytes())
            (self.home / 'bin' / name).chmod(0o755)
        for name in ('modbus-rtu@.service', 'mqtt-driver@.service', 'system-monitor@.service',
                     'ky-ems.service', 'gateway-services.service', 'qt-display-bridge.service'):
            write(mapped('/etc/systemd/system') / name, (REPO / 'deploy' / name).read_bytes())
        components, installed, paths = [], {}, {}
        for name in ('ModbusRtu', 'MqttDriver', 'SystemMonitor', 'LocalDisplayQtEms', 'memory_point_store_migrate'):
            relative = 'ky-ems/KY-EMS' if name == 'LocalDisplayQtEms' else 'bin/' + name
            paths[name] = relative
            write(self.home / relative, 'old ' + name)
            installed[relative] = sha(self.home / relative)
            payload = self.root / 'payload' / relative
            write(payload, 'synthetic new ' + name)
            components.append({'kind': 'product', 'target': name, 'archivePath': relative,
                               'sha256': sha(payload), 'bytes': payload.stat().st_size})
        for owner, key, target in ((self.g, 'MONITOR_BINARY', 'SystemMonitor'),
                                   (self.m, 'B_MONITOR_BINARY_SHA256', 'SystemMonitor'),
                                   (self.g, 'QT_BINARY', 'LocalDisplayQtEms'),
                                   (self.m, 'A_QT_BINARY_SHA256', 'LocalDisplayQtEms')):
            self.patch(owner, key, next(c['sha256'] for c in components if c['target'] == target))
        pins = {name: sha(self.config / name) for name in self.documents}
        self.patch(self.g, 'A_SOURCE_CONFIGS', pins)
        stores = set().union(*(self.g.configured_names(doc) for doc in self.documents.values()))
        stores.update(('gateway_point_store', self.m.MONITOR_IMPLICIT_SHM))
        segments = []
        for i, name in enumerate(sorted(stores)):
            file = mapped('/dev/shm') / name
            write(file, struct.pack('<II', 0x4d505354, 10) + b'synthetic measured data')
            segments.append({'source': name, 'target': 'synthetic_target_' + str(i), 'sha256': sha(file)})
        write(self.root / 'manifest.json', json.dumps({'components': components}))
        self.approval = {
            'schemaVersion': 'offline-shm11-2', 'mode': 'standalone', 'controlEnabled': False,
            'aReadonlyAcquisition': True, 'gatewayHome': str(self.home), 'nodeId': 'SYNTHETIC_A',
            'transactionId': 'synthetic-acquisition', 'expiresAtUnix': int(time.time()) + 900,
            'runtimeCompatibility': dict(self.g.COMPATIBILITY), 'segments': segments,
            'units': ['gateway-services.service'] + self.g.A_ACQUISITION_READERS + [
                'modbus-rtu@.service', 'mqtt-driver@.service', 'system-monitor@.service', 'qt-display-bridge.service'],
            'configSha256': self.m.tree_hashes(self.config), 'installPaths': paths,
            'installedRuntimeSha256': installed, 'programManifestSha256': sha(self.root / 'manifest.json'),
            'identitySha256': sha(self.config / 'device_identity.json'),
            'qtDisplayEnvSha256': sha(self.config / 'qt-display.env'),
            'aInboundDisableSourceSha256': pins['apps/monitor-service.json'],
            'scadaReadOnlyProject': {'oldTarget': 'releases/old', 'newTarget': 'releases/new'},
            'offlineLocal': {'nodeId': 'SYNTHETIC_A', 'controlDisabled': True, 'participantsStopped': True,
                             'restartInhibited': True, 'evidenceSha256': 'a' * 64},
            'activationStartupScripts': {name: {
                'oldSha256': None if name == 'runtime-upgrade-guard.py' else sha(self.home / 'bin' / name),
                'newSha256': sha(REPO / 'deploy' / name), 'mode': 0o755} for name in self.m.ACTIVATION_SCRIPTS}}
        self.originals = {name: (self.config / name).read_bytes() for name in self.m.tree_hashes(self.config)}
        # Existing SCADA package validation has its own tests; verify the calls remain in this profile.
        self.scada_calls = self.patch(self.g, 'scada_readonly_binding')
        self.patch(self.m, 'systemctl', self.systemctl)
        def guard_value(unit, prop):
            value = self.properties(unit).get(prop, '')
            if unit == self.g.MONITOR_UNIT and prop == 'DropInPaths':
                value = value.replace(str(self.m.MONITOR_DROPIN), '/etc/systemd/system/' + unit + '.d/998-shm.conf')
            return value
        self.patch(self.g, 'systemctl_value', guard_value)
        self.patch(self.m.subprocess, 'run', self.external)
        self.patch(self.m.subprocess, 'check_output', lambda *a, **k: 'ext4\n')

    def command(self, executable, arguments=''):
        return '{ path=' + executable + ' ; argv[]=' + executable + arguments + ' ; ignore_errors=no ; pid=0 ; code=(null) ; }'

    def properties(self, unit):
        g = self.g
        template = unit.split('@')[0] + '@.service' if '@' in unit else unit
        fragment = self.path('/etc/systemd/system') / template
        directory = self.path('/etc/systemd/system') / (unit + '.d')
        drops = sorted(str(p) for p in directory.glob('*.conf'))
        executables = {g.A_RTU_UNIT: ('bin/ModbusRtu', ' --config ' + str(self.config / 'devices/device_modbusRTU_2_readonly.json')),
                       g.A_MQTT_UNIT: ('bin/MqttDriver', ' --app-config ' + str(self.config / 'apps/mqtt-service.json')),
                       g.MONITOR_UNIT: ('bin/SystemMonitor', ' --app-config ' + str(self.config / 'apps/monitor-service.json')),
                       'gateway-services.service': ('bin/gateway-services.sh', ' apply')}
        executable, arguments = executables.get(unit, ('', ''))
        executable = str(self.home / executable)
        if unit == g.QT_UNIT:
            executable, arguments = '/bin/sh', ' ' + str(self.home / 'bin/gateway-qt-run.sh')
        slices = {g.A_RTU_UNIT: r'system-modbus\x2drtu.slice', g.A_MQTT_UNIT: r'system-mqtt\x2ddriver.slice',
                  g.MONITOR_UNIT: r'system-system\x2dmonitor.slice'}
        values = dict.fromkeys(('Environment', 'PassEnvironment', 'UnsetEnvironment', 'Wants', 'BindsTo', 'Triggers', 'TriggeredBy'), '')
        values.update(Id=unit, LoadState='loaded', NeedDaemonReload='no', FragmentPath=str(fragment),
                      DropInPaths=' '.join(drops), WorkingDirectory=str(self.home), UnitFileState='disabled',
                      ActiveState='active' if unit in self.live or unit == 'graphical.target' else 'inactive',
                      MainPID='123' if unit in self.live else '0',
                      Requires='sysinit.target ' + slices.get(unit, 'system.slice'),
                      ExecStart=self.command(executable, arguments))
        if unit == g.QT_UNIT:
            values.update(WorkingDirectory=str(self.home / 'ky-ems'), Requires=values['Requires'] + ' qt-display-bridge.service',
                          Wants='graphical.target', Environment='DISPLAY=:0 QT_QPA_PLATFORM=xcb XDG_RUNTIME_DIR=/run/ky-ems',
                          EnvironmentFiles=str(self.config / 'qt-display.env') + ' (ignore_errors=yes)')
        if unit == g.MONITOR_UNIT and self.m.MONITOR_DROPIN.exists():
            values['Environment'] = self.m.MONITOR_DROPIN.read_text().split('Environment=')[1].strip()
        if unit == 'gateway-services.service':
            values['ExecStop'] = self.command(executable, ' stop')
        for path in directory.glob('*.conf'):
            for line in path.read_text().splitlines():
                if line.startswith('ExecStartPre='):
                    executable, arguments = line.split('=', 1)[1].split(' ', 1)
                    values['ExecStartPre'] = self.command(executable, ' ' + arguments)
        values.update(self.overrides.get(unit, {}))
        return {key: value for key, value in values.items() if value is not None}

    def systemctl(self, *args):
        self.calls.append(args)
        if args[0] in ('list-units', 'list-unit-files'):
            return ''
        if args[0] == 'stop':
            if self.fail_stop:
                raise ValueError('synthetic stop failure')
            self.live.difference_update(args[1:])
            return ''
        if args[0] == 'start':
            if args[1] == self.fail_start:
                raise ValueError('synthetic start failure')
            self.g.activated_unit(self.home, args[1])
            self.live.add(args[1])
            return ''
        if args[0] == 'show':
            if args[1] == '--property=Id,ActiveState,MainPID':
                return '\n\n'.join('\n'.join(key + '=' + self.properties(unit)[key]
                                             for key in ('Id', 'ActiveState', 'MainPID')) for unit in args[2:])
            prop = args[1].split('=', 1)[1]
            if prop == 'FragmentPath' and args[-1] in (self.g.QT_UNIT, 'qt-display-bridge.service'):
                # The existing A preflight compares these two paths as literal strings.
                return '/etc/systemd/system/' + args[-1]
            if args[-1] == 'gateway-health-watchdog.service' and prop == 'LoadState':
                return 'not-found'
            return self.properties(args[-1]).get(prop, '')
        if args[0] in ('daemon-reload', 'is-active'):
            return ''
        raise AssertionError('unexpected systemctl: ' + str(args))

    def external(self, command, **kwargs):
        if command[0] == 'systemctl':
            unit = command[-1]
            requested = next(arg.split('=', 1)[1].split(',') for arg in command if arg.startswith('--property='))
            body = '\n'.join(k + '=' + v for k, v in self.properties(unit).items() if k in requested)
            return subprocess.CompletedProcess(command, 0, body, '')
        if '--copy-to-v11' in command:
            target = command[command.index('--copy-to-v11') + 1]
            write(self.path('/dev/shm') / target, struct.pack('<II', 0x4d505354, 11) + b'synthetic measured data')
            return subprocess.CompletedProcess(command, 0, b'synthetic migration\n')
        raise AssertionError('unexpected external command: ' + str(command))

    def apply(self):
        write(self.root / 'approval.json', json.dumps(self.approval))
        self.args = types.SimpleNamespace(approval=self.root / 'approval.json', approval_sha256=sha(self.root / 'approval.json'),
                                         manifest=self.root / 'manifest.json', payload=self.root / 'payload', state=self.state_dir)
        self.m.apply(self.args)
        return json.loads((self.state_dir / 'state.json').read_text())

    def activate(self, readers=None):
        pin = sha(self.state_dir / 'state.json')
        ready = {'schemaVersion': 'offline-shm11-activate-1', 'mode': 'standalone', 'controlEnabled': False,
                 'expiresAtUnix': int(time.time()) + 900, 'transactionId': self.approval['transactionId'],
                 'stateSha256': pin, 'approvalSha256': self.args.approval_sha256,
                 'programManifestSha256': self.approval['programManifestSha256'],
                 'startUnits': self.g.A_ACQUISITION_READERS if readers is None else readers,
                 'local': {'nodeId': self.approval['nodeId'], 'phase': 'UPGRADED_STOPPED', 'controlEnabled': False,
                           'stateSha256': pin, 'programManifestSha256': self.approval['programManifestSha256']}}
        self.args.ready = self.root / 'ready.json'
        write(self.args.ready, json.dumps(ready))
        self.args.ready_sha256 = sha(self.args.ready)
        self.m.activate(self.args)

    def assert_refenced(self):
        self.assertFalse(self.live)
        self.assertFalse((self.home / 'data' / self.g.ACTIVATION_FILE).exists())
        for unit in self.approval['units']:
            path, body = self.m.inhibition_dropin(self.home, self.approval['transactionId'], unit)
            self.assertEqual(body, path.read_bytes())
        stops = [set(call[1:]) for call in self.calls if call[0] == 'stop']
        self.assertTrue(any(set(self.g.A_ACQUISITION_READERS) <= group for group in stops))

    def test_apply_activate_recover_restores_exact_bytes(self):
        state = self.apply()
        self.assertEqual('UPGRADED_STOPPED', state['phase'])
        mqtt = json.loads((self.config / 'apps/mqtt-service.json').read_text())
        self.assertTrue(all(mqtt['mqtt'][key] == '' for key in self.g.A_RX_TOPICS))
        expected = self.m.switch_names(self.documents['apps/mqtt-service.json'],
                                       {s['source']: s['target'] for s in self.approval['segments']})
        self.assertEqual(expected['mqttDriver'], mqtt['mqttDriver'])
        self.assertEqual(5000, mqtt['mqttDriver']['fullUploadIntervalMs'])
        for key in ('username', 'password', 'tls', 'telemetryTopic'):
            self.assertEqual(expected['mqtt'][key], mqtt['mqtt'][key])
        self.activate()
        self.assertEqual(set(self.g.A_ACQUISITION_READERS + ['gateway-services.service']), self.live)
        state = json.loads((self.state_dir / 'state.json').read_text())
        self.assertEqual(self.g.A_ACQUISITION_PROFILE, state['activationProfile'])
        self.assertEqual('STATIC_PINNED_FILES', state['aReadonlyUnitProof']['source'])
        self.assertTrue(self.scada_calls.called)
        self.m.recover(self.args)
        self.assert_refenced()
        for name, raw in self.originals.items():
            self.assertEqual(raw, (self.config / name).read_bytes(), name)
        self.assertEqual('RECOVERED_STOPPED', json.loads((self.state_dir / 'state.json').read_text())['phase'])

    def test_each_driver_start_failure_refences_every_participant(self):
        self.apply()
        self.fail_start = self.g.A_MQTT_UNIT
        with self.assertRaisesRegex(ValueError, 'start failure'):
            self.activate()
        self.assert_refenced()
        self.m.recover(self.args)
        for name, raw in self.originals.items():
            self.assertEqual(raw, (self.config / name).read_bytes())

    def test_rtu_start_failure_refences_every_participant(self):
        self.apply()
        self.fail_start = self.g.A_RTU_UNIT
        with self.assertRaisesRegex(ValueError, 'start failure'):
            self.activate()
        self.assert_refenced()

    def test_interrupted_second_start_refences_and_recovers(self):
        self.apply()
        original = self.m.systemctl
        def interrupted(*args):
            if args == ('start', self.g.A_MQTT_UNIT):
                raise KeyboardInterrupt('synthetic interruption')
            return original(*args)
        self.patch(self.m, 'systemctl', interrupted)
        with self.assertRaises(KeyboardInterrupt):
            self.activate()
        self.assert_refenced()
        self.m.recover(self.args)
        for name, raw in self.originals.items():
            self.assertEqual(raw, (self.config / name).read_bytes())

    def test_guarded_same_boot_service_cycle_keeps_full_configuration(self):
        self.apply()
        self.activate()
        before = (self.config / 'apps/mqtt-service.json').read_bytes()
        units = self.g.A_ACQUISITION_READERS + ['gateway-services.service']
        self.systemctl('stop', *units)
        for unit in units:
            self.systemctl('start', unit)
        self.assertEqual(set(units), self.live)
        self.assertEqual(before, (self.config / 'apps/mqtt-service.json').read_bytes())

    def test_changed_backup_prevents_any_config_restore(self):
        state = self.apply()
        self.activate()
        index = next(i for i, row in enumerate(state['files']) if row['path'] == 'config/runtime/apps/mqtt-service.json')
        write(self.state_dir / 'files' / str(index), 'changed backup')
        before = self.m.tree_hashes(self.config)
        with self.assertRaisesRegex(ValueError, 'backup mismatch'):
            self.m.recover(self.args)
        self.assert_refenced()
        self.assertEqual(before, self.m.tree_hashes(self.config))

    def test_unconfirmed_stop_retains_failed_state_and_backups(self):
        self.apply()
        self.fail_start, self.fail_stop = self.g.A_MQTT_UNIT, True
        with self.assertRaises(ValueError):
            self.activate()
        state = json.loads((self.state_dir / 'state.json').read_text())
        self.assertEqual('FAILED_STOP_UNCONFIRMED', state['phase'])
        self.assertIn(self.g.A_RTU_UNIT, self.live)
        self.assertTrue({self.g.A_RTU_UNIT, self.g.A_MQTT_UNIT} <= {r['unit'] for r in state['activationStopResult']})
        self.assertTrue(any(self.state_dir.joinpath('files').iterdir()))

    def test_opt_in_strict_boolean_and_source_pin(self):
        for value in (False, 0, 1, 'true', None, {}, []):
            self.approval['aReadonlyAcquisition'] = value
            with self.assertRaisesRegex(ValueError, 'original config pins'):
                self.apply()
            self.assertFalse(self.state_dir.exists())
        self.approval['aReadonlyAcquisition'] = True
        self.approval['configSha256']['apps/mqtt-service.json'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'original config pins'):
            self.apply()

    def test_actual_four_source_pins_are_fixed(self):
        self.assertEqual('a656fe30d5a7220cad5efcde04f40b4211393fb56bfa867eaf3f9b097d8f542b',
                         self.real_pins['apps/mqtt-service.json'])
        for relative, constant in (('modbus-rtu@.service', self.g.A_RTU_TEMPLATE),
                                   ('mqtt-driver@.service', self.g.A_MQTT_TEMPLATE)):
            self.assertEqual(constant, sha(REPO / 'deploy' / relative))

    def test_ready_cannot_drop_or_add_units(self):
        self.apply()
        for readers in ([self.g.MONITOR_UNIT, self.g.QT_UNIT], self.g.A_ACQUISITION_READERS[:-1],
                        self.g.A_ACQUISITION_READERS + ['camera-service@camera-service.service']):
            with self.assertRaisesRegex(ValueError, 'ready group'):
                self.activate(readers)
        self.assertFalse(self.live)

    def test_runtime_guard_rejects_patch_expansion_even_with_rehashed_state(self):
        state = self.apply()
        path = self.config / 'apps/mqtt-service.json'
        document = json.loads(path.read_text())
        for key, value in (('fullUploadIntervalMs', 0), ('enabled', 1)):
            document['mqttDriver'][key] = value
            write(path, json.dumps(document))
            state['configSha256']['apps/mqtt-service.json'] = sha(path)
            next(row for row in state['files'] if row['path'] == 'config/runtime/apps/mqtt-service.json')['newSha256'] = sha(path)
            with self.assertRaisesRegex(ValueError, 'minimal config patch'):
                self.g.a_acquisition_config(self.home, self.state_dir, self.approval, state)
            document['mqttDriver'][key] = self.documents['apps/mqtt-service.json']['mqttDriver'][key]

    def test_unknown_dependency_environment_or_unloaded_state_refused(self):
        self.apply()
        for field, value in (('Requires', 'sysinit.target evil.service'), ('Wants', 'evil.service'),
                             ('Environment', 'LD_PRELOAD=evil'), ('PassEnvironment', 'LD_PRELOAD'),
                             ('EnvironmentFiles', '/unknown'), ('NeedDaemonReload', 'yes'),
                             ('LoadState', 'not-found'), ('Id', None), ('ExecStart', 'unknown'),
                             ('ExecStartPre', 'unexpected'), ('ExecStop', 'unexpected')):
            with self.subTest(field=field):
                self.overrides[self.g.A_RTU_UNIT] = {field: value}
                with self.assertRaises(ValueError):
                    self.activate()
                self.assertFalse(self.live)

    def test_shadowed_template_override_refused(self):
        self.apply()
        path = self.path('/etc/systemd/system/modbus-rtu@.service.d') / ('90-offline-' + self.approval['transactionId'] + '.conf')
        write(path, '[Service]\nEnvironmentFile=/unknown\n')
        with self.assertRaisesRegex(ValueError, 'drop-in bytes'):
            self.activate()

    def test_missing_monitor_envfile_closes_only_with_actual_file_coverage(self):
        self.apply()
        path = self.path('/run/systemd/system/system-monitor@.service.d/unknown.conf')
        write(path, '[Service]\nEnvironmentFile=/unknown\n')
        with self.assertRaisesRegex(ValueError, 'drop-in coverage'):
            self.activate()
        path.unlink()
        self.activate()

    def test_late_dropin_drift_is_rechecked_by_guard(self):
        self.apply()
        self.activate()
        write(self.path('/etc/systemd/system/mqtt-driver@.service.d/99-extra.conf'), '[Service]\nEnvironment=EVIL=1\n')
        with self.assertRaisesRegex(ValueError, 'drop-in coverage'):
            self.g.activated_unit(self.home, self.g.A_MQTT_UNIT)

    def test_missing_nonempty_prestart_not_static_empty(self):
        self.apply()
        self.activate()
        self.overrides[self.g.A_MQTT_UNIT] = {'ExecStartPre': None}
        with self.assertRaisesRegex(ValueError, 'nonempty effective field missing'):
            self.g.activated_unit(self.home, self.g.A_MQTT_UNIT)

    def test_old_groups_keep_original_membership(self):
        approval = {'units': [self.g.MONITOR_UNIT, 'gateway-health-watchdog.service']}
        self.assertEqual(['gateway-services.service', 'gateway-health-watchdog.service', self.g.MONITOR_UNIT],
                         self.m.activation_units(approval, [self.g.MONITOR_UNIT]))
        self.assertEqual(['gateway-services.service', self.g.MONITOR_UNIT, self.g.QT_UNIT],
                         self.m.activation_units(approval, [self.g.MONITOR_UNIT, self.g.QT_UNIT]))


if __name__ == '__main__':
    unittest.main()
