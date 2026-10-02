"""Local parser/file-policy tests, NOT Linux mounts/fork or AArch64 execution."""
import ast
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, HERE/path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


env = load('B1_env', 'environment.py')
outer = load('B1_outer', 'board-run.py')
inner = load('B1_inner', 'namespace-child.py')
entry = load('B1_entry', 'execute-board.py')


class BoardTests(unittest.TestCase):
    def test_export_versions_not_undefined_or_hidden(self):
        raw = b' 1: 00000001 4 FUNC GLOBAL DEFAULT 12 fn@@GLIBC_2.17\n 2: 0 0 FUNC GLOBAL DEFAULT UND absent@GLIBC_2.17\n 3: 1 4 FUNC GLOBAL HIDDEN 12 hidden\n'
        self.assertEqual(env.exported_symbols(raw), {'fn', 'fn@GLIBC_2.17'})

    def test_loader_not_found_rejected(self):
        with self.assertRaises(AssertionError): env.listed_libraries(b'libc.so.6 => not found\n')

    def test_unknown_loader_listing_rejected(self):
        with self.assertRaises(AssertionError): env.listed_libraries(b'noise\n')

    def test_loader_expected_shape(self):
        self.assertEqual(env.listed_libraries(b'linux-vdso.so.1 (0x1)\nlibc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x2)\n/lib/ld-linux-aarch64.so.1 (0x3)\n'),
                         {'/lib/aarch64-linux-gnu/libc.so.6', '/lib/ld-linux-aarch64.so.1'})

    def test_primary_survives_secondary_error(self):
        result = {}
        outer.primary(result, ValueError('FIRST')); outer.primary(result, OSError('LATER'))
        self.assertEqual(result['primaryError']['message'], 'FIRST')

    def test_atomic_failed_replace_keeps_original(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root/'result.json').write_bytes(b'old')
            with mock.patch.object(outer.os, 'replace', side_effect=OSError('fail')):
                with self.assertRaises(OSError): outer.persist(root, dict(primaryError='FIRST'))
            self.assertEqual((root/'result.json').read_bytes(), b'old')
            self.assertEqual(len(list(root.glob('.receipt-*'))), 1)

    def test_rescue_or_baseline_drift_cannot_pass(self):
        result = dict(fixtureExitCode=0, remainingNamespaceProcesses=[], workRemoved=True, baselineChanges=[], allInputHashesUnchanged=True)
        self.assertTrue(outer.accepted(result))
        for change in ({'fixtureExitCode': 1}, {'remainingNamespaceProcesses': [7]}, {'cleanupError': 'unknown'},
                       {'baselineChanges': ['readers']}, {'primaryError': {'message': 'FIRST'}}):
            self.assertFalse(outer.accepted(dict(result, **change)))

    def test_private_sweep_rescue_is_not_product_cleanup(self):
        fake = [Path('/proc/1'), Path('/proc/8')]
        with mock.patch.object(inner.os, 'getpid', return_value=1), mock.patch.object(inner.Path, 'iterdir', side_effect=[fake, [Path('/proc/1')]]), \
             mock.patch.object(inner.os, 'kill') as kill, mock.patch.object(inner.signal, 'SIGKILL', 9, create=True), \
             mock.patch.object(inner.os, 'WNOHANG', 1, create=True), \
             mock.patch.object(inner.os, 'waitpid', side_effect=ChildProcessError, create=True):
            result = inner.sweep()
        kill.assert_called_once_with(8, 9)
        self.assertFalse(result['productCleanupProved']); self.assertEqual(result['outerRescuedPids'], [8])

    def test_service_files_and_three_native_segments_fit(self):
        inner.check_budgets()
        budget = inner.CASE_BUDGETS['system_monitor_service_test']
        self.assertGreater(budget['fileBytes'], 5*1024**2+1)
        self.assertGreater(budget['fileBytes'], inner.V11_BYTES)
        self.assertEqual(budget['shmSegments']*inner.V11_BYTES, 47001096)
        self.assertLess(47001096+16*1024**2, 128*1024**2)
        self.assertLess(47001096+256*1024**2, budget['addressSpaceBytes'])

    def test_bad_file_or_shm_budget_rejected(self):
        original = inner.CASE_BUDGETS['system_monitor_service_test']
        for update in ({'fileBytes': 1024**2}, {'shmSegments': 10}, {'addressSpaceBytes': 32*1024**2}):
            with mock.patch.dict(inner.CASE_BUDGETS, {'system_monitor_service_test': dict(original, **update)}):
                with self.assertRaises(AssertionError): inner.check_budgets()

    def test_source_bound_service_budget_and_parent_hard_limit(self):
        source = entry.blob(entry.SOURCE_COMMIT, 'tools/system_monitor_service_test.cpp').decode()
        self.assertIn("std::string(5 * 1024 * 1024 + 1, 'x')", source)
        self.assertEqual(source.count('MemoryPointStore store(')+source.count('MemoryPointStore systemMonitorStore(')+source.count('MemoryPointStore lockedStore('), 3)
        self.assertEqual(entry.blob(entry.SOURCE_COMMIT, 'src/memory_point_store_layout.hpp'),
                         entry.blob('cb45f3fc78cd6288d9bb49f06f93ca07b441ecfa', 'src/memory_point_store_layout.hpp'))
        tree = ast.parse((HERE/'board-run.py').read_text())
        values = [node for node in ast.walk(tree) if isinstance(node, ast.Tuple) and len(node.elts) == 2
                  and isinstance(node.elts[0], ast.Attribute) and node.elts[0].attr == 'RLIMIT_FSIZE']
        self.assertEqual(len(values), 1)
        self.assertEqual(eval(compile(ast.Expression(values[0].elts[1]), '<budget>', 'eval')), 32*1024**2)

    def test_fixed_six_commands_no_automatic_24(self):
        self.assertEqual(len(inner.COMMANDS), 6)
        self.assertEqual(entry.HOST, '192.168.22.23')
        self.assertEqual([x[1] for x in inner.COMMANDS[-3:]], [['fd'], ['parent-death'], ['supervisor-death']])


if __name__ == '__main__': unittest.main(verbosity=2)
