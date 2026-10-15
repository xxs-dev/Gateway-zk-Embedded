import importlib.util
from pathlib import Path
import unittest
from unittest import mock


def load(name):
    path = Path(__file__).resolve().parents[1] / 'deploy' / (name + '.py')
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The profile is Linux-only; tests run on the target/build host.
installer = load('install-ems-kiosk')
runtime = load('gateway-ems-kiosk')


class KioskTests(unittest.TestCase):
    def test_preserves_unrelated_configuration(self):
        raw = '[daemon]\nWaylandEnable=true\nCustomOption=keep\n[debug]\nEnable=false\n'
        result = installer.ini_update(raw, {'daemon': {'WaylandEnable': 'false'}}).decode()
        self.assertIn('WaylandEnable=false', result)
        self.assertIn('CustomOption=keep', result)
        self.assertIn('Enable=false', result)

    def test_does_not_start_duplicate_or_interrupt_starting_unit(self):
        for state in ('active', 'activating', 'deactivating', 'reloading'):
            self.assertFalse(runtime.needs_start({'ActiveState': state}))
        self.assertFalse(runtime.needs_start({}))

    def test_restores_failed_and_cleanly_exited_ui(self):
        for state in ('failed', 'inactive'):
            self.assertTrue(runtime.needs_start({'ActiveState': state}))

    def test_rejects_unsafe_user(self):
        for user in ('root;reboot', '../szrl', 'szrl\nExecStart=x'):
            with self.assertRaises(ValueError):
                installer.profile(user, b'')

    def test_rejects_root_or_wrong_authority_uid(self):
        for uid in (0, 1001):
            with mock.patch.object(installer.pwd, 'getpwnam', return_value=mock.Mock(pw_uid=uid)):
                with self.assertRaises(ValueError):
                    installer.profile('szrl', b'')

    def test_no_business_units_or_authority_pins_are_edited(self):
        user = mock.Mock(pw_uid=1000, pw_gid=1000, pw_dir='/home/szrl')
        with mock.patch.object(installer.pwd, 'getpwnam', return_value=user), \
                mock.patch.object(Path, 'exists', return_value=False), \
                mock.patch.object(Path, 'is_symlink', return_value=False):
            files = installer.profile('szrl', b'helper')
        self.assertEqual(len(files), 8)
        for path in files:
            self.assertNotIn('/opt/modbus-gateway', str(path))
            self.assertNotIn('ky-ems.service', str(path))
            self.assertNotIn('gateway-services.service', str(path))
        desktop = files[Path('/usr/share/xsessions/gateway-ems-kiosk.desktop')][0]
        self.assertNotIn(b'gnome-session', desktop)
        self.assertNotIn(b'KY-EMS', desktop)
        self.assertIn(b'gateway-ems-kiosk.py session', desktop)
        watch = files[Path('/etc/systemd/system/gateway-ems-kiosk-watch.service')][0]
        self.assertIn(b'ProtectHome=tmpfs\nBindReadOnlyPaths=/run/user/1000\n', watch)
        self.assertIn(b'CapabilityBoundingSet=CAP_DAC_READ_SEARCH\n', watch)
        self.assertIn(b'PrivateDevices=yes\n', watch)

    def test_refuses_root_session(self):
        with mock.patch.object(runtime.os, 'getuid', return_value=0):
            with self.assertRaises(ValueError):
                runtime.session()

    def test_absent_session_marker_never_claims_readiness(self):
        with mock.patch.object(runtime.pwd, 'getpwnam', return_value=mock.Mock(pw_uid=1000)), \
                mock.patch.object(Path, 'lstat', side_effect=FileNotFoundError()):
            with self.assertRaises(FileNotFoundError):
                runtime.display_environment('szrl')

    def test_pending_maintenance_blocks_relaunch(self):
        for marker in runtime.INHIBIT_PATHS:
            with mock.patch.object(Path, 'exists', lambda path: str(path) == marker), \
                    mock.patch.object(Path, 'is_symlink', return_value=False), \
                    mock.patch.object(runtime, 'command') as command:
                self.assertTrue(runtime.maintenance_inhibited())
                command.assert_not_called()

    def test_dangling_maintenance_marker_blocks_relaunch(self):
        with mock.patch.object(Path, 'exists', return_value=False), \
                mock.patch.object(Path, 'is_symlink', return_value=True):
            self.assertTrue(runtime.maintenance_inhibited())

    def test_only_completed_active_launcher_allows_relaunch(self):
        with mock.patch.object(Path, 'exists', return_value=False), \
                mock.patch.object(Path, 'is_symlink', return_value=False):
            for active, sub, expected in (('active', 'exited', False),
                    ('activating', 'start', True), ('deactivating', 'stop', True),
                    ('inactive', 'dead', True), ('failed', 'failed', True)):
                output = 'ActiveState=' + active + '\nSubState=' + sub + '\n'
                with mock.patch.object(runtime, 'command', return_value=mock.Mock(returncode=0, stdout=output)):
                    self.assertEqual(runtime.maintenance_inhibited(), expected)

    def test_service_query_failure_blocks_relaunch(self):
        with mock.patch.object(Path, 'exists', return_value=False), \
                mock.patch.object(Path, 'is_symlink', return_value=False), \
                mock.patch.object(runtime, 'command', return_value=mock.Mock(returncode=1, stdout='')):
            self.assertTrue(runtime.maintenance_inhibited())

    def test_retained_reprovision_stop_is_not_live_maintenance(self):
        # First-runtime activation retains the stopped-install journal/STOP;
        # its production-unit gate, not that historical marker, owns startup.
        with mock.patch.object(Path, 'exists', lambda path: str(path) == '/opt/gateway-reprovision/STOP'), \
                mock.patch.object(Path, 'is_symlink', return_value=False), \
                mock.patch.object(runtime, 'command', return_value=mock.Mock(returncode=0,
                    stdout='ActiveState=active\nSubState=exited\n')):
            self.assertFalse(runtime.maintenance_inhibited())


if __name__ == '__main__':
    unittest.main()
