"""Opt-in environment proof using sandboxed sleep units, never gateway services."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('systemd_binding_guard', ROOT / 'deploy/runtime-upgrade-guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
KEY = 'GATEWAY_SYSTEM_MONITOR_SHARED_MEMORY_NAME'


@unittest.skipUnless(os.environ.get('GATEWAY_TEST_SYSTEMD_MONITOR_BINDING') == '1' and
                     getattr(os, 'geteuid', lambda: -1)() == 0,
                     'opt-in Linux root sandboxed systemd environment proof')
class SystemdMonitorBindingTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='gateway-monitor-binding-', dir='/srv')
        self.addCleanup(temp.cleanup)
        self.home = Path(temp.name)
        self.config = self.home / 'config/runtime'
        self.config.mkdir(parents=True)
        self.unit = 'gateway-monitor-binding-' + uuid.uuid4().hex[:12] + '.service'
        self.baseline = {'format': 'runtime-baseline-v2', 'configSha256': {},
                         'roleAuthority': {'resolved': {'protected': {'monitorShmName': 'fresh_monitor'},
                             'roles': [{'unit': self.unit, 'family': 'system-monitor',
                                        'runtimeFiles': ['bin/SystemMonitor']}]}}}
        self.addCleanup(self.stop)

    def stop(self):
        result = subprocess.run(['systemctl', 'stop', self.unit], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=20)
        if result.returncode and guard.systemctl_value(self.unit, 'LoadState') != 'not-found':
            self.fail('fixture unit did not stop: ' + result.stderr.decode(errors='replace'))
        self.assertIn(guard.systemctl_value(self.unit, 'ActiveState'), ('inactive', 'failed'))
        self.assertEqual(guard.systemctl_value(self.unit, 'MainPID'), '0')

    def start(self, properties):
        args = ['systemd-run', '--quiet', '--collect', '--unit=' + self.unit]
        for prop in ('PrivateNetwork=yes', 'PrivateDevices=yes', 'NoNewPrivileges=yes',
                     'CapabilityBoundingSet=', 'AmbientCapabilities=', 'ProtectSystem=strict',
                     'ProtectHome=yes', 'RuntimeMaxSec=30', 'TimeoutStopSec=5') + tuple(properties):
            args.append('--property=' + prop)
        subprocess.run(args + ['/bin/sleep', '30'], check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=20)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            pid = guard.systemctl_value(self.unit, 'MainPID')
            if pid.isdigit() and int(pid) > 0:
                proc = Path('/proc') / pid
                # MainPID can be visible before systemd's child has exec'd sleep.
                if (proc / 'exe').resolve() == Path('/bin/sleep').resolve():
                    environment = (proc / 'environ').read_bytes().split(b'\0')
                    self.environment = dict(row.split(b'=', 1) for row in environment if b'=' in row)
                    return
            time.sleep(0.05)
        self.fail('fixture sleep did not start')

    def test_monitor_explicit_binding_matches_real_process_environment(self):
        self.start(['Environment=' + KEY + '=fresh_monitor'])
        guard.production_monitor_bindings(self.home, self.baseline)
        self.assertEqual(self.environment[KEY.encode()], b'fresh_monitor')

    def test_monitor_default_without_binding_is_refused(self):
        self.start([])
        self.assertNotIn(KEY.encode(), self.environment)
        with self.assertRaisesRegex(ValueError, 'SHM environment mismatch'):
            guard.production_monitor_bindings(self.home, self.baseline)

    def test_qt_file_overrides_manager_and_matches_real_process_environment(self):
        path = self.config / 'qt-display.env'
        path.write_bytes(('# literal fixture\n' + KEY + '="fresh_monitor"\n').encode())
        path.chmod(0o600)
        self.baseline['configSha256']['qt-display.env'] = guard.production_pin(path)['sha256']
        role = self.baseline['roleAuthority']['resolved']['roles'][0]
        role.update(family='ky-ems', runtimeFiles=['bin/gateway-qt-native-run.sh'])
        self.start(['Environment=' + KEY + '=retired_monitor', 'EnvironmentFile=-' + str(path)])
        guard.production_monitor_bindings(self.home, self.baseline)
        self.assertEqual(self.environment[KEY.encode()], b'fresh_monitor')


if __name__ == '__main__':
    unittest.main()
