#!/usr/bin/env python3
import unittest
import errno
import stat
from unittest.mock import Mock, patch

from board_control_isolation_probe import (blocked_host_root, check_links, check_mounts,
                                          check_status, device_inventory, mount_rows)


class IsolationProofTest(unittest.TestCase):
    def test_host_pseudoterminal_and_block_devices_rejected(self):
        path = Mock()
        root = Mock()
        root.rglob.return_value = [path]
        path.lstat.return_value = Mock(st_mode=stat.S_IFCHR, st_rdev=0)
        with patch('board_control_isolation_probe.os.major', return_value=136, create=True), \
                patch('board_control_isolation_probe.os.minor', return_value=0, create=True), \
                self.assertRaises(ValueError):
            device_inventory(root)
        path.lstat.return_value = Mock(st_mode=stat.S_IFBLK)
        with self.assertRaises(ValueError):
            device_inventory(root)

    def test_safe_null_device_admitted(self):
        path = Mock()
        root = Mock()
        root.rglob.return_value = [path]
        path.lstat.return_value = Mock(st_mode=stat.S_IFCHR, st_rdev=0)
        with patch('board_control_isolation_probe.os.major', return_value=1, create=True), \
                patch('board_control_isolation_probe.os.minor', return_value=3, create=True):
            self.assertEqual(3, device_inventory(root)[0]['minor'])

    def test_host_root_requires_permission_denial(self):
        with patch('board_control_isolation_probe.os.stat', side_effect=PermissionError(errno.EACCES, 'denied')):
            self.assertEqual(errno.EACCES, blocked_host_root())
        for result in (None, FileNotFoundError(errno.ENOENT, 'missing')):
            with patch('board_control_isolation_probe.os.stat', side_effect=result), self.assertRaises(ValueError):
                blocked_host_root()

    def test_network_only_loopback_and_down_sit(self):
        links = [{'ifname': 'lo'}, {'ifname': 'sit0', 'flags': ['NOARP']}]
        check_links(links, [{'dev': 'lo', 'dst': '::1'}])
        for bad in ([{'ifname': 'lo'}, {'ifname': 'can0'}],
                    [{'ifname': 'lo'}, {'ifname': 'eth0'}],
                    [{'ifname': 'lo'}, {'ifname': 'sit0', 'flags': ['UP']}], []):
            with self.assertRaises(ValueError):
                check_links(bad, [])
        with self.assertRaises(ValueError):
            check_links(links, [{'dev': 'eth0', 'dst': 'default', 'table': 200}])

    def test_all_capability_sets_and_seccomp_checked(self):
        fields = dict.fromkeys(('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb'), '00000000')
        fields.update(NoNewPrivs='1', Seccomp='2')
        text = lambda rows: '\n'.join(key + ':\t' + value for key, value in rows.items())
        check_status(text(fields))
        for key in fields:
            bad = dict(fields)
            bad[key] = '00000001' if key.startswith('Cap') else '0'
            with self.assertRaises(ValueError):
                check_status(text(bad))

    def test_mountinfo_escapes_and_nested_writable_sysfs_rejected(self):
        rows = mount_rows('1 0 0:1 / /sys ro,nosuid - sysfs sysfs ro\n'
                          '2 0 0:2 / /run rw - tmpfs tmpfs rw\n'
                          '3 0 0:3 / /dev rw - tmpfs tmpfs rw\n'
                          '4 0 0:4 / /path\\040with\\134slash ro - tmpfs tmpfs ro\n')
        self.assertEqual(rows[3]['path'], '/path with\\slash')
        check_mounts(rows)
        with self.assertRaises(ValueError):
            check_mounts(rows + [{'path': '/sys/fs/cgroup', 'options': ['rw'], 'type': 'cgroup'}])
        for missing in ('/sys', '/run', '/dev'):
            with self.assertRaises(ValueError):
                check_mounts([row for row in rows if row['path'] != missing])


if __name__ == '__main__':
    unittest.main()
