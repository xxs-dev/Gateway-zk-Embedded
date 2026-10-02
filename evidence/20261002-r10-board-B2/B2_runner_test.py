"""B2 local admission and PID1 adoption tests; not Linux/board proof."""
import ast
import contextlib
import errno
import importlib.util
import io
import json
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


entry = load('r10_b2_entry', 'execute-b2.py')
pid1 = load('r10_b2_pid1', 'namespace-child.py')


class B2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files, cls.manifest, cls.base = entry.prepare()
        cls.frozen = json.loads((HERE / 'input-manifest-B2.json').read_bytes())

    def test_frozen_source_and_six_case_scope(self):
        self.assertEqual(self.manifest, self.frozen)
        self.assertEqual(self.manifest['sourceCommit'], entry.SOURCE_COMMIT)
        self.assertEqual(self.manifest['buildEvidenceCommit'], entry.BUILD_COMMIT)
        self.assertEqual(self.manifest['host'], '192.168.22.23')
        self.assertEqual(len(self.files), 10)
        self.assertEqual(len(self.manifest['cases']), 6)
        self.assertNotIn('SystemMonitor', self.files)
        self.assertFalse(self.manifest['heldSystemMonitor']['uploaded'])
        self.assertFalse(self.manifest['heldSystemMonitor']['executed'])
        self.assertFalse(self.manifest['board24ExecutionReady'])

    def test_fixed_launcher_and_echild_cleanup_are_pinned(self):
        outer = self.files['board-run.py'].decode()
        launcher = self.files['board-group.py'].decode()
        base = self.files['namespace-child-base.py'].decode()
        module = {'__name__': 'r10_b2_outer_contract'}
        exec(compile(outer, 'board-run.py', 'exec'), module)
        self.assertEqual(len(module['KEYS']), 16)
        self.assertIn("expected['parentNamespace']", outer)
        self.assertIn("result['afterCleanup']['parentNamespace']", outer)
        self.assertNotIn('/children', outer)
        self.assertIn('except ChildProcessError:', outer)
        self.assertIn('NSpid', outer)
        self.assertIn('initHostPid=child', launcher)
        self.assertIn('os.WNOWAIT', launcher)
        self.assertIn("('diagnostic_process_test', [], 30)", base)
        self.assertIn("('ownership-probe', ['supervisor-death'], 5)", base)
        self.assertIn(entry.ROOT.encode(), self.files['environment.py'])
        self.assertIn(entry.ROOT.encode(), self.files['namespace-child.py'])
        code = entry.remote_code(entry.baseline_code(self.base), self.manifest, {})
        ast.parse(code, feature_version=(3, 6))
        self.assertIn(self.manifest['remoteFiles']['board-run.py']['sha256'], code)

    def test_default_dry_run_has_no_authentication_or_connection(self):
        output = io.StringIO()
        with mock.patch.object(entry, 'prepare', return_value=(self.files, self.manifest, self.base)), \
             mock.patch.object(entry.importlib.util, 'spec_from_file_location', side_effect=AssertionError('authentication import')), \
             mock.patch.object(sys, 'argv', ['execute-b2.py']), contextlib.redirect_stdout(output):
            self.assertEqual(entry.main(), 0)
        plan = json.loads(output.getvalue())
        self.assertFalse(plan['connects'])
        self.assertFalse(plan['executionAuthorized'])
        self.assertFalse((HERE / 'board-23-B2-result.json').exists())

    def test_manifest_drift_or_bad_pin_rejects_before_authentication(self):
        changed = dict(self.manifest, root='/tmp/other')
        for argv, candidate, reason in ((['execute-b2.py'], changed, 'fixed B2 input drift'),
                                        (['execute-b2.py', '--manifest-sha256', '0' * 64], self.manifest, '')):
            with self.subTest(argv=argv), mock.patch.object(entry, 'prepare', return_value=(self.files, candidate, self.base)), \
                 mock.patch.object(entry.importlib.util, 'spec_from_file_location', side_effect=AssertionError('authentication import')), \
                 mock.patch.object(sys, 'argv', argv), self.assertRaises(AssertionError) as failure:
                entry.main()
            self.assertEqual(str(failure.exception), reason)
        self.assertFalse((HERE / 'board-23-B2-result.json').exists())

    def test_proc_children_preflight_is_read_only_and_bounded(self):
        namespace = {'Path': lambda path: types.SimpleNamespace(open=lambda mode: io.BytesIO(b'12 13\n'))}
        exec(entry.proc_children_code(), namespace)
        self.assertEqual(namespace['proc_children_capability'](), dict(available=True, bytes=6))
        def missing(mode):
            raise FileNotFoundError(errno.ENOENT, 'missing')
        namespace['Path'] = lambda path: types.SimpleNamespace(open=missing)
        self.assertEqual(namespace['proc_children_capability'](), dict(available=False, errno=errno.ENOENT))
        namespace['Path'] = lambda path: types.SimpleNamespace(open=lambda mode: io.BytesIO(b'x' * 4097))
        with self.assertRaises(AssertionError):
            namespace['proc_children_capability']()

    def test_natural_adopted_exit_is_recorded_without_rescue(self):
        with mock.patch.object(pid1.os, 'getpid', return_value=1), \
             mock.patch.object(pid1.os, 'readlink', return_value='pid:[private]'), \
             mock.patch.object(pid1.os, 'WNOHANG', 1, create=True), \
             mock.patch.object(pid1.os, 'WIFEXITED', return_value=True, create=True), \
             mock.patch.object(pid1.os, 'WEXITSTATUS', return_value=0, create=True), \
             mock.patch.object(pid1.os, 'waitpid', side_effect=[(42, 0), ChildProcessError(), ChildProcessError()], create=True), \
             mock.patch.object(pid1.Path, 'iterdir', side_effect=[[Path('/proc/1')], [Path('/proc/1')]]), \
             mock.patch.object(pid1.os, 'kill') as kill:
            result = pid1.sweep()
        self.assertEqual(result['naturalReaped'], [dict(pid=42, rawStatus=0, kind='exited', exitCode=0)])
        self.assertEqual(result['outerRescuedPids'], [])
        self.assertTrue(result['productCleanupProved'])
        kill.assert_not_called()

    def test_live_process_requires_rescue_and_fails_product_proof(self):
        status = b'Pid:\t8\nPPid:\t1\nNSpid:\t8\nState:\tS (sleeping)\n'
        with mock.patch.object(pid1.os, 'getpid', return_value=1), \
             mock.patch.object(pid1.os, 'readlink', return_value='pid:[private]'), \
             mock.patch.object(pid1.os, 'WNOHANG', 1, create=True), \
             mock.patch.object(pid1.time, 'monotonic', side_effect=[0, 4, 5, 9]), \
             mock.patch.object(pid1.Path, 'iterdir', side_effect=[[Path('/proc/1'), Path('/proc/8')], [Path('/proc/1')]]), \
             mock.patch.object(pid1.Path, 'read_bytes', return_value=status), \
             mock.patch.object(pid1.signal, 'SIGKILL', 9, create=True), \
             mock.patch.object(pid1.os, 'kill') as kill:
            result = pid1.sweep()
        self.assertEqual(result['naturalReaped'], [])
        self.assertEqual(result['outerRescuedPids'], [8])
        self.assertFalse(result['productCleanupProved'])
        kill.assert_called_once_with(8, 9)

    def test_unverified_process_identity_rejects_instead_of_ignoring(self):
        status = b'Pid:\t8\nPPid:\t1\nNSpid:\t9\nState:\tS (sleeping)\n'
        with mock.patch.object(pid1.os, 'getpid', return_value=1), \
             mock.patch.object(pid1.os, 'readlink', return_value='pid:[private]'), \
             mock.patch.object(pid1.time, 'monotonic', side_effect=[0, 4]), \
             mock.patch.object(pid1.Path, 'iterdir', return_value=[Path('/proc/1'), Path('/proc/8')]), \
             mock.patch.object(pid1.Path, 'read_bytes', return_value=status), \
             mock.patch.object(pid1.os, 'kill') as kill, self.assertRaises(AssertionError):
            pid1.sweep()
        kill.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
