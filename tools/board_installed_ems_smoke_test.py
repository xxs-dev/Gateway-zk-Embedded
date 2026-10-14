import hashlib
from pathlib import Path
import signal
import subprocess
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import board_installed_ems_smoke as smoke


class InstalledSmokeTest(unittest.TestCase):
    def test_native_probe_never_selects_legacy_bridge(self):
        self.assertEqual(['monitor', 'ems'], [name for name, _ in smoke.commands('native-direct-probe')])
        self.assertEqual(['monitor', 'bridge', 'ems'],
                         [name for name, _ in smoke.commands('legacy-bridge')])
        with self.assertRaisesRegex(ValueError, 'unknown display probe'):
            smoke.commands('unknown')

    def test_native_probe_rejects_legacy_segment(self):
        smoke.check_sysv('key shmid owner\n', 'native-direct-probe')
        with self.assertRaisesRegex(ValueError, 'unexpected legacy'):
            smoke.check_sysv('key shmid owner\n4660 1 root\n', 'native-direct-probe')
        smoke.check_sysv('key shmid owner\n4660 1 root\n', 'legacy-bridge')
        with self.assertRaisesRegex(ValueError, 'did not create'):
            smoke.check_sysv('key shmid owner\n', 'legacy-bridge')

    def test_varlist_is_immutable_unless_legacy_probe_explicitly_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'ky-ems').mkdir()
            (root / 'ky-ems/VarList.xml').write_bytes(b'changed')
            approval = {'imageTree': {'ky-ems/VarList.xml': {
                'kind': 'file', 'sha256': hashlib.sha256(b'original').hexdigest()}}}
            with patch.object(smoke, 'HOME', root):
                with self.assertRaisesRegex(ValueError, 'image drift'):
                    smoke.check_image(approval)
                smoke.check_image(approval, generated_varlist=True)

    def test_failed_isolation_never_starts_programs(self):
        args = types.SimpleNamespace(seconds=45)
        with patch.object(smoke.argparse.ArgumentParser, 'parse_args', return_value=args), \
                patch.object(smoke.isolation, 'probe', side_effect=ValueError('isolation failed')), \
                patch.object(smoke.subprocess, 'Popen') as start:
            with self.assertRaisesRegex(ValueError, 'isolation failed'):
                smoke.main()
            start.assert_not_called()

    def test_unbounded_run_rejected_before_probe(self):
        for seconds in (0, 121):
            with patch.object(smoke.argparse.ArgumentParser, 'parse_args',
                              return_value=types.SimpleNamespace(seconds=seconds)), \
                    patch.object(smoke.isolation, 'probe') as probe:
                with self.assertRaisesRegex(ValueError, 'bounded'):
                    smoke.main()
                probe.assert_not_called()

    def test_image_tamper_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'bin').mkdir()
            path = root / 'bin/SystemMonitor'
            path.write_bytes(b'approved')
            approval = {'imageTree': {'bin/SystemMonitor': {
                'kind': 'file', 'sha256': hashlib.sha256(b'approved').hexdigest()}}}
            with patch.object(smoke, 'HOME', root):
                smoke.check_image(approval)
                path.write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'image drift'):
                    smoke.check_image(approval)

    def test_exited_child_is_not_stable(self):
        process = Mock(pid=123)
        process.poll.return_value = 1
        with self.assertRaisesRegex(ValueError, 'program exited'):
            smoke.snapshot([('ems', process)], None)

    def test_child_must_have_private_namespaces_and_zero_caps(self):
        process = Mock(pid=123)
        process.poll.return_value = None
        fields = dict.fromkeys(('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb'), '0000')
        fields.update(NoNewPrivs='1', Seccomp='2')
        text = lambda: '\n'.join(k + ':' + v for k, v in fields.items())
        args = types.SimpleNamespace(host_netns='net:[1]', shared_netns='net:[2]',
                                     host_ipcns='ipc:[1]', shared_ipcns='ipc:[2]')
        paths = {'/proc/123/ns/net': 'net:[2]', '/proc/123/ns/ipc': 'ipc:[2]',
                 '/proc/123/exe': '/opt/modbus-gateway/ky-ems/KY-EMS'}
        with patch.object(smoke.Path, 'read_text', side_effect=lambda: text()), \
                patch.object(smoke.os, 'readlink', side_effect=lambda p: paths[Path(p).as_posix()]):
            self.assertIn('ems', smoke.snapshot([('ems', process)], args))
            fields['CapEff'] = '1'
            with self.assertRaisesRegex(ValueError, 'capabilities remain'):
                smoke.snapshot([('ems', process)], args)
            fields['CapEff'] = '0'
            paths['/proc/123/ns/ipc'] = args.host_ipcns
            with self.assertRaisesRegex(ValueError, 'host IPC'):
                smoke.snapshot([('ems', process)], args)

    def test_stop_terminates_own_groups(self):
        process = Mock(pid=123)
        process.poll.return_value = None
        with patch.object(smoke.os, 'killpg', create=True) as kill:
            self.assertEqual([], smoke.terminate([('ems', process)]))
            kill.assert_called_once_with(123, signal.SIGTERM)
        process.wait.assert_called_once_with(timeout=8)

    def test_forced_stop_is_reported_as_failure(self):
        process = Mock(pid=123)
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired('ems', 8), -9]
        with patch.object(smoke.os, 'killpg', create=True), \
                patch.object(smoke.signal, 'SIGKILL', 9, create=True):
            self.assertEqual(['ems required SIGKILL'], smoke.terminate([('ems', process)]))


if __name__ == '__main__':
    unittest.main()
