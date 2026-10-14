"""Literal systemd environment fixtures; no service or shared-memory mutation."""
import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('monitor_binding_guard', ROOT / 'deploy/runtime-upgrade-guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
KEY = 'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME'
DEFAULT = 'gateway_point_store_system_monitor'
MONITOR = 'system-monitor@monitor-service.service'
QT = 'ky-ems.service'


class ProductionMonitorBindingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='production-monitor-binding-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.config = self.home / 'config/runtime'
        self.config.mkdir(parents=True)
        self.baseline = {'format': 'runtime-baseline-v2', 'gatewayHome': str(self.home),
                         'configSha256': {}, 'units': [],
                         'roleAuthority': {'resolved': {'protected': {'monitorShmName': 'fresh_monitor'},
                             'roles': [{'family': 'system-monitor', 'unit': MONITOR,
                                        'runtimeFiles': ['bin/SystemMonitor']}]}}}
        self.properties = {MONITOR: {'Environment': KEY + '=fresh_monitor'}}
        patcher = mock.patch.object(guard, 'systemctl_value', self.property)
        patcher.start()
        self.addCleanup(patcher.stop)
        if os.name == 'nt':
            # NT chmod has no POSIX ownership/mode semantics; Linux keeps the real check.
            pin = mock.patch.object(guard, 'production_pin',
                                    lambda p: {'sha256': hashlib.sha256(guard.exact_file(p).read_bytes()).hexdigest()})
            pin.start()
            self.addCleanup(pin.stop)

    def property(self, unit, prop):
        self.assertIn(prop, ('Environment', 'EnvironmentFiles', 'PassEnvironment', 'UnsetEnvironment', 'PAMName'))
        return self.properties.get(unit, {}).get(prop, '')

    def check(self, config_root=None):
        guard.production_monitor_bindings(self.home, self.baseline, config_root=config_root)

    def qt(self):
        self.baseline['roleAuthority']['resolved']['roles'].append({
            'family': 'ky-ems', 'unit': QT, 'runtimeFiles': ['ky-ems/KY-EMS', 'bin/gateway-qt-native-run.sh']})
        self.properties[QT] = {'Environment': 'DISPLAY=:0 QT_QPA_PLATFORM=offscreen',
                              'EnvironmentFiles': str(self.config / 'qt-display.env') + ' (ignore_errors=yes)'}
        self.envfile(KEY + '=fresh_monitor\n')

    def envfile(self, raw, root=None):
        path = (root if root is not None else self.config) / 'qt-display.env'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw.encode('ascii'))
        path.chmod(0o644)
        self.baseline['configSha256']['qt-display.env'] = hashlib.sha256(path.read_bytes()).hexdigest()

    def test_exact_monitor_binding_passes_without_writes(self):
        before = copy.deepcopy(self.baseline)
        self.check()
        self.assertEqual(self.baseline, before)
        self.assertEqual(list(self.config.iterdir()), [])

    def test_monitor_missing_empty_and_stale_bindings_are_refused(self):
        for env in ('', KEY + '=', KEY + '=' + DEFAULT, KEY + '=retired_monitor'):
            with self.subTest(environment=env):
                self.properties[MONITOR]['Environment'] = env
                with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
                    self.check()

    def test_default_requires_an_explicit_nonempty_environment_binding(self):
        self.baseline['roleAuthority']['resolved']['protected']['monitorShmName'] = DEFAULT
        for env in ('', KEY + '='):
            self.properties[MONITOR]['Environment'] = env
            with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
                self.check()
        self.properties[MONITOR]['Environment'] = KEY + '=' + DEFAULT
        self.check()

    def test_duplicate_escaped_and_malformed_manager_values_are_refused(self):
        for env in (KEY + '=fresh_monitor ' + KEY + '=retired', KEY + '=fresh\\x5fmonitor',
                    KEY + '=fresh_monitor\n', 'NOT_AN_ASSIGNMENT', KEY + '="fresh_monitor'):
            with self.subTest(environment=env):
                self.properties[MONITOR]['Environment'] = env
                with self.assertRaises(ValueError):
                    self.check()

    def test_manager_quoted_literal_assignments_are_supported(self):
        self.properties[MONITOR]['Environment'] = '"' + KEY + '=fresh_monitor" "OTHER=literal value"'
        self.check()

    def test_other_environment_sources_and_removal_are_refused(self):
        for prop, value in (('EnvironmentFiles', '/foreign (ignore_errors=yes)'),
                            ('PassEnvironment', KEY), ('UnsetEnvironment', KEY), ('PAMName', 'login')):
            with self.subTest(property=prop):
                self.properties[MONITOR][prop] = value
                with self.assertRaises(ValueError):
                    self.check()
                del self.properties[MONITOR][prop]

    def test_native_qt_and_monitor_share_the_approved_binding(self):
        self.qt()
        self.check()
        self.envfile(KEY + '=retired_monitor\n')
        with self.assertRaisesRegex(ValueError, 'SHM environment mismatch: ky-ems.service'):
            self.check()

    def test_environment_file_overrides_manager_assignment(self):
        self.qt()
        self.properties[QT]['Environment'] = KEY + '=retired_monitor'
        self.check()
        self.envfile(KEY + '=\n')
        with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
            self.check()

    def test_environment_file_comments_quotes_spaces_and_crlf(self):
        self.qt()
        self.envfile('# comment\r\n; second comment\r\n' + KEY + '="fresh_monitor"\r\n'
                     "XAUTHORITY='/a path/with spaces'\r\nDISPLAY=:0\r\n")
        self.check()

    def test_file_duplicate_continuation_control_and_mixed_quotes_are_refused(self):
        self.qt()
        for raw in (KEY + '=fresh_monitor\n' + KEY + '=retired\n', KEY + '=fresh\\\nmonitor\n',
                    KEY + '=fresh_monitor\vOTHER=ignored\n', KEY + '=fresh_monitor\rOTHER=ignored\n',
                    KEY + '="fresh_monitor"unexpected\n', 'export ' + KEY + '=fresh_monitor\n',
                    KEY + ' =fresh_monitor\n'):
            with self.subTest(raw=raw):
                self.envfile(raw)
                with self.assertRaises(ValueError):
                    self.check()

    def test_unpinned_changed_and_oversized_file_are_refused(self):
        self.qt()
        original = self.baseline['configSha256'].pop('qt-display.env')
        with self.assertRaisesRegex(ValueError, 'unapproved'):
            self.check()
        self.baseline['configSha256']['qt-display.env'] = original
        (self.config / 'qt-display.env').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'pin drift'):
            self.check()
        self.envfile('#' + 'x' * 65536)
        with self.assertRaisesRegex(ValueError, 'exceeds bound'):
            self.check()

    def test_missing_optional_file_uses_the_actual_default(self):
        self.qt()
        (self.config / 'qt-display.env').unlink()
        self.baseline['configSha256'].clear()
        with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
            self.check()
        self.properties[QT]['Environment'] += ' ' + KEY + '=fresh_monitor'
        self.check()
        self.properties[QT]['EnvironmentFiles'] = str(self.config / 'qt-display.env') + ' (ignore_errors=no)'
        with self.assertRaisesRegex(ValueError, 'unapproved'):
            self.check()

    def test_environment_file_second_read_cannot_use_unpinned_bytes(self):
        self.qt()
        original = guard.production_pin
        def changed(path):
            pin = original(path)
            path.write_bytes((KEY + '=fresh_monitor\nOTHER=unapproved\n').encode())
            return pin
        with mock.patch.object(guard, 'production_pin', changed):
            with self.assertRaisesRegex(ValueError, 'pin drift while reading'):
                self.check()

    def test_extra_absent_and_foreign_environment_file_paths_are_refused(self):
        self.qt()
        for files in ('', '/tmp/qt-display.env (ignore_errors=yes)',
                      self.properties[QT]['EnvironmentFiles'] + ' /tmp/override (ignore_errors=yes)'):
            with self.subTest(files=files):
                self.properties[QT]['EnvironmentFiles'] = files
                with self.assertRaisesRegex(ValueError, 'approved path'):
                    self.check()

    def test_candidate_config_uses_staged_bytes_with_live_systemd_path(self):
        self.qt()
        staged = self.home / 'staged'
        self.envfile(KEY + '=retired_monitor\n')
        self.envfile(KEY + '=fresh_monitor\n', root=staged)
        self.check(config_root=staged)
        with self.assertRaisesRegex(ValueError, 'pin drift'):
            self.check()

    def test_legacy_and_unrelated_services_keep_their_existing_policy(self):
        self.baseline['format'] = 'runtime-baseline-v1'
        self.properties.clear()
        self.check()
        self.baseline['format'] = 'runtime-baseline-v2'
        self.baseline['roleAuthority']['resolved']['roles'] = [
            {'family': 'ky-ems', 'unit': QT, 'runtimeFiles': ['bin/gateway-qt-run.sh']},
            {'family': 'modbus-rtu', 'unit': 'modbus-rtu@meter.service', 'runtimeFiles': []}]
        self.check()

    def test_effective_unit_gate_includes_binding_verification(self):
        self.properties[MONITOR]['Environment'] = ''
        with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
            guard.production_effective(self.baseline)


if __name__ == '__main__':
    unittest.main()
