"""Capacity gates through real local apply; no device, daemon or native migration."""
import contextlib
import io
import json
import os
import types
import unittest
from unittest import mock

import core_readonly_activation_test as core


class CapacityPreflightTest(unittest.TestCase):
    def setUp(self):
        self.fixture = core.CoreReadOnlyTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.f = self.fixture.f
        self.output = io.StringIO()
        self.page = os.sysconf('SC_PAGE_SIZE')
        self.v10, self.v11 = 15174272, 15667032
        rounded = lambda size: ((size + self.page - 1) // self.page) * self.page
        self.target_bytes = rounded(self.v11)
        self.work_bytes = 2 * rounded(self.v10) + 2 * rounded(self.v11)
        self.reserve_bytes = 64 * 1024 * 1024

    def budget(self):
        added = len(self.f.approval['segments']) * self.target_bytes
        return added + self.work_bytes + self.reserve_bytes, added

    def capacity(self, memory=None, shm=None):
        required, added = self.budget()
        core.write(self.f.path('/proc/meminfo'), 'MemAvailable: %d kB\n' %
                   ((required if memory is None else memory) // 1024))
        free = added if shm is None else shm
        self.f.patch(self.f.m.os, 'statvfs', lambda path:
                     types.SimpleNamespace(f_frsize=self.page, f_bavail=free // self.page))

    def snapshot(self):
        result = {}
        for path in sorted((self.f.root / 'system').rglob('*')):
            relative = str(path.relative_to(self.f.root))
            info = path.lstat()
            if path.is_symlink(): value = ('link', os.readlink(path))
            elif path.is_file(): value = ('file', core.sha(path), info.st_ino, info.st_mode)
            else: value = ('directory', info.st_ino, info.st_mode)
            result[relative] = value
        return result

    def apply(self):
        with contextlib.redirect_stdout(self.output):
            return self.f.apply()

    def refused_without_mutation(self, message):
        before = self.snapshot()
        with mock.patch.object(self.f.m, 'write_new', wraps=self.f.m.write_new) as writes, \
                mock.patch.object(self.f.m, 'replace', wraps=self.f.m.replace) as replacements:
            with self.assertRaisesRegex(ValueError, message):
                self.apply()
            writes.assert_not_called()
            replacements.assert_not_called()
        self.assertFalse(self.f.state_dir.exists())
        self.assertEqual(before, self.snapshot())
        self.assertFalse(any(call[0] in ('stop', 'start', 'daemon-reload') for call in self.f.calls))

    def reports(self):
        return [json.loads(line.split(': ', 1)[1]) for line in self.output.getvalue().splitlines()
                if line.startswith('CAPACITY_PRECHECK: ')]

    def test_insufficient_memory_refuses_real_apply_without_mutation(self):
        required, _ = self.budget()
        for available in (required - 1024, 0):
            with self.subTest(available=available):
                self.capacity(memory=available)
                self.refused_without_mutation('insufficient MemAvailable')

    def test_exact_boundary_multiple_segments_counts_full_targets_and_single_workspace(self):
        self.assertGreater(len(self.f.approval['segments']), 1)
        self.capacity()
        self.assertEqual('UPGRADED_STOPPED', self.apply()['phase'])
        reports = self.reports()
        self.assertEqual(['initial', 'before-state'], [r['checkpoint'] for r in reports])
        required, added = self.budget()
        for report in reports:
            self.assertEqual(required, report['memRequiredBytes'])
            self.assertEqual(added, report['shmRequiredBytes'])
            self.assertEqual(self.work_bytes, report['migrationWorkBytes'])
            self.assertEqual(self.reserve_bytes, report['systemReserveBytes'])
            self.assertEqual(len(self.f.approval['segments']) * self.v10, report['retainedSourceV10Bytes'])
            self.assertEqual(required, report['memAvailableBytes'])
            self.assertEqual(added, report['shmAvailableBytes'])

    def test_meminfo_missing_refuses_without_mutation(self):
        self.f.path('/proc/meminfo').unlink()
        self.refused_without_mutation('cannot read MemAvailable')

    def test_meminfo_malformed_or_missing_field_refuses_without_mutation(self):
        for body in ('MemFree: 2097152 kB\n', 'MemAvailable: garbage kB\n',
                     'MemAvailable: -1 kB\n', 'MemAvailable: 123 MB\n',
                     'MemAvailable: 123\n', 'MemAvailable: 123 kB\nMemAvailable: 456 kB\n'):
            with self.subTest(body=body):
                core.write(self.f.path('/proc/meminfo'), body)
                self.refused_without_mutation('invalid or missing MemAvailable')

    def test_meminfo_read_error_refuses_without_mutation(self):
        self.f.path('/proc/meminfo').unlink()
        self.f.path('/proc/meminfo').mkdir()
        self.refused_without_mutation('cannot read MemAvailable')

    def test_tmpfs_one_page_short_refuses_without_mutation(self):
        _, added = self.budget()
        self.capacity(shm=added - self.page)
        self.refused_without_mutation('insufficient /dev/shm')

    def test_tmpfs_error_or_invalid_metadata_refuses_without_mutation(self):
        values = (OSError('synthetic statvfs failure'),
                  types.SimpleNamespace(f_frsize=0, f_bavail=10),
                  types.SimpleNamespace(f_frsize=4096, f_bavail=-1))
        for value in values:
            with self.subTest(value=value):
                with mock.patch.object(self.f.m.os, 'statvfs',
                                       side_effect=value if isinstance(value, OSError) else None,
                                       return_value=value):
                    self.refused_without_mutation('cannot inspect /dev/shm|invalid /dev/shm capacity')

    def test_capacity_drift_at_final_prestate_check_refuses_without_mutation(self):
        self.capacity()
        original = self.f.m.os.statvfs
        calls = []
        def drifting(path):
            calls.append(path)
            if len(calls) == 2:
                return types.SimpleNamespace(f_frsize=self.page, f_bavail=0)
            return original(path)
        with mock.patch.object(self.f.m.os, 'statvfs', side_effect=drifting):
            self.refused_without_mutation('insufficient /dev/shm')
        self.assertEqual(2, len(calls))

    def test_duplicate_segments_do_not_create_state(self):
        self.f.approval['segments'].append(dict(self.f.approval['segments'][0]))
        self.refused_without_mutation('distinct unique source and target')

    def test_monitor_only_two_segments_exact_boundary(self):
        self.fixture.pinned_custom_monitor()
        f = self.f
        keep = {'gateway_point_store', 'gateway_point_store_COMM202600998_monitor'}
        remap = {s['source']: 'gateway_point_store' for s in f.approval['segments'] if s['source'] not in keep}
        for path in (f.config / 'apps').glob('*.json'):
            core.write(path, json.dumps(f.m.switch_names(json.loads(path.read_text()), remap)))
        f.approval['segments'] = [s for s in f.approval['segments'] if s['source'] in keep]
        self.fixture.refresh_config()
        self.assertEqual(2, len(self.f.approval['segments']))
        self.capacity()
        self.assertEqual('UPGRADED_STOPPED', self.apply()['phase'])
        required, added = self.budget()
        self.assertEqual(required, self.reports()[-1]['memRequiredBytes'])
        self.assertEqual(added, self.reports()[-1]['shmRequiredBytes'])

    def test_formal_main_holds_existing_lock_during_both_capacity_checks(self):
        f = self.f
        self.capacity()
        approval = f.root / 'approval.json'
        core.write(approval, json.dumps(f.approval))
        lock = f.path('/run/gateway-offline-runtime.lock')
        core.write(lock, '')
        original = f.m.os.statvfs
        checks = []
        def locked_capacity(path):
            fd = os.open(lock, os.O_RDWR)
            try:
                with self.assertRaises(BlockingIOError):
                    f.m.fcntl.flock(fd, f.m.fcntl.LOCK_EX | f.m.fcntl.LOCK_NB)
            finally:
                os.close(fd)
            checks.append(path)
            return original(path)
        argv = ['offline-runtime-upgrade.py', 'apply', '--approval', str(approval),
                '--approval-sha256', core.sha(approval), '--manifest', str(f.root / 'manifest.json'),
                '--payload', str(f.root / 'payload'), '--state', str(f.state_dir)]
        with mock.patch.object(f.m.os, 'statvfs', side_effect=locked_capacity), \
                mock.patch.object(f.m.os, 'geteuid', return_value=0), \
                mock.patch.object(f.m.sys, 'argv', argv), contextlib.redirect_stdout(self.output):
            f.m.main()
        self.assertEqual(2, len(checks))
        self.assertEqual('UPGRADED_STOPPED', json.loads((f.state_dir / 'state.json').read_text())['phase'])
        fd = os.open(lock, os.O_RDWR)
        try:
            f.m.fcntl.flock(fd, f.m.fcntl.LOCK_EX | f.m.fcntl.LOCK_NB)
        finally:
            os.close(fd)

    def test_successor_retained_targets_are_not_charged_twice(self):
        self.fixture.historical_predecessor()
        self.f.calls.clear()
        self.capacity()
        self.assertEqual('UPGRADED_STOPPED', self.apply()['phase'])
        required, added = self.budget()
        self.assertEqual(required, self.reports()[-1]['memRequiredBytes'])
        self.assertEqual(added, self.reports()[-1]['shmRequiredBytes'])


if __name__ == '__main__':
    unittest.main()
