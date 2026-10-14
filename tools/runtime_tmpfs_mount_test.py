"""Visible mount identity tests, plus an opt-in private real-kernel check."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


def load():
    spec = importlib.util.spec_from_file_location('tmpfs_mount_fixture',
        Path(__file__).resolve().parents[1] / 'deploy/offline-runtime-upgrade.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(os.name == 'posix', 'Linux kernel mount semantics')
class TmpfsMountTest(unittest.TestCase):
    def setUp(self):
        self.m = load()
        temp = tempfile.TemporaryDirectory(prefix='tmpfs-mount-')
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / 'shared memory'
        self.path.mkdir()
        device = self.path.stat().st_dev
        escaped = str(self.path).replace(' ', r'\040')
        self.row = '20 1 %d:%d / %s rw - {kind} visible rw\n' % (os.major(device), os.minor(device), escaped)
        self.mountinfo = self.row.format(kind='tmpfs')
        self.fdinfo = 'pos:\t0\nflags:\t010000000\nmnt_id:\t20\n'
        self.seen_fd = None

    def regular(self, path):
        self.assertEqual(path.parts[:3], ('/', 'proc', str(os.getpid())))
        if path.name == 'mountinfo':
            return mock.Mock(read_text=lambda: self.mountinfo)
        self.assertEqual(path.parent.name, 'fdinfo')
        self.seen_fd = int(path.name)
        os.fstat(self.seen_fd)
        return mock.Mock(read_text=lambda: self.fdinfo)

    def check(self):
        with mock.patch.object(self.m, 'regular', self.regular):
            self.m.tmpfs_mount(self.path)

    def test_visible_tmpfs_over_hidden_layer_and_escaped_path(self):
        self.mountinfo = self.row.format(kind='ext4').replace('20 ', '10 ', 1) + self.mountinfo
        self.check()
        with self.assertRaises(OSError):
            os.fstat(self.seen_fd)

    def test_hidden_tmpfs_does_not_admit_visible_non_tmpfs(self):
        self.mountinfo = self.mountinfo.replace('20 ', '10 ', 1) + self.row.format(kind='ext4')
        with self.assertRaisesRegex(ValueError, 'exact tmpfs'):
            self.check()
        with self.assertRaises(OSError):
            os.fstat(self.seen_fd)

    def test_missing_duplicate_wrong_device_and_wrong_path_rows_refuse(self):
        valid = self.mountinfo
        wrong_device = valid.split()
        wrong_device[2] = '999:999'
        wrong_path = valid.split()
        wrong_path[4] = '/unreviewed'
        for value in ('', valid + valid, valid.replace('20 ', '30 ', 1),
                      ' '.join(wrong_device), ' '.join(wrong_path)):
            with self.subTest(value=value):
                self.mountinfo = value
                with self.assertRaisesRegex(ValueError, 'exact tmpfs'):
                    self.check()

    def test_missing_or_invalid_fd_mount_identity_refuses(self):
        for value in ('pos:\t0\n', 'mnt_id:\t0\n', 'mnt_id:\t-1\n', 'mnt_id:\tother\n'):
            with self.subTest(value=value):
                self.fdinfo = value
                with self.assertRaisesRegex(ValueError, 'mount identity'):
                    self.check()

    def test_symlink_is_refused_without_following_it(self):
        alias = self.path.parent / 'alias'
        alias.symlink_to(self.path, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'canonical tmpfs'):
            self.m.tmpfs_mount(alias)

    @unittest.skipUnless(os.environ.get('GATEWAY_TEST_TMPFS_MOUNT'), 'opt-in private kernel mount test')
    def test_real_procfs_and_private_overmounted_tmpfs(self):
        rows = [line.split() for line in Path('/proc/self/mountinfo').read_text().splitlines()]
        self.assertTrue(any(r[4] == '/dev/shm' and r[r.index('-') + 1] == 'tmpfs' and
                            r[r.index('-') + 2].startswith('gateway-pointctl-empty-test-') for r in rows))
        self.assertNotEqual(os.readlink('/proc/self/ns/mnt'), os.readlink('/proc/1/ns/mnt'))
        self.m.tmpfs_mount(Path('/dev/shm'))
        with self.assertRaisesRegex(ValueError, 'exact tmpfs'):
            self.m.tmpfs_mount(self.path)


if __name__ == '__main__':
    unittest.main()
