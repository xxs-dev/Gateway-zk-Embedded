"""Native Qt startup must not restart a whole OS target's inactive children."""
import configparser
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import uuid


SOURCE = Path(__file__).resolve().parents[1] / 'deploy/ky-ems-native.service.in'


class NativeQtTargetDependencyTest(unittest.TestCase):
    def test_native_qt_does_not_pull_graphical_target(self):
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        parser.read(SOURCE)
        for key in ('Wants', 'Requires', 'BindsTo'):
            self.assertNotIn('graphical.target', parser['Unit'].get(key, '').split())
        self.assertIn('graphical.target', parser['Install']['WantedBy'].split())

    @unittest.skipUnless(os.environ.get('GATEWAY_QT_TARGET_SYSTEMD_TEST') == '1',
                         'opt-in real manager probe using unique harmless units')
    def test_start_does_not_rerun_inactive_target_children(self):
        self.assertEqual(os.geteuid(), 0)
        prefix = 'gateway-qt-target-probe-' + uuid.uuid4().hex[:12]
        target, child, qt = prefix + '.target', prefix + '-init.service', prefix + '-qt.service'
        barrier = prefix + '-barrier.service'
        units = [qt, target, child, barrier]
        root = Path('/run/systemd/system')
        created = []
        def ctl(*args):
            return subprocess.check_output(['systemctl', *args], text=True, stderr=subprocess.STDOUT, timeout=30).strip()
        with tempfile.TemporaryDirectory(prefix=prefix + '-') as directory:
            marker = Path(directory) / 'starts'
            script = Path(directory) / 'init.sh'
            script.write_text('#!/bin/sh\nprintf x >> "' + str(marker) + '"\n')
            parser = configparser.ConfigParser(interpolation=None, strict=False)
            parser.optionxform = str
            parser.read(SOURCE)
            unit = '\n'.join(k + '=' + v.replace('graphical.target', target)
                             .replace('display-manager.service', prefix + '-display.service')
                             for k, v in parser['Unit'].items())
            bodies = {
                target: '[Unit]\nDefaultDependencies=no\nWants=' + child + ' ' + barrier + '\nAfter=' + child + ' ' + barrier + '\n',
                child: '[Service]\nType=oneshot\nPrivateNetwork=yes\nPrivateDevices=yes\n'
                       'CapabilityBoundingSet=\nNoNewPrivileges=yes\nExecStart=/bin/sh ' + str(script) + '\n',
                barrier: '[Service]\nType=oneshot\nRemainAfterExit=yes\nPrivateNetwork=yes\n'
                         'PrivateDevices=yes\nCapabilityBoundingSet=\nNoNewPrivileges=yes\nExecStart=/bin/sleep 5\n',
                qt: '[Unit]\n' + unit + '\n[Service]\nType=oneshot\nPrivateNetwork=yes\n'
                    'PrivateDevices=yes\nCapabilityBoundingSet=\nNoNewPrivileges=yes\nExecStart=/bin/true\n',
            }
            try:
                for name, body in bodies.items():
                    path = root / name
                    with path.open('x') as stream:
                        stream.write(body)
                    created.append(path)
                ctl('daemon-reload')
                ctl('start', '--no-block', target)
                for _ in range(60):
                    if marker.exists() and ctl('show', child, '-p', 'ActiveState', '--value') == 'inactive':
                        break
                    time.sleep(0.02)
                self.assertEqual(marker.read_bytes(), b'x')
                self.assertEqual(ctl('show', child, '-p', 'ActiveState', '--value'), 'inactive')
                self.assertNotEqual(ctl('show', target, '-p', 'ActiveState', '--value'), 'active')
                ctl('start', qt)
                self.assertEqual(marker.read_bytes(), b'x', 'Qt start reran a target child')
                ctl('start', qt)
                self.assertEqual(marker.read_bytes(), b'x', 'Qt restart reran a target child')
            finally:
                ctl('stop', *units)
                for path in created:
                    path.unlink()
                ctl('daemon-reload')


if __name__ == '__main__':
    unittest.main()
