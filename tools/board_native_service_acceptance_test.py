import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('board_native_service_acceptance',
                                             Path(__file__).with_name('board_native_service_acceptance.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NativeServiceAcceptanceTest(unittest.TestCase):
    def test_points_require_unique_integer_indexes(self):
        for rows in (None, {}, {'points': {}}, {'points': [None]},
                     {'points': [{'index': True}]}, {'points': [{'index': 1}, {'index': 1}]}):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                module.point_rows(rows)
        self.assertEqual(module.point_rows({'points': [{'index': 910002}]}), {910002: {'index': 910002}})

    def test_healthy_requires_fresh_good_quality_and_nonstale(self):
        good = {'quality': 1, 'stale': False, 'ts': 1000}
        self.assertTrue(module.healthy(good, 1100))
        for change in ({'quality': 0}, {'stale': True}, {'ts': 1101}, {'ts': 1.0}, {'ts': True}):
            self.assertFalse(module.healthy(dict(good, **change), 1100))
        self.assertFalse(module.healthy(good, 4000))

    def test_installed_entrypoints_not_test_substitutes(self):
        commands = module.commands(Path('/srv/gateway-native-services-test'))
        self.assertEqual(commands['ui'], ['/bin/sh', str(module.HOME / 'bin/gateway-qt-native-run.sh')])
        self.assertEqual(commands['driver'][0], str(module.HOME / 'bin/ModbusRtu'))
        self.assertEqual(commands['monitor'][0], str(module.HOME / 'bin/SystemMonitor'))
        self.assertNotIn('write', commands['driver'])


if __name__ == '__main__':
    unittest.main()
