"""Native point-store startup must not require or start the legacy XML bridge."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'posix', 'POSIX launcher required')
class QtNativeRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.write('ky-ems/KY-EMS', '#!/bin/sh\nprintf "QT_EXECUTED %s\\n" "$*"\n')
        self.write('mock/ldd', '#!/bin/sh\necho "${MOCK_DEPS:-libQt5Core.so.5 => /lib/mock}"\n')
        self.env = dict(os.environ, GATEWAY_HOME=str(self.home), QT_QPA_PLATFORM='offscreen',
                        PATH=str(self.home / 'mock') + ':' + os.environ['PATH'])
        self.env.pop('GATEWAY_QT_BINARY', None)
        self.env.pop('GATEWAY_QT_BRIDGE', None)

    def write(self, name, text):
        path = self.home / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(0o755)

    def run_launcher(self, *args, legacy=False):
        launcher = 'gateway-qt-run.sh' if legacy else 'gateway-qt-native-run.sh'
        return subprocess.run(['/bin/sh', str(ROOT / 'deploy' / launcher), *args],
                              env=self.env, capture_output=True, text=True, timeout=10)

    def test_native_starts_without_bridge_and_keeps_app_config(self):
        result = self.run_launcher()
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn('QT_EXECUTED --app-config ' + str(self.home / 'config/runtime/apps/monitor-service.json'),
                      result.stdout)
        self.assertFalse((self.home / 'ky-ems/VarList.xml').exists())

    def test_legacy_default_still_requires_bridge(self):
        result = self.run_launcher(legacy=True)
        self.assertEqual(2, result.returncode)
        self.assertIn('QtDisplayBridge dependency missing', result.stderr)
        self.write('bin/QtDisplayBridge', '#!/bin/sh\nexit 99\n')
        self.assertEqual(0, self.run_launcher(legacy=True).returncode)

    def test_native_preflight_does_not_execute(self):
        result = self.run_launcher('--check')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn('QT_EXECUTED', result.stdout)

    def test_unknown_argument_rejected(self):
        self.write('bin/QtDisplayBridge', '#!/bin/sh\nexit 0\n')
        result = self.run_launcher('--native-point-stor')
        self.assertEqual(2, result.returncode)
        self.assertNotIn('QT_EXECUTED', result.stdout)

    def test_native_still_requires_dependencies_and_real_display_session(self):
        self.env['MOCK_DEPS'] = 'libQt5Widgets.so.5 => not found'
        result = self.run_launcher()
        self.assertEqual(2, result.returncode)
        self.assertIn('not found', result.stderr)
        self.env.pop('MOCK_DEPS')
        self.env['QT_QPA_PLATFORM'] = 'xcb'
        self.env.pop('XAUTHORITY', None)
        result = self.run_launcher()
        self.assertEqual(2, result.returncode)
        self.assertIn('XAUTHORITY', result.stderr)


if __name__ == '__main__':
    unittest.main()
