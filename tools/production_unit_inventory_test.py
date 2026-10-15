"""Filtered systemd inventory must retain the complete existing admission scope."""
import fnmatch
import importlib.util
from pathlib import Path
import re
import subprocess
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('inventory_guard', ROOT / 'deploy/runtime-upgrade-guard.py')
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)
FAMILIES = ('modbus-rtu dlt645-driver dio-driver can-driver iec-driver compute-engine agc-avc '
            'ems-cluster event-engine local-display local-display-qt local-kiosk system-monitor '
            'camera-service mqtt-tls-tunnel mqtt-driver mqtt-forwarder').split()
SINGLES = ('gateway-services gateway-health-watchdog qt-display-bridge ky-ems').split()
LEGACY = re.compile(r'(?:(?:' + '|'.join(FAMILIES) + r')@[A-Za-z0-9_.:-]*|' +
                    '|'.join(SINGLES) + r')\.service\Z')


class ProductionUnitInventoryTest(unittest.TestCase):
    def test_filtered_queries_cover_full_families_not_only_approved_instances(self):
        calls = []
        def query(argv, **kwargs):
            calls.append(argv)
            self.assertEqual(argv[:6], ['systemctl', argv[1], '--all', '--no-legend', '--no-pager', '--plain'])
            self.assertEqual(set(argv[6:]), {name + '@*.service' for name in FAMILIES} |
                             {name + '.service' for name in SINGLES})
            self.assertEqual(kwargs['timeout'], 45)
            return subprocess.CompletedProcess(argv, 0, 'ky-ems.service loaded\n', '')
        with mock.patch.object(guard.subprocess, 'run', query):
            guard.production_units_present(['ky-ems.service'])
        self.assertEqual([call[1] for call in calls], ['list-units', 'list-unit-files'])

    def test_every_legacy_name_is_covered_and_grammar_is_unchanged(self):
        candidates = [name + '.service' for name in SINGLES]
        for family in FAMILIES + ['foreign']:
            for instance in ('', 'new', 'a_B-9.test:2', 'OLD', 'a/b', 'a b', 'a\\x20b', '*'):
                for suffix in ('.service', '.socket', '.service\n'):
                    candidates.append(family + '@' + instance + suffix)
        for name in candidates:
            with self.subTest(name=name):
                self.assertEqual(bool(guard.PRODUCTION_UNIT.fullmatch(name)), bool(LEGACY.fullmatch(name)))
                if LEGACY.fullmatch(name):
                    self.assertTrue(any(fnmatch.fnmatchcase(name, pattern) for pattern in guard.PRODUCTION_UNIT_PATTERNS))

    def test_unknown_instances_templates_and_singletons_remain_rejected(self):
        names = [family + '@unexpected.service' for family in FAMILIES]
        names += [family + '@.service' for family in FAMILIES]
        names += [name + '.service' for name in SINGLES]
        for action in ('list-units', 'list-unit-files'):
            for name in names:
                with self.subTest(action=action, name=name):
                    def query(argv, **kwargs):
                        text = name + ' loaded\n' if argv[1] == action else ''
                        return subprocess.CompletedProcess(argv, 0, text, '')
                    with mock.patch.object(guard.subprocess, 'run', query):
                        with self.assertRaisesRegex(ValueError, 'uncovered production participant'):
                            guard.production_units_present([])

    def test_inventory_is_fresh_and_command_failures_are_not_ignored(self):
        good = subprocess.CompletedProcess([], 0, 'ky-ems.service loaded\n', '')
        foreign = subprocess.CompletedProcess([], 0, 'modbus-rtu@unapproved.service loaded\n', '')
        with mock.patch.object(guard.subprocess, 'run', side_effect=[good, good, foreign]):
            guard.production_units_present(['ky-ems.service'])
            with self.assertRaisesRegex(ValueError, 'uncovered production participant'):
                guard.production_units_present(['ky-ems.service'])
        with mock.patch.object(guard.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'failed')):
            with self.assertRaisesRegex(ValueError, 'cannot inventory'):
                guard.production_units_present(['ky-ems.service'])
        with mock.patch.object(guard.subprocess, 'run', side_effect=subprocess.TimeoutExpired('systemctl', 45)):
            with self.assertRaises(subprocess.TimeoutExpired):
                guard.production_units_present(['ky-ems.service'])


if __name__ == '__main__':
    unittest.main()
