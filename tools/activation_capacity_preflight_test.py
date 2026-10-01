"""Local activation admission; real entry paths, fake systemctl/native migration."""
import contextlib
import io
import json
import os
from pathlib import Path
import unittest
from unittest import mock

import a_acquisition_activation_test as legacy
import core_readonly_activation_test as core


class ActivationCapacityTest(unittest.TestCase):
    def prepare(self, profile='core999'):
        if profile.startswith('core') or profile == 'B_MONITOR':
            owner = core.CoreReadOnlyTest()
            owner.setUp()
            self.addCleanup(owner.doCleanups)
            f = owner.f
            if profile in ('core998', 'B_MONITOR'):
                owner.pinned_custom_monitor()
            readers = owner.readers() if profile == 'core999' else [f.g.MONITOR_UNIT]
            if profile == 'B_MONITOR':
                del f.approval['coreReadOnlyTest']
                f.approval['activationStartupScripts'] = {k: v for k, v in
                    f.approval['activationStartupScripts'].items() if k in f.m.ACTIVATION_SCRIPTS}
                source = 'gateway_point_store_COMM202600998_monitor'
                for name in ('camera-service.json', 'monitor-service.json', 'mqtt-service.json'):
                    doc = {'deviceConfigFiles': [],
                           'mqttDriver': {'sharedMemoryName': source, 'sharedMemoryNames': [source]},
                           'cameraService': {'enabled': False, 'sharedMemoryName': source}}
                    if name != 'camera-service.json':
                        doc['mqttDriver']['enabled'] = False
                        doc['computeEngine'] = {'enabled': False, 'sharedMemoryNames': [source],
                                                'outputDefaultSharedMemoryName': source}
                        doc['localDisplay'] = {'enabled': False, 'sharedMemoryNames': [source]}
                    if name == 'monitor-service.json': doc['systemMonitor'] = {'enabled': True}
                    legacy.write(f.config / 'apps' / name, json.dumps(doc))
                f.approval['segments'] = [s for s in f.approval['segments'] if s['source'] in ('gateway_point_store', source)]
                owner.refresh_config()
                pin = next(c['sha256'] for c in json.loads((f.root / 'manifest.json').read_text())['components']
                           if c['target'] == 'SystemMonitor')
                f.g.MONITOR_BINARY = f.m.B_MONITOR_BINARY_SHA256 = pin
        else:
            f = legacy.AcquisitionTest()
            f.setUp()
            self.addCleanup(f.doCleanups)
            readers = f.g.A_ACQUISITION_READERS
            if profile == 'A_MONITOR_QT':
                del f.approval['aReadonlyAcquisition']
                readers = [f.g.MONITOR_UNIT, f.g.QT_UNIT]
            elif profile == 'A_WINDOWS':
                f.approval['aReadonlyWindowsIntegration'] = True
        self.f, self.readers = f, readers
        self.output = io.StringIO()
        with contextlib.redirect_stdout(self.output): f.apply()
        # Generate separately pinned ready input, without invoking activation yet.
        with mock.patch.object(f.m, 'activate'):
            f.activate(readers)
        f.calls.clear()
        self.allowed = f.m.activation_units(f.approval, readers)

    def required(self):
        f = self.f
        mib = {f.g.MONITOR_UNIT: 128, f.g.A_RTU_UNIT: 128, f.g.A_MQTT_UNIT: 128,
               f.g.QT_UNIT: 256, 'gateway-services.service': 32, 'gateway-health-watchdog.service': 32}
        return (sum(mib[u] for u in self.allowed) + (64 if f.g.QT_UNIT in self.allowed else 0) + 64) * 1024**2

    def memory(self, available=None):
        legacy.write(self.f.path('/proc/meminfo'), 'MemAvailable: %d kB\n' %
                     ((self.required() if available is None else available) // 1024))

    def snapshot(self):
        rows = {}
        for p in self.f.root.rglob('*'):
            info = p.lstat()
            value = ('link', os.readlink(p)) if p.is_symlink() else \
                    ('file', legacy.sha(p)) if p.is_file() else ('directory',)
            rows[str(p.relative_to(self.f.root))] = (value, info.st_ino, info.st_mode)
        return rows

    def activate(self):
        with contextlib.redirect_stdout(self.output): self.f.m.activate(self.f.args)

    def refused(self, message):
        f = self.f
        before = self.snapshot()
        with mock.patch.object(f.m, 'write_new', wraps=f.m.write_new) as writes, \
                mock.patch.object(f.m, 'save', wraps=f.m.save) as saves, \
                mock.patch.object(f.m, 'replace', wraps=f.m.replace) as replacements:
            with self.assertRaisesRegex(ValueError, message): self.activate()
            writes.assert_not_called();saves.assert_not_called();replacements.assert_not_called()
        self.assertEqual(before, self.snapshot())
        self.assertEqual('UPGRADED_STOPPED', json.loads((f.state_dir / 'state.json').read_text())['phase'])
        self.assertFalse((f.state_dir / 'activation-ready.json').exists())
        self.assertFalse((f.home / 'data' / f.g.ACTIVATION_FILE).exists())
        self.assertFalse(f.live)
        self.assertFalse(any(c[0] in ('start', 'stop', 'daemon-reload') for c in f.calls))

    def reports(self):
        return [json.loads(line.split(': ', 1)[1]) for line in self.output.getvalue().splitlines()
                if line.startswith('ACTIVATION_CAPACITY_PRECHECK: ')]

    def test_insufficient_activation_memory_preserves_stopped_state_and_ready(self):
        self.prepare()
        for available in (self.required() - 1024, 0):
            with self.subTest(available=available):
                self.memory(available)
                self.refused('insufficient MemAvailable for activation')

    def test_core_three_readers_exact_boundary_no_shm_or_cli_charge(self):
        self.prepare()
        self.memory()
        with mock.patch.object(self.f.m.os, 'statvfs', side_effect=AssertionError('activation must not budget tmpfs')):
            self.activate()
        self.assertEqual(['initial', 'before-ready'], [r['checkpoint'] for r in self.reports()])
        for row in self.reports():
            self.assertEqual(480 * 1024**2, row['memRequiredBytes'])
            self.assertEqual(self.required(), row['memAvailableBytes'])
            self.assertEqual(set(self.allowed), set(row['serviceReserveBytes']))
        self.assertEqual(set(self.allowed), self.f.live)

    def test_monitor_only_exact_boundary(self):
        self.prepare('core998')
        self.memory();self.activate()
        self.assertEqual(224 * 1024**2, self.reports()[-1]['memRequiredBytes'])
        self.assertEqual(set(self.allowed), self.f.live)

    def test_missing_meminfo_preserves_stopped_files(self):
        self.prepare()
        self.f.path('/proc/meminfo').unlink()
        self.refused('cannot read MemAvailable')

    def test_invalid_meminfo_preserves_stopped_files(self):
        self.prepare()
        for text in ('MemFree: 9000000 kB\n', 'MemAvailable: bad kB\n', 'MemAvailable: -1 kB\n',
                     'MemAvailable: 9000000 MB\n', 'MemAvailable: 1 kB\nMemAvailable: 2 kB\n'):
            with self.subTest(text=text):
                legacy.write(self.f.path('/proc/meminfo'), text)
                self.refused('invalid or missing MemAvailable')

    def test_meminfo_io_error_preserves_stopped_files(self):
        self.prepare()
        p = self.f.path('/proc/meminfo');p.unlink();p.mkdir()
        self.refused('cannot read MemAvailable')

    def test_memory_drift_before_first_persist_preserves_ready_and_fences(self):
        self.prepare();self.memory()
        original = Path.read_text
        reads = []
        def drift(path, *args, **kwargs):
            if path == self.f.path('/proc/meminfo'):
                reads.append(str(path))
                if len(reads) == 2: return 'MemAvailable: 0 kB\n'
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, 'read_text', new=drift): self.refused('insufficient MemAvailable for activation')
        self.assertEqual(2, len(reads))

    def test_formal_activate_main_holds_lock_for_both_checks(self):
        self.prepare();self.memory()
        f = self.f
        lock = f.path('/run/gateway-offline-runtime.lock');legacy.write(lock, '')
        original = Path.read_text
        reads = []
        def locked(path, *args, **kwargs):
            if path == f.path('/proc/meminfo'):
                fd = os.open(lock, os.O_RDWR)
                try:
                    with self.assertRaises(BlockingIOError): f.m.fcntl.flock(fd, f.m.fcntl.LOCK_EX | f.m.fcntl.LOCK_NB)
                finally: os.close(fd)
                reads.append(str(path))
            return original(path, *args, **kwargs)
        argv = ['offline-runtime-upgrade.py', 'activate', '--approval-sha256', f.args.approval_sha256,
                '--state', str(f.state_dir), '--ready', str(f.args.ready), '--ready-sha256', f.args.ready_sha256]
        with mock.patch.object(Path, 'read_text', new=locked), mock.patch.object(f.m.sys, 'argv', argv), \
                mock.patch.object(f.m.os, 'geteuid', return_value=0), contextlib.redirect_stdout(self.output): f.m.main()
        self.assertEqual(2, len(reads))

    def test_old_acquisition_profile_exact_boundary(self):
        self.prepare('A_ACQUISITION');self.memory();self.activate()
        self.assertEqual(800 * 1024**2, self.reports()[-1]['memRequiredBytes'])
        self.assertEqual(64 * 1024**2, self.reports()[-1]['serviceReserveBytes']['qt-display-bridge.service'])

    def test_old_windows_profile_exact_boundary(self):
        self.prepare('A_WINDOWS');self.memory();self.activate()
        self.assertEqual(self.required(), self.reports()[-1]['memRequiredBytes'])

    def test_old_joint_qt_profile_exact_boundary(self):
        self.prepare('A_MONITOR_QT');self.memory();self.activate()
        self.assertEqual(544 * 1024**2, self.reports()[-1]['memRequiredBytes'])

    def test_old_b_monitor_profile_exact_boundary(self):
        self.prepare('B_MONITOR');self.memory();self.activate()
        self.assertEqual(224 * 1024**2, self.reports()[-1]['memRequiredBytes'])

    def test_b_monitor_optional_watchdog_policy_adds_only_its_allowance(self):
        self.prepare('B_MONITOR')
        approval = dict(self.f.approval)
        approval['units'] = approval['units'] + ['gateway-health-watchdog.service']
        self.allowed = self.f.m.activation_units(approval, self.readers)
        self.memory()
        before = self.snapshot()
        with contextlib.redirect_stdout(self.output):
            self.f.m.activation_capacity_preflight(self.allowed, 'watchdog-policy')
        self.assertEqual(256 * 1024**2, self.reports()[-1]['memRequiredBytes'])
        self.assertEqual(32 * 1024**2, self.reports()[-1]['serviceReserveBytes']['gateway-health-watchdog.service'])
        self.assertEqual(before, self.snapshot())
        self.assertFalse(self.f.live)

    def test_unknown_or_duplicate_policy_units_refused_without_mutation(self):
        self.prepare('core998')
        before = self.snapshot()
        for units in (self.allowed + ['unapproved.service'], self.allowed + [self.allowed[0]], []):
            with self.subTest(units=units):
                with self.assertRaisesRegex(ValueError, 'known unique units'):
                    self.f.m.activation_capacity_preflight(units, 'invalid-policy')
                self.assertEqual(before, self.snapshot())
        self.assertFalse(self.f.live)


if __name__ == '__main__': unittest.main()
